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
 * 5.8 autostart, and the self-test hook (D18, handoff 7.2, mode `buffer`).
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

/* self-test watchdog thread (buffer mode) */
static HANDLE g_st_thread;
static volatile LONG g_st_stop;
static obs_source_t *g_st_media;

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

/* Watchdog: let the buffer run RR_SELFTEST_RUN seconds, then stop it, then
 * log "selftest finished" and keep OBS alive for the harness to close. */
static DWORD WINAPI selftest_worker(LPVOID arg)
{
	const uint64_t run_ns = (uint64_t)(uintptr_t)arg * 1000000000ull;
	const uint64_t start = os_gettime_ns();
	uint64_t next_stat = start;

	char reason[192] = "";
	bool ok = true;

	while (!g_st_stop && (os_gettime_ns() - start) < run_ns) {
		if (os_gettime_ns() >= next_stat) {
			struct rr_status st;
			rr_engine_get_status(&st);
			obs_log(LOG_INFO,
				"selftest: t=%.0fs state=%d buffered=%.1fs bytes=%lld total_frames=%u lagged=%u",
				(double)(os_gettime_ns() - start) / 1e9, st.state, st.buffered_sec,
				(long long)st.buffer_bytes, obs_get_total_frames(), obs_get_lagged_frames());
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

/* Called on the UI thread at FINISHED_LOADING when RR_SELFTEST is set. */
static void selftest_start(void)
{
	const char *mode = getenv("RR_SELFTEST");
	if (!mode || strcmp(mode, "buffer") != 0) {
		obs_log(LOG_WARNING, "selftest: RR_SELFTEST='%s' is not supported by this build (buffer only)",
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

	obs_log(LOG_INFO, "selftest: buffer mode window=%ds run=%ds dir=%s media=%s", window, run_sec, dir,
		media ? media : "(none)");

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
