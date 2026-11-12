/*
Replay to Disk
Copyright (C) 2026 John-UNOwen

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* Module entry: engine lifetime against the frontend events (D16/B8), the
 * 5.8 autostart, the global proc and saved signal (8.5), and the self-test
 * hook (D18, handoff 7.2, modes `buffer` and `save`).
 * All UI-thread work (obs_frontend_*, folder resolution) happens here before
 * rr_engine_start; the worker never calls obs_frontend_*.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <libavformat/avformat.h>
#include <libavformat/version.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "plugin/engine.h"
#include "plugin/settings.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static struct rr_config g_settings; /* settings.json, loaded at module load */

/* The effective config autostart uses: the resolved g_settings, or the
 * self-test run's config while a self-test is armed (so a profile switch
 * restarts into the same folders). */
static struct rr_config g_start_cfg;
static bool g_start_cfg_valid;

static bool g_selftest; /* RR_SELFTEST is set */

/* self-test watchdog thread (buffer/save modes) */
static HANDLE g_st_thread;
static volatile LONG g_st_stop;
static obs_source_t *g_st_media;

/* save-mode save schedule (7.2 RR_SELFTEST_SAVES): seconds into the run at
 * which the watchdog enqueues a SAVE. */
#define RR_ST_MAX_SAVES 32
static int g_st_save_sec[RR_ST_MAX_SAVES];
static int g_st_save_count;
static bool g_st_save_fired[RR_ST_MAX_SAVES];

/* ------------------------------------------------ global proc and signal (8.5) */

static void on_save_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	rr_engine_save(); /* queue only; the worker decides per 5.3 */
}

/* ------------------------------------------------- recording-path resolution */

/* OBS 33 has no obs_frontend_get_recording_directory() (wp1-findings:
 * removed from the frontend API), so the recording folder comes from the
 * profile config. */
static void resolve_recording_dir(char *out, size_t out_sz)
{
	out[0] = 0;
	config_t *pc = obs_frontend_get_profile_config();
	if (pc) {
		const char *p = config_get_string(pc, "Output", "RecFilePath"); /* advanced mode */
		if (!p || !*p)
			p = config_get_string(pc, "SimpleOutput", "FilePath"); /* simple mode */
		if (p && *p)
			snprintf(out, out_sz, "%s", p);
	}
	if (!out[0]) { /* last fallback: the user's Videos folder */
		const char *up = getenv("USERPROFILE");
		if (up && *up)
			snprintf(out, out_sz, "%s\\Videos", up);
	}
}

/* UI thread only. Empty dirs mean "the OBS recording path" (5.8, D12). */
static void resolve_dirs(struct rr_config *cfg)
{
	char rec[RR_PATHLEN];
	resolve_recording_dir(rec, sizeof(rec));
	if (!cfg->output_dir[0] && rec[0])
		snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", rec);
	if (!cfg->buffer_dir[0] && rec[0])
		snprintf(cfg->buffer_dir, sizeof(cfg->buffer_dir), "%s\\.rolling-replay-buffer", rec);
}

static void start_from_settings(void)
{
	struct rr_config cfg;
	if (g_start_cfg_valid) {
		cfg = g_start_cfg;
	} else {
		cfg = g_settings;
		resolve_dirs(&cfg);
		snprintf(g_start_cfg.buffer_dir, sizeof(g_start_cfg.buffer_dir), "%s", cfg.buffer_dir);
		snprintf(g_start_cfg.output_dir, sizeof(g_start_cfg.output_dir), "%s", cfg.output_dir);
		g_start_cfg_valid = true; /* a profile switch restarts with the same folders */
	}
	rr_engine_start(&cfg);
}

/* ------------------------------------------------------------ self-test (7.2) */

static int env_int(const char *name, int dflt)
{
	const char *v = getenv(name);
	return (v && *v) ? atoi(v) : dflt;
}

static void selftest_attach_media(const char *media)
{
	if (!media || !*media)
		return;
	struct stat st;
	if (os_stat(media, &st) != 0) {
		obs_log(LOG_WARNING, "selftest: media file not found: %s", media);
		return;
	}
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "local_file", media);
	obs_data_set_bool(ss, "is_local_file", true);
	obs_data_set_bool(ss, "looping", true);
	g_st_media = obs_source_create("ffmpeg_source", "rr_selftest_media", ss, NULL);
	obs_data_release(ss);
	if (!g_st_media) {
		obs_log(LOG_WARNING, "selftest: could not create the media source");
		return;
	}
	obs_set_output_source(6, g_st_media); /* channel 6: test content, no scene touched */
	obs_log(LOG_INFO, "selftest: media on channel 6: %s", media);
}

static void selftest_detach_media(void)
{
	if (!g_st_media)
		return;
	obs_set_output_source(6, NULL);
	obs_source_release(g_st_media);
	g_st_media = NULL;
}

/* Watchdog: let the buffer run RR_SELFTEST_RUN seconds, firing any scheduled
 * SAVE at its time (save mode, 7.2), then stop the buffer, then log
 * "selftest finished" and keep OBS alive for the harness to close. */
static DWORD WINAPI selftest_worker(LPVOID arg)
{
	const uint64_t run_ns = (uint64_t)(uintptr_t)arg * 1000000000ull;
	const uint64_t start = os_gettime_ns();
	uint64_t next_stat = start;

	char reason[192] = "";
	bool ok = true;

	while (!g_st_stop && (os_gettime_ns() - start) < run_ns) {
		const double elapsed = (double)(os_gettime_ns() - start) / 1e9;

		for (int i = 0; i < g_st_save_count; i++) {
			if (!g_st_save_fired[i] && elapsed >= (double)g_st_save_sec[i]) {
				g_st_save_fired[i] = true;
				obs_log(LOG_INFO, "selftest: save trigger at t=%.0fs (scheduled %ds)", elapsed,
					g_st_save_sec[i]);
				rr_engine_save();
			}
		}

		if (os_gettime_ns() >= next_stat) {
			struct rr_status st;
			rr_engine_get_status(&st);
			obs_log(LOG_INFO,
				"selftest: t=%.0fs state=%d buffered=%.1fs bytes=%lld total_frames=%u lagged=%u",
				elapsed, st.state, st.buffered_sec, (long long)st.buffer_bytes, obs_get_total_frames(),
				obs_get_lagged_frames());
			next_stat += 10ull * 1000000000ull;
		}
		os_sleep_ms(100);
	}

	if (g_st_stop) {
		ok = false;
		snprintf(reason, sizeof(reason), "interrupted (OBS exit)");
	} else {
		rr_engine_stop();
		if (rr_engine_wait_off(15000)) {
			snprintf(reason, sizeof(reason), "buffer ran %us", (unsigned)(run_ns / 1000000000ull));
		} else {
			ok = false;
			snprintf(reason, sizeof(reason), "the buffer did not stop within 15 s");
		}
	}

	struct rr_status st;
	rr_engine_get_status(&st);
	obs_log(LOG_INFO, "selftest: final state=%d buffered=%.1fs bytes=%lld total_frames=%u lagged=%u", st.state,
		st.buffered_sec, (long long)st.buffer_bytes, obs_get_total_frames(), obs_get_lagged_frames());

	selftest_detach_media();

	obs_log(LOG_INFO, "selftest finished: %s %s", ok ? "ok" : "failed", reason);
	return 0;
}

/* RR_SELFTEST_SAVES: "20,60,72" — seconds into the run, saved into
 * g_st_save_sec. Returns the count (watchdog thread fires them). */
static int selftest_parse_saves(const char *list)
{
	int n = 0;
	const char *p = list;
	while (p && *p && n < RR_ST_MAX_SAVES) {
		g_st_save_sec[n++] = atoi(p);
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return n;
}

/* Called on the UI thread at FINISHED_LOADING when RR_SELFTEST is set. */
static void selftest_start(void)
{
	const char *mode = getenv("RR_SELFTEST");
	const bool save_mode = mode && strcmp(mode, "save") == 0;
	if (!mode || (strcmp(mode, "buffer") != 0 && !save_mode)) {
		obs_log(LOG_WARNING, "selftest: RR_SELFTEST='%s' is not supported by this build (buffer/save only)",
			mode ? mode : "");
		return;
	}

	const char *dir = getenv("RR_SELFTEST_DIR");
	const char *media = getenv("RR_SELFTEST_MEDIA");
	const int window = env_int("RR_SELFTEST_WINDOW", 30);
	const int run_sec = env_int("RR_SELFTEST_RUN", 120);

	if (!dir || !*dir) {
		obs_log(LOG_INFO, "selftest finished: failed RR_SELFTEST_DIR is not set");
		return;
	}

	struct rr_config cfg;
	rr_settings_default(&cfg);
	cfg.buffer_seconds = window;
	snprintf(cfg.buffer_dir, sizeof(cfg.buffer_dir), "%s\\buffer", dir); /* purge scope stays D12 */
	snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s\\out", dir);
	os_mkdirs(cfg.buffer_dir);
	os_mkdirs(cfg.output_dir);

	const char *enc = getenv("RR_SELFTEST_ENCODER");
	if (enc && *enc)
		snprintf(cfg.video_encoder, sizeof(cfg.video_encoder), "%s", enc);

	if (save_mode) {
		const char *scope = getenv("RR_SELFTEST_SCOPE");
		cfg.save_scope = (scope && strcmp(scope, "since_last_save") == 0) ? RR_SCOPE_SINCE_LAST_SAVE
										  : RR_SCOPE_WINDOW;

		/* Self-test-only overrides (WP4); production settings keep the
		 * 5.8 defaults. RR_SELFTEST_TRACKS is the 5.8 bit mask (3 =
		 * tracks 1+2), the bitrate keys are kbps. */
		cfg.audio_tracks = env_int("RR_SELFTEST_TRACKS", 1);
		cfg.audio_bitrate_kbps = env_int("RR_SELFTEST_AUDIO_BITRATE", 160);
		cfg.bitrate_kbps = env_int("RR_SELFTEST_BITRATE", 30000);

		const char *saves = getenv("RR_SELFTEST_SAVES");
		if (saves && *saves)
			g_st_save_count = selftest_parse_saves(saves);

		if (env_int("RR_SELFTEST_REFERENCE", 0) == 1)
			rr_engine_selftest_reference(cfg.output_dir); /* must precede rr_engine_start */

		obs_log(LOG_INFO, "selftest: save mode window=%ds run=%ds scope=%s saves=%d tracks=%d reference=%d",
			window, run_sec, cfg.save_scope == RR_SCOPE_SINCE_LAST_SAVE ? "since_last_save" : "window",
			g_st_save_count, cfg.audio_tracks, env_int("RR_SELFTEST_REFERENCE", 0));
	} else {
		obs_log(LOG_INFO, "selftest: buffer mode window=%ds run=%ds dir=%s media=%s", window, run_sec, dir,
			media ? media : "(none)");
	}

	g_start_cfg = cfg;
	g_start_cfg_valid = true; /* a profile switch restarts into the self-test folders */

	selftest_attach_media(media);
	rr_engine_start(&cfg);

	g_st_thread = CreateThread(NULL, 0, selftest_worker, (LPVOID)(uintptr_t)(unsigned)run_sec, 0, NULL);
	if (!g_st_thread)
		obs_log(LOG_INFO, "selftest finished: failed could not create the watchdog thread");
}

/* ------------------------------------------------------------ frontend glue */

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		if (g_selftest) {
			selftest_start();
		} else if (g_settings.autostart) {
			obs_log(LOG_INFO, "engine: autostart is on, starting the buffer");
			start_from_settings();
		}
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		/* B8/D16: a profile switch resets video; no output may be active. */
		rr_engine_stop();
		if (!rr_engine_wait_off(6000))
			obs_log(LOG_WARNING, "engine: the buffer did not reach OFF within 6 s before a profile change");
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (g_settings.autostart) {
			obs_log(LOG_INFO, "engine: profile changed, autostarting the buffer");
			start_from_settings();
		}
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		InterlockedExchange(&g_st_stop, 1);
		rr_engine_shutdown();
		break;

	default:
		break;
	}
}

bool obs_module_load(void)
{
	const unsigned ver = avformat_version();
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_log(LOG_INFO, "avformat %u.%u.%u", AV_VERSION_MAJOR(ver), AV_VERSION_MINOR(ver), AV_VERSION_MICRO(ver));

	rr_settings_load(&g_settings);
	rr_engine_init();

	/* 8.5: the global save proc and the saved signal live on OBS's global
	 * handlers. OBS 33 has no proc_handler_remove and no per-signal
	 * removal (only signal_handler_remove_current inside a callback), so
	 * they are added at load and stay registered until the process ends:
	 * the module is only unloaded at OBS exit, and the proc callback just
	 * enqueues (rr_engine_save is a no-op once the worker is gone). The
	 * duplicate-signal log below covers a plugin reload in one session. */
	proc_handler_add(obs_get_proc_handler(), "void rolling_replay_save()", on_save_proc, NULL);
	if (!signal_handler_add(obs_get_signal_handler(), "void rolling_replay_saved(string path)"))
		obs_log(LOG_WARNING, "signal rolling_replay_saved is already registered");

	const char *mode = getenv("RR_SELFTEST");
	g_selftest = (mode && *mode);
	if (g_selftest)
		obs_log(LOG_INFO, "selftest: RR_SELFTEST=%s armed", mode);

	obs_frontend_add_event_callback(on_frontend_event, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);

	/* The EXIT event stops the engine already; this is the bounded safety
	 * net so no thread of ours runs after the DLL unloads. */
	InterlockedExchange(&g_st_stop, 1);
	if (g_st_thread) {
		WaitForSingleObject(g_st_thread, 8000);
		g_st_thread = NULL;
	}
	rr_engine_shutdown();
	selftest_detach_media();

	obs_log(LOG_INFO, "plugin unloaded");
}
