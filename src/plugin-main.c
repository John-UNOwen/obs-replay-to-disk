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
 * 5.8 autostart, the hotkeys (B6, snippet 8.3), the UI bridge for the Qt
 * dialog/dock (plugin-ui.h), the global proc and saved signal (8.5), and the
 * self-test hook (D18, handoff 7.2, modes `buffer` and `save`).
 * All UI-thread work (obs_frontend_*, folder resolution) happens here before
 * rr_engine_start; the worker never calls obs_frontend_*. The self-test
 * watchdog thread likewise never touches libobs objects or the watcher: it
 * hands those operations to OBS_TASK_UI tasks and waits on events.
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
#include "plugin/ffmpeg-load.h"
#include "plugin/plugin-ui.h"
#include "plugin/settings.h"
#include "plugin/settings-source.h"

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

/* ------------------------------------------------------------------ hotkeys */

/* B6 / snippet 8.3: the names are stable keys OBS writes to the profile's
 * basic.ini [Hotkeys]; the display text comes from the locale. */
static obs_hotkey_id g_save_hotkey = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_toggle_hotkey = OBS_INVALID_HOTKEY_ID;

static void on_save_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		rr_engine_save();
}

/* Toggle: when off (or after an error, which returns the state to OFF),
 * start from a snapshot of the current settings whose folders are resolved
 * here on the UI thread (rr_plugin_start); otherwise stop. */
static void on_toggle_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;

	struct rr_status st;
	rr_engine_get_status(&st);
	if (st.state == RR_STATE_OFF)
		rr_plugin_start();
	else
		rr_plugin_stop();
}

/* Snippet 8.3 (adjusted): OBS saves hotkey bindings but does not load them for
 * plugins. Call on the UI thread at FINISHED_LOADING and PROFILE_CHANGED.
 * obs_hotkey_load is always called, with an empty array when the new profile
 * stores no bindings: it is the only libobs path that clears the previous
 * profile's live bindings (remove_bindings inside obs_hotkey_load,
 * libobs/obs-hotkey.c), so skipping it for a profile without an entry leaves
 * the old profile's hotkey firing while the new profile's dialog shows no
 * binding. */
static void load_hotkey(obs_hotkey_id id, const char *name)
{
	const char *json = config_get_string(obs_frontend_get_profile_config(), "Hotkeys", name);
	obs_data_t *data = json ? obs_data_create_from_json(json) : NULL;
	obs_data_array_t *bindings = data ? obs_data_get_array(data, "bindings") : NULL;
	if (!bindings)
		bindings = obs_data_array_create();
	obs_hotkey_load(id, bindings);
	obs_data_array_release(bindings);
	obs_data_release(data);
}

static void load_hotkeys(void)
{
	if (g_save_hotkey != OBS_INVALID_HOTKEY_ID)
		load_hotkey(g_save_hotkey, "ReplayToDisk.Save");
	if (g_toggle_hotkey != OBS_INVALID_HOTKEY_ID)
		load_hotkey(g_toggle_hotkey, "ReplayToDisk.Toggle");
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

/* UI thread only. Empty dirs mean "the OBS recording path" (5.8, D12): the
 * same defaults the dialog shows greyed, via one shared resolver. */
static void resolve_dirs(struct rr_config *cfg)
{
	char buf[RR_PATHLEN], out[RR_PATHLEN];
	rr_plugin_effective_dirs(cfg, buf, sizeof(buf), out, sizeof(out));
	snprintf(cfg->buffer_dir, sizeof(cfg->buffer_dir), "%s", buf);
	snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", out);
}

/* --------------------------------------------- start gate (owner 2026-11-15) */

/* The buffer never starts while OBS is recording: both write video files with
 * the same encoder. The refusal says where it is visible (the status dock),
 * not only in the log. UI thread. */
static bool recording_blocks_start(void)
{
	if (!obs_frontend_recording_active())
		return false;
	obs_log(LOG_WARNING, "engine: start refused, OBS is recording");
#ifdef RR_UI
	rr_status_dock_flash(obs_module_text("Start.RecordingBlocked"));
#endif
	return true;
}

static void start_from_settings(void)
{
	if (recording_blocks_start())
		return;
	struct rr_config cfg;
	if (g_start_cfg_valid) {
		cfg = g_start_cfg;
	} else {
		cfg = g_settings;
		resolve_dirs(&cfg);
		g_start_cfg = cfg; /* full snapshot: a profile switch restarts with the same settings and folders */
		g_start_cfg_valid = true;
	}
	/* The OBS settings sources are re-read at every start, so a change in
	 * OBS's Output dialog applies at the next buffer start (the snapshot
	 * above keeps folders and the rest stable across a profile switch). */
	rr_settings_resolve_source(&cfg);
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
	/* The run clock starts when the buffer reaches BUFFERING: encoder init
	 * on hardware takes seconds (wp1-findings), and RR_SELFTEST_SAVES
	 * should mean seconds into buffered video, not into OBS loading. */
	const uint64_t gate_deadline = os_gettime_ns() + 60ull * 1000000000ull;
	for (;;) {
		struct rr_status gate;
		rr_engine_get_status(&gate);
		if (gate.state == RR_STATE_BUFFERING || g_st_stop || os_gettime_ns() >= gate_deadline)
			break;
		os_sleep_ms(50);
	}

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

/* ------------------------------------------------- watch mode (selftest) */

/* Schedule (t = seconds since OBS finished loading, buffer starts via the
 * watcher, never via rr_engine_start here):
 *   3 s  spawn the foreground app                       -> rising-edge
 *        autostart + follow retarget expected
 *  12 s  check the buffer started; check the pinned capture source now
 *        points at the foreground app
 *  13 s  delete the follow source (release its last ref; the watcher pin is
 *        weak): the watcher must warn — throttled, not once and then silence
 *        forever — and keep retrying while the app is still foreground
 *  15 s  spawn the hidden app (a second listed app that never gets focus)  ->
 *        must NOT move the source and must NOT touch the running buffer
 *  16 s  re-add the source (re-created, answers the uuid the settings point
 *        at) -> the retry path must re-follow on its own, within a tick
 *  19 s  check the source points at the foreground app again (follow-retry)
 *  21 s  check hidden app stayed out and buffer still running, then terminate
 *        the foreground app (a real process exit: the watcher must never stop
 *        the buffer)
 *  28 s  check the buffer is still running
 *
 * Cleanup has no scheduled tick: the loop then runs until RR_SELFTEST_RUN
 * seconds (default 120), and only at loop exit does the worker terminate the
 * test apps and close their process handles, restore the live settings the run
 * injected (end-of-run
 * watch_ui_task, then rr_process_watch_refresh — the watcher must go back to
 * inert), release the pinned source, stop the engine and log the summary.
 *
 * The two spawned apps are configurable (RR_SELFTEST_WATCH_APP for the
 * foreground app, RR_SELFTEST_WATCH_HIDDEN for the windowless one) so the rig
 * can use private scratch exes no unrelated process on the machine can
 * collide with; the checks look for their base names.               */
struct watch_procs {
	PROCESS_INFORMATION app;    /* the foreground app (spawned at t=3) */
	PROCESS_INFORMATION hidden; /* the windowless app (spawned at t=15) */
	obs_source_t *src;          /* own ref, pinned into the watcher */
	bool ok_autostart;
	bool ok_follow;
	bool ok_follow_retry; /* re-followed on its own after delete + re-add */
	bool ok_background;
	bool ok_kept_running;
};
static struct watch_procs g_st_watch;

/* Live-settings snapshot taken in selftest_start right before watch mode
 * injects its test config into g_settings (g_settings = cfg). The run's last
 * UI task restores it; until then the watcher runs on the test values by
 * design. */
static struct rr_config g_settings_saved;
static bool g_settings_saved_valid;

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

static void lower_copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src);
	for (char *p = dst; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p + 32);
	}
}

static void base_lower(char *dst, size_t n, const char *path)
{
	const char *slash = strrchr(path, '\\');
	lower_copy(dst, n, slash ? slash + 1 : path);
}

/* The selftest never waits on or inspects the children's threads, so their
 * thread handles are dropped right where they are acquired; only hProcess
 * stays open, closed by the worker at loop exit after its final
 * TerminateProcess. Keeping them open would leak one kernel handle per child
 * for the whole OBS session. */
static void close_child_thread(PROCESS_INFORMATION *pi)
{
	if (pi->hThread) {
		CloseHandle(pi->hThread);
		pi->hThread = NULL;
	}
}

static void spawn_hidden(const wchar_t *exe, const wchar_t *args, PROCESS_INFORMATION *pi)
{
	STARTUPINFOW si = {sizeof(si)};
	ZeroMemory(pi, sizeof(*pi));
	wchar_t line[512];
	/* Quote the exe path: lpApplicationName makes CreateProcessW find the right exe,
	 * but the child builds its own argv from this command line, so an unquoted path
	 * with spaces would cut its argv[0] at the first space. */
	if (args && *args)
		swprintf(line, sizeof(line) / sizeof(line[0]), L"\"%s\" %s", exe, args);
	else
		swprintf(line, sizeof(line) / sizeof(line[0]), L"\"%s\"", exe);
	if (CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, pi)) {
		close_child_thread(pi);
		return;
	}
	/* A failed CreateProcessW leaves the struct undefined; zero it so the run-end
	 * NULL guards below can never act on a garbage handle value. */
	ZeroMemory(pi, sizeof(*pi));
	obs_log(LOG_WARNING, "selftest: spawn failed (err %lu)", GetLastError());
}

static void spawn_foreground(const wchar_t *exe, PROCESS_INFORMATION *pi)
{
	STARTUPINFOW si = {sizeof(si)};
	ZeroMemory(pi, sizeof(*pi));
	if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi)) {
		close_child_thread(pi);
		return;
	}
	ZeroMemory(pi, sizeof(*pi)); /* same: never leave an undefined handle behind */
	obs_log(LOG_WARNING, "selftest: spawn failed (err %lu)", GetLastError());
}

/* ------------------------------------------------------ watchdog ↔ UI handoff
 *
 * The watchdog thread must not call libobs or touch the watcher: both are
 * UI-thread state. The operations the watch and gate schedules need — reading
 * the pinned source's "window" setting, the final unpin + release, driving
 * OBS's own recording, and probing the OBS buttons — run as
 * obs_queue_task(OBS_TASK_UI, ...) tasks (OBSApp's ui_task_handler queues
 * them onto the Qt event loop, frontend/OBSApp.cpp), which is exactly where
 * the 2 s QTimer already mutates the same source (obs-data.c has no lock, so
 * a settings read here would race obs_source_update; a second thread in
 * rr_process_watch_pin_source could double-release the weak cache — its
 * release-then-null is not atomic). Each task ends with SetEvent and the
 * worker waits on that event bounded. If OBS is closing meanwhile the Qt
 * loop stops taking work, so the worker skips the task on g_st_stop and the
 * UI-thread handlers below release the source instead — SCRIPTING_SHUTDOWN
 * fires inside Shutdown() right before ClearSceneData, whose orphan check
 * enumerates every still-refcounted source (frontend/widgets/OBSBasic.cpp,
 * OBSBasic_SceneCollections.cpp). Exactly one task is in flight; the
 * schedule is strictly sequential. */
#define RR_WATCH_REQ_WINDOW 1 /* copy the source's window setting into g_watch_window */
#define RR_WATCH_REQ_RELEASE 2 /* restore the live settings, then unpin + release the pinned test source */
#define RR_WATCH_REQ_KILL 3   /* release the test source's strong ref, keeping the watcher pin */
#define RR_WATCH_REQ_READD 4  /* re-create the test source and pin it back under the uuid */
#define RR_GATE_REQ_REC_ON 5  /* obs_frontend_start_recording */
#define RR_GATE_REQ_REC_OFF 6 /* obs_frontend_stop_recording */
#define RR_GATE_REQ_REC_STATE \
	7 /* obs_frontend_recording_active into g_gate_rec_active */
#define RR_GATE_REQ_TRY_START 8 /* rr_plugin_start, the refusal case and the allowed case */
#define RR_GATE_REQ_PROBE 9    /* the three OBS buttons' enabled states into g_gate_probe */
#define RR_GATE_REQ_RESTORE 10 /* put the pre-test settings back */
/* Manual reset, created at selftest_start and never closed: a task the worker
 * abandoned can still SetEvent, and closing a handle a queued task holds
 * risks hitting a recycled one; process teardown reclaims it. */
static HANDLE g_watch_evt;
static volatile LONG g_watch_req;
static char g_watch_window[2048]; /* UI task writes, worker reads only after the event */
static bool g_gate_rec_active;    /* gate selftest: recording_active result, UI task writes */
static bool g_gate_probe[3];      /* gate selftest: stream/record/replay button enabled states */
static bool g_gate_probe_ok;      /* gate selftest: the probe found all three OBS widgets */

/* Release the pinned test source's strong ref and the watcher pin. UI thread
 * only and idempotent (NULL-guarded): the queued task, the frontend-event
 * handlers below and obs_module_unload all call it; whoever gets there first
 * wins, the later calls are no-ops. */
static void release_watch_src_ui(void)
{
	rr_process_watch_pin_source(NULL, NULL);
	if (g_st_watch.src) {
		obs_source_release(g_st_watch.src);
		g_st_watch.src = NULL;
	}
}

/* Undo the watch selftest's injection into the live settings (g_settings = cfg
 * in selftest_start): restore the whole pre-test snapshot, so nothing of the
 * test config outlives the run — not only the two toggles but the watch list,
 * the uuid of the released test source and the selftest folders too. Without
 * this the 2 s watcher keeps ticking on the test config until OBS exits: a
 * foreground switch to a listed app warns that the follow source (the released
 * test source) is gone, starting a listed exe autostarts a buffer writing into
 * the selftest directories, and the dialog shows test values. Idempotent via
 * the saved flag. UI thread only, like g_settings itself. */
static void restore_watch_settings_ui(void)
{
	if (!g_settings_saved_valid)
		return;
	g_settings = g_settings_saved;
	g_settings_saved_valid = false;
	obs_log(LOG_INFO, "selftest: watch live settings restored");
	rr_process_watch_refresh(); /* re-reads the restored (inert) settings: follow state, cache and baseline */
}

/* Gate mode has no watcher injection to undo, only the live settings (folder
 * paths) the run swapped in. UI thread, like g_settings itself. */
static void restore_gate_settings_ui(void)
{
	if (!g_settings_saved_valid)
		return;
	g_settings = g_settings_saved;
	g_settings_saved_valid = false;
	obs_log(LOG_INFO, "selftest: gate live settings restored");
}

static void watch_ui_task(void *param) /* runs on the UI thread */
{
	UNUSED_PARAMETER(param);
	switch (InterlockedCompareExchange(&g_watch_req, 0, 0)) {
	case RR_WATCH_REQ_RELEASE:
		/* Restore first: from here the watcher reads the live settings again and
		 * cannot warn about, or start anything for, the dying test state; then
		 * release the test objects. Both are idempotent, and this is the run-end
		 * task every schedule path (including the break-out early exits) queues. */
		restore_watch_settings_ui();
		release_watch_src_ui();
		break;
	case RR_WATCH_REQ_KILL:
		/* Emulate the user deleting the followed source while a listed
		 * app is foreground: the test's strong ref is the last one (the
		 * watcher holds only a weak pin), so the source really goes
		 * away. The watcher must warn repeatedly-but-throttled, keep
		 * retrying, and re-follow on its own once one answers again. */
		if (g_st_watch.src) {
			obs_source_release(g_st_watch.src);
			g_st_watch.src = NULL;
		}
		break;
	case RR_WATCH_REQ_READD:
		/* "Deleted, then re-added": a new source answers the uuid the
		 * settings point at (the pin keys the watcher's cache by that
		 * uuid), so the retry path re-follows on the next tick. */
		if (!g_st_watch.src && g_settings.watch_source_id[0]) {
			g_st_watch.src = obs_source_create("window_capture", "rr_watch_selftest_re", NULL, NULL);
			if (g_st_watch.src)
				rr_process_watch_pin_source(g_st_watch.src, g_settings.watch_source_id);
			else
				obs_log(LOG_WARNING, "selftest: watch follow-retry: could not re-create the source");
		}
		break;
	case RR_GATE_REQ_REC_ON:
		obs_frontend_recording_start();
		break;
	case RR_GATE_REQ_REC_OFF:
		obs_frontend_recording_stop();
		break;
	case RR_GATE_REQ_REC_STATE:
		g_gate_rec_active = obs_frontend_recording_active();
		break;
	case RR_GATE_REQ_TRY_START:
		rr_plugin_start();
		break;
	case RR_GATE_REQ_PROBE:
#ifdef RR_UI
		g_gate_probe_ok = rr_outputs_gate_debug_enabled(&g_gate_probe[0], &g_gate_probe[1], &g_gate_probe[2]);
#else
		g_gate_probe_ok = false;
#endif
		break;
	case RR_GATE_REQ_RESTORE:
		restore_gate_settings_ui();
		break;
	default: /* RR_WATCH_REQ_WINDOW */
		g_watch_window[0] = 0;
		if (g_st_watch.src) {
			obs_data_t *s = obs_source_get_settings(g_st_watch.src);
			const char *window = obs_data_get_string(s, "window");
			snprintf(g_watch_window, sizeof(g_watch_window), "%s", window ? window : "");
			obs_data_release(s);
		}
		break;
	}
	SetEvent(g_watch_evt);
}

/* Queue one task on the UI thread and wait for it bounded. False means the
 * worker must not read the result: OBS is closing (the UI handlers do the
 * release), or the UI never answered and the task may still run later — the
 * tasks are idempotent and NULL-guarded, so a later pass is harmless. */
static bool watch_run_ui_task(long req, DWORD timeout_ms)
{
	if (g_st_stop || !g_watch_evt)
		return false;
	ResetEvent(g_watch_evt);
	InterlockedExchange(&g_watch_req, req);
	obs_queue_task(OBS_TASK_UI, watch_ui_task, NULL, false);

	/* Bounded wait, polled: when OBS closes the Qt loop stops serving the
	 * task, and g_st_stop lets the watchdog leave now — the UI-thread
	 * handlers already did (or will do) the release. The abandoned task may
	 * still run later: it only touches UI-thread state idempotently. */
	const ULONGLONG deadline = GetTickCount64() + timeout_ms;
	for (;;) {
		if (WaitForSingleObject(g_watch_evt, 100) == WAIT_OBJECT_0)
			return true;
		if (g_st_stop || GetTickCount64() >= deadline)
			return false;
	}
}

static DWORD WINAPI selftest_watch_worker(LPVOID arg)
{
	const uint64_t start = os_gettime_ns();
	const double until = (double)(uintptr_t)arg;
	bool done[8] = {false, false, false, false, false, false, false, false};

	const char *app_path = env_or("RR_SELFTEST_WATCH_APP", "C:\\Windows\\System32\\notepad.exe");
	const char *hid_path = env_or("RR_SELFTEST_WATCH_HIDDEN", "C:\\Windows\\System32\\cmd.exe");
	wchar_t app_w[MAX_PATH], hid_w[MAX_PATH];
	os_utf8_to_wcs(app_path, 0, app_w, MAX_PATH);
	os_utf8_to_wcs(hid_path, 0, hid_w, MAX_PATH);
	char want_app[128], want_hid[128];
	base_lower(want_app, sizeof(want_app), app_path);
	base_lower(want_hid, sizeof(want_hid), hid_path);

	for (;;) {
		const double t = (double)(os_gettime_ns() - start) / 1e9;
		if (g_st_stop || t >= until)
			break;

		if (!done[0] && t >= 3.0) {
			done[0] = true;
			spawn_foreground(app_w, &g_st_watch.app);
			obs_log(LOG_INFO, "selftest: %s spawned (foreground app)", want_app);
		}
		if (!done[1] && t >= 12.0) {
			done[1] = true;
			struct rr_status st;
			rr_engine_get_status(&st);
			g_st_watch.ok_autostart = st.state != RR_STATE_OFF;
			obs_log(LOG_INFO, "selftest: watch check autostart %s (state=%d)",
				g_st_watch.ok_autostart ? "ok" : "FAIL", st.state);

			char lower[2048] = "";
			if (!watch_run_ui_task(RR_WATCH_REQ_WINDOW, 5000)) {
				obs_log(LOG_WARNING, "selftest: watch check follow: the UI thread did not answer");
				break; /* the run is over: OBS is closing or the UI is stuck */
			}
			lower_copy(lower, sizeof(lower), g_watch_window);
			g_st_watch.ok_follow = strstr(lower, want_app) != NULL;
			obs_log(LOG_INFO, "selftest: watch check follow %s (window='%s')",
				g_st_watch.ok_follow ? "ok" : "FAIL", lower);
		}
		if (!done[2] && t >= 13.0) {
			done[2] = true;
			if (!watch_run_ui_task(RR_WATCH_REQ_KILL, 5000)) {
				obs_log(LOG_WARNING, "selftest: watch follow-retry: the UI thread did not answer");
				break;
			}
			obs_log(LOG_INFO, "selftest: watch follow source deleted while the listed app is foreground");
		}
		if (!done[3] && t >= 15.0) {
			done[3] = true;
			/* the keepalive ping is cmd.exe's own syntax and only applies to the
			 * fallback; a configured scratch app stays alive on its own and must not
			 * be handed cmd's arguments */
			const wchar_t *hid_args = strcmp(want_hid, "cmd.exe") == 0 ? L"/c ping -n 40 127.0.0.1" : L"";
			spawn_hidden(hid_w, hid_args, &g_st_watch.hidden);
			obs_log(LOG_INFO, "selftest: %s spawned hidden (background listed app)", want_hid);
		}
		if (!done[4] && t >= 16.0) {
			done[4] = true;
			if (!watch_run_ui_task(RR_WATCH_REQ_READD, 5000)) {
				obs_log(LOG_WARNING, "selftest: watch follow-retry: the UI thread did not answer");
				break;
			}
			obs_log(LOG_INFO, "selftest: watch follow source re-added (same uuid) while still foreground");
		}
		if (!done[5] && t >= 19.0) {
			done[5] = true;
			char lower[2048] = "";
			if (!watch_run_ui_task(RR_WATCH_REQ_WINDOW, 5000)) {
				obs_log(LOG_WARNING,
					"selftest: watch check follow-retry: the UI thread did not answer");
				break;
			}
			lower_copy(lower, sizeof(lower), g_watch_window);
			g_st_watch.ok_follow_retry = strstr(lower, want_app) != NULL;
			obs_log(LOG_INFO, "selftest: watch check follow-retry %s (window='%s')",
				g_st_watch.ok_follow_retry ? "ok" : "FAIL", lower);
		}
		if (!done[6] && t >= 21.0) {
			done[6] = true;
			char lower[2048] = "";
			if (!watch_run_ui_task(RR_WATCH_REQ_WINDOW, 5000)) {
				obs_log(LOG_WARNING,
					"selftest: watch check background-app-stays-away: the UI thread did not "
					"answer");
				break;
			}
			lower_copy(lower, sizeof(lower), g_watch_window);
			struct rr_status st;
			rr_engine_get_status(&st);
			g_st_watch.ok_background = strstr(lower, want_app) != NULL && strstr(lower, want_hid) == NULL &&
						   st.state != RR_STATE_OFF;
			obs_log(LOG_INFO, "selftest: watch check background-app-stays-away %s (window='%s')",
				g_st_watch.ok_background ? "ok" : "FAIL", lower);
			if (g_st_watch.app.hProcess) {
				/* a real process exit (the notepad on this machine is UWP-hosted, and only
				 * the exit matters here: the watcher must never stop the buffer) */
				TerminateProcess(g_st_watch.app.hProcess, 0);
				obs_log(LOG_INFO, "selftest: %s terminated (process exit)", want_app);
			}
		}
		if (!done[7] && t >= 28.0) {
			done[7] = true;
			struct rr_status st;
			rr_engine_get_status(&st);
			g_st_watch.ok_kept_running = st.state != RR_STATE_OFF;
			obs_log(LOG_INFO, "selftest: watch check kept-running-after-exit %s (state=%d)",
				g_st_watch.ok_kept_running ? "ok" : "FAIL", st.state);
		}
		os_sleep_ms(100);
	}

	const bool ok = g_st_watch.ok_autostart && g_st_watch.ok_follow && g_st_watch.ok_follow_retry &&
			g_st_watch.ok_background && g_st_watch.ok_kept_running;
	obs_log(LOG_INFO,
		"selftest: watch summary autostart=%d follow=%d follow-retry=%d background-away=%d "
		"kept-running-after-exit=%d",
		g_st_watch.ok_autostart, g_st_watch.ok_follow, g_st_watch.ok_follow_retry, g_st_watch.ok_background,
		g_st_watch.ok_kept_running);

	/* Last use of the children's process handles: terminate, then close them
	 * (their thread handles were closed at spawn). NULL after closing so a
	 * repeated pass can never double-close. */
	if (g_st_watch.hidden.hProcess) {
		TerminateProcess(g_st_watch.hidden.hProcess, 0);
		CloseHandle(g_st_watch.hidden.hProcess);
		g_st_watch.hidden.hProcess = NULL;
	}
	if (g_st_watch.app.hProcess) {
		TerminateProcess(g_st_watch.app.hProcess, 0);
		CloseHandle(g_st_watch.app.hProcess);
		g_st_watch.app.hProcess = NULL;
	}

	/* The unpin + release belongs on the UI thread (watch_ui_task): a live
	 * timer tick may be inside obs_source_update on this source, and the
	 * watcher's weak-ref cache is UI-thread state. If OBS is closing, the
	 * handler in on_frontend_event does it (or has) instead. */
	if (!watch_run_ui_task(RR_WATCH_REQ_RELEASE, 8000))
		obs_log(LOG_WARNING, "selftest: watch release task not confirmed (OBS exiting?)");

	rr_engine_stop();
	rr_engine_wait_off(15000);
	obs_log(LOG_INFO,
		"selftest finished: %s watch mode (foreground app start+follow, delete/re-add re-follows, "
		"background app ignored, start-only honored)",
		ok ? "ok" : "failed");
	return 0;
}

/* RR_SELFTEST=gate schedule (t = seconds since OBS finished loading):
 *   3 s  start an OBS recording
 *   7 s  confirm the recording is active
 *   8 s  rr_plugin_start must refuse (state stays OFF while recording)
 *  11 s  stop the OBS recording
 *  14 s  rr_plugin_start must start the buffer
 *  17 s  buffer buffering, and all three OBS buttons disabled
 *  19 s  stop the buffer
 *  22 s  buffer off, and all three OBS buttons enabled again
 * The run is meaningless if OBS cannot start a recording (bad recording
 * path), and the logs show which check lost its footing. */
static DWORD WINAPI selftest_gate_worker(LPVOID arg)
{
	const uint64_t start = os_gettime_ns();
	const double until = (double)(uintptr_t)arg;
	bool done[8] = {false, false, false, false, false, false, false, false};
	bool ok_rec_start = false, ok_refused = false, ok_buf = false, ok_buttons_off = false, ok_buttons_on = false;

	for (;;) {
		const double t = (double)(os_gettime_ns() - start) / 1e9;
		if (g_st_stop || t >= until)
			break;

		if (!done[0] && t >= 3.0) {
			done[0] = true;
			if (!watch_run_ui_task(RR_GATE_REQ_REC_ON, 5000))
				break;
			obs_log(LOG_INFO, "selftest: gate OBS recording start requested");
		}
		if (!done[1] && t >= 7.0) {
			done[1] = true;
			if (!watch_run_ui_task(RR_GATE_REQ_REC_STATE, 5000))
				break;
			ok_rec_start = g_gate_rec_active;
			obs_log(LOG_INFO, "selftest: gate check obs-recording %s", ok_rec_start ? "ok" : "FAIL");
		}
		if (!done[2] && t >= 8.0) {
			done[2] = true;
			if (!watch_run_ui_task(RR_GATE_REQ_TRY_START, 5000))
				break;
		}
		if (!done[3] && t >= 9.5) {
			done[3] = true;
			struct rr_status st;
			rr_engine_get_status(&st);
			ok_refused = ok_rec_start && st.state == RR_STATE_OFF;
			obs_log(LOG_INFO, "selftest: gate check refused-while-recording %s (state=%d)",
				ok_refused ? "ok" : "FAIL", st.state);
		}
		if (!done[4] && t >= 11.0) {
			done[4] = true;
			if (!watch_run_ui_task(RR_GATE_REQ_REC_OFF, 5000))
				break;
			obs_log(LOG_INFO, "selftest: gate OBS recording stop requested");
		}
		if (!done[5] && t >= 14.0) {
			done[5] = true;
			if (!watch_run_ui_task(RR_GATE_REQ_TRY_START, 5000))
				break;
		}
		if (!done[6] && t >= 17.0) {
			done[6] = true;
			struct rr_status st;
			rr_engine_get_status(&st);
			ok_buf = st.state != RR_STATE_OFF;
			obs_log(LOG_INFO, "selftest: gate check buffer-started %s (state=%d)", ok_buf ? "ok" : "FAIL",
				st.state);
			if (!watch_run_ui_task(RR_GATE_REQ_PROBE, 5000))
				break;
			ok_buttons_off = g_gate_probe_ok && !g_gate_probe[0] && !g_gate_probe[1] && !g_gate_probe[2];
			obs_log(LOG_INFO,
				"selftest: gate check buttons-disabled %s (found=%d stream=%d record=%d replay=%d)",
				ok_buttons_off ? "ok" : "FAIL", g_gate_probe_ok, g_gate_probe[0], g_gate_probe[1],
				g_gate_probe[2]);
			rr_plugin_stop();
		}
		if (!done[7] && t >= 20.0) {
			done[7] = true;
			rr_engine_wait_off(15000);
			struct rr_status st;
			rr_engine_get_status(&st);
			/* Let one 1 s gate poll run: it re-enables the buttons once the
			 * engine reads OFF. */
			os_sleep_ms(1500);
			if (!watch_run_ui_task(RR_GATE_REQ_PROBE, 5000))
				break;
			ok_buttons_on = g_gate_probe_ok && g_gate_probe[0] && g_gate_probe[1] && g_gate_probe[2] &&
					st.state == RR_STATE_OFF;
			obs_log(LOG_INFO,
				"selftest: gate check buttons-re-enabled %s (found=%d stream=%d record=%d replay=%d state=%d)",
				ok_buttons_on ? "ok" : "FAIL", g_gate_probe_ok, g_gate_probe[0], g_gate_probe[1],
				g_gate_probe[2], st.state);
		}
		os_sleep_ms(100);
	}

	const bool ok = ok_rec_start && ok_refused && ok_buf && ok_buttons_off && ok_buttons_on;
	obs_log(LOG_INFO,
		"selftest: gate summary obs-recording=%d refused-while-recording=%d buffer-started=%d "
		"buttons-disabled=%d buttons-re-enabled=%d",
		ok_rec_start, ok_refused, ok_buf, ok_buttons_off, ok_buttons_on);

	if (!watch_run_ui_task(RR_GATE_REQ_RESTORE, 8000))
		obs_log(LOG_WARNING, "selftest: gate settings restore not confirmed (OBS exiting?)");

	rr_engine_stop();
	rr_engine_wait_off(15000);
	obs_log(LOG_INFO,
		"selftest finished: %s gate mode (start refused while OBS records, OBS buttons gated while "
		"buffer runs)",
		ok ? "ok" : "failed");
	return 0;
}

/* Called on the UI thread at FINISHED_LOADING when RR_SELFTEST is set. */
static void selftest_start(void)
{
	const char *mode = getenv("RR_SELFTEST");
	const bool save_mode = mode && strcmp(mode, "save") == 0;
	const bool watch_mode = mode && strcmp(mode, "watch") == 0;
	const bool gate_mode = mode && strcmp(mode, "gate") == 0;
	if (!mode || (strcmp(mode, "buffer") != 0 && !save_mode && !watch_mode && !gate_mode)) {
		obs_log(LOG_WARNING,
			"selftest: RR_SELFTEST='%s' is not supported by this build (buffer/save/watch/gate only)",
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

	if (watch_mode) {
#ifdef RR_UI
		/* The watcher (installed at module load) starts the buffer here; rr_engine_start
		 * is never called for this mode. Configuration goes into the plugin's live
		 * settings only (not settings.json) and into a pinned test capture source, so
		 * the test does not touch the user's scene graph. */
		const char *names = getenv("RR_SELFTEST_WATCH_NAMES");
		snprintf(cfg.watch_names, sizeof(cfg.watch_names), "%s",
			 names && *names ? names : "notepad.exe,cmd.exe");
		cfg.watch_autostart = true;
		cfg.watch_follow = true;

		g_st_watch.src = obs_source_create("window_capture", "rr_watch_selftest", NULL, NULL);
		if (!g_st_watch.src) {
			obs_log(LOG_INFO, "selftest finished: failed could not create the test capture source");
			return;
		}
		const char *uuid = obs_source_get_uuid(g_st_watch.src);
		snprintf(cfg.watch_source_id, sizeof(cfg.watch_source_id), "%s", uuid);
		snprintf(cfg.watch_source_name, sizeof(cfg.watch_source_name), "rr_watch_selftest");
		rr_process_watch_pin_source(g_st_watch.src, uuid);

		g_settings_saved = g_settings; /* the run-end RELEASE task (watch_ui_task) restores it */
		g_settings_saved_valid = true;
		g_settings = cfg; /* the watcher and rr_plugin_start read these */

		selftest_attach_media(media);
		obs_log(LOG_INFO, "selftest: watch mode window=%ds run=%ds names=%s dir=%s", window, run_sec,
			cfg.watch_names, dir);

		/* The UI handoff event exists before the watchdog starts; it is
		 * never closed — a task the worker abandoned can still SetEvent
		 * later, and process teardown reclaims the handle. */
		if (!g_watch_evt)
			g_watch_evt = CreateEventW(NULL, TRUE, FALSE, NULL); /* watch_ui_task signals it */
		if (!g_watch_evt) {
			obs_log(LOG_INFO, "selftest finished: failed could not create the UI handoff event");
			release_watch_src_ui(); /* UI thread here, and no watchdog runs */
			restore_watch_settings_ui();
			return;
		}

		g_st_thread =
			CreateThread(NULL, 0, selftest_watch_worker, (LPVOID)(uintptr_t)(unsigned)run_sec, 0, NULL);
		if (!g_st_thread) {
			obs_log(LOG_INFO, "selftest finished: failed could not create the watchdog thread");
			/* No watchdog to run the end-of-run task: undo the injection here. */
			release_watch_src_ui();
			restore_watch_settings_ui();
		}
#else
		obs_log(LOG_INFO, "selftest finished: failed watch mode needs the UI build");
#endif
		return;
	}

	if (gate_mode) {
#ifdef RR_UI
		/* The run drives OBS's own recording and the plugin's start path through
		 * rr_plugin_start (the guard lives there), so the live settings carry the
		 * selftest folders; the run-end RESTORE task puts the user's copy back. */
		g_settings_saved = g_settings;
		g_settings_saved_valid = true;
		g_settings = cfg;

		selftest_attach_media(media);
		obs_log(LOG_INFO, "selftest: gate mode window=%ds run=%ds dir=%s media=%s", window, run_sec, dir,
			media ? media : "(none)");

		if (!g_watch_evt)
			g_watch_evt = CreateEventW(NULL, TRUE, FALSE, NULL); /* watch_ui_task signals it */
		if (!g_watch_evt) {
			obs_log(LOG_INFO, "selftest finished: failed could not create the UI handoff event");
			restore_gate_settings_ui(); /* UI thread here, and no watchdog runs */
			return;
		}

		g_st_thread =
			CreateThread(NULL, 0, selftest_gate_worker, (LPVOID)(uintptr_t)(unsigned)run_sec, 0, NULL);
		if (!g_st_thread) {
			obs_log(LOG_INFO, "selftest finished: failed could not create the watchdog thread");
			restore_gate_settings_ui();
		}
#else
		obs_log(LOG_INFO, "selftest finished: failed gate mode needs the UI build");
#endif
		return;
	}

	const char *enc = getenv("RR_SELFTEST_ENCODER");
	if (enc && *enc)
		snprintf(cfg.video_encoder, sizeof(cfg.video_encoder), "%s", enc);

	/* RR_SELFTEST_SOURCE: arm a settings source (manual/streaming/recording).
	 * Resolution runs at the very end (UI thread), so it overrides the
	 * encoder/bitrate/tracks keys above exactly like it does in production. */
	const char *source = getenv("RR_SELFTEST_SOURCE");
	if (source && *source) {
		if (strcmp(source, "streaming") == 0)
			cfg.settings_source = RR_SOURCE_STREAMING;
		else if (strcmp(source, "recording") == 0)
			cfg.settings_source = RR_SOURCE_RECORDING;
		else
			cfg.settings_source = RR_SOURCE_MANUAL;
		obs_log(LOG_INFO, "selftest: settings source override: %s", source);
	}

	/* RR_SELFTEST_FILENAME: selftest-only filename_format override. Without
	 * it the selftest always uses the default timestamp format, whose name a
	 * harness cannot pre-create, so the suffix-chain proof (a known stem the
	 * save collides with) was not runnable on the rig. */
	const char *fname = getenv("RR_SELFTEST_FILENAME");
	if (fname && *fname) {
		snprintf(cfg.filename_format, sizeof(cfg.filename_format), "%s", fname);
		obs_log(LOG_INFO, "selftest: filename_format override: %s", cfg.filename_format);
	}

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

	rr_settings_resolve_source(&cfg); /* after every override: the source wins, like in production */

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
		load_hotkeys(); /* B6: reload the profile's bindings (8.3) */
		if (g_selftest) {
			selftest_start();
		} else if (g_settings.autostart) {
			obs_log(LOG_INFO, "engine: autostart is on, starting the buffer");
			start_from_settings();
		}
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		/* B8/D16: a profile switch resets video, and libobs only does
		 * that while no raw-video consumer is active — ours are
		 * released when the buffer reaches OFF. The engine escalates
		 * (stop, save cancel, force stop) within a bounded time; the
		 * old fixed 6 s let a save outlast it and OBS then silently
		 * kept the old video mode ("Tried to reset when already
		 * active"). */
		if (!rr_engine_halt_for_video_reset())
			obs_log(LOG_WARNING,
				"engine: the buffer did not reach OFF before a profile change: OBS may keep the "
				"current video settings for the new profile");
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		load_hotkeys(); /* the new profile carries its own bindings (B6) */
		if (g_settings.autostart) {
			obs_log(LOG_INFO, "engine: profile changed, autostarting the buffer");
			start_from_settings();
		}
		break;

	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* Drop every selftest-owned strong ref before OBS tears the
		 * scene data down: SCRIPTING_SHUTDOWN fires in Shutdown()
		 * right before ClearSceneData, whose orphan check flags every
		 * still-refcounted source (OBSBasic.cpp), and CHANGING/CLEANUP
		 * cover a collection switch mid-run. Both releases are
		 * idempotent NULL-guards, so no-op outside a selftest run. */
		release_watch_src_ui();
		selftest_detach_media();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		release_watch_src_ui();
		selftest_detach_media();
		InterlockedExchange(&g_st_stop, 1);
		rr_engine_shutdown();
		break;

	default:
		break;
	}
}

bool obs_module_load(void)
{
	/* FFmpeg is delay-loaded (see plugin/ffmpeg-load.h): bring the closure
	 * in from the plugin's own folder before the first FFmpeg call. */
	char fferr[160] = {0};
	if (!rr_ffmpeg_preload(fferr, sizeof(fferr))) {
		obs_log(LOG_ERROR, "FFmpeg preload failed: %s", fferr);
		return false;
	}

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

	/* B6 / 8.3: register the hotkeys at load (they appear in
	 * Settings -> Hotkeys); their bindings load at FINISHED_LOADING and
	 * PROFILE_CHANGED. Names are stable keys; display text is locale. */
	g_save_hotkey =
		obs_hotkey_register_frontend("ReplayToDisk.Save", obs_module_text("Hotkey.Save"), on_save_hotkey, NULL);
	g_toggle_hotkey = obs_hotkey_register_frontend("ReplayToDisk.Toggle", obs_module_text("Hotkey.Toggle"),
						       on_toggle_hotkey, NULL);

#ifdef RR_UI
	rr_settings_dialog_install(); /* Tools menu item -> the dialog (WP5) */
	rr_status_dock_install();     /* "replay_to_disk_dock" (WP5) */
	rr_process_watch_install();   /* 2 s follow/autostart timer (inert until enabled) */
	rr_outputs_gate_install();    /* disables OBS stream/record/replay buttons while the buffer runs */
#endif

	obs_frontend_add_event_callback(on_frontend_event, NULL);
	return true;
}

void obs_module_unload(void)
{
	/* UI thread, Qt loop gone: no OBS_TASK_UI runs after this point. The
	 * watchdog never touches libobs or the watcher, so dropping the
	 * watcher here (its cache is UI-thread state) while it may still be
	 * finishing is safe. */
#ifdef RR_UI
	rr_process_watch_uninstall();
	rr_settings_dialog_uninstall();
	rr_status_dock_uninstall();
	rr_outputs_gate_uninstall(); /* re-enables the OBS buttons it may have disabled */
#endif

	obs_frontend_remove_event_callback(on_frontend_event, NULL);

	/* The EXIT event stopped the engine already; this call re-runs the
	 * worker join, so no thread of ours runs once unload returns. */
	InterlockedExchange(&g_st_stop, 1);
	if (g_st_thread) {
		WaitForSingleObject(g_st_thread, 8000);
		g_st_thread = NULL;
	}
	rr_engine_shutdown();
	selftest_detach_media();
	/* Last fallback for the watch selftest's strong ref (an unload without
	 * the EXIT events, or a watchdog that left its release task behind):
	 * after this no libobs object of ours is alive. */
	release_watch_src_ui();

	obs_log(LOG_INFO, "plugin unloaded");
}

/* ------------------------------------------------------------- UI bridge
 * (plugin-ui.h): the Qt dialog/dock work through these, so the recording-path
 * resolver and the settings live in exactly one place, on the UI thread.
 */

void rr_plugin_get_settings(struct rr_config *out)
{
	if (out)
		*out = g_settings;
}

bool rr_plugin_set_settings(const struct rr_config *cfg)
{
	if (!cfg)
		return false;

	struct rr_config copy = *cfg;
	rr_settings_clamp(&copy);     /* clamp on set (5.8) */
	if (!rr_settings_save(&copy)) /* saves the clamped copy through settings.c */
		return false;

	g_settings = copy;
	g_start_cfg_valid = false; /* the next start re-resolves the folders from the new settings */
#ifdef RR_UI
	rr_process_watch_refresh(); /* the watcher re-reads these settings on its next tick */
#endif
	return true;
}

void rr_plugin_effective_dirs(const struct rr_config *cfg, char *buffer_dir, size_t buffer_sz, char *output_dir,
			      size_t output_sz)
{
	char rec[RR_PATHLEN];
	resolve_recording_dir(rec, sizeof(rec));

	if (output_dir) {
		output_dir[0] = 0;
		if (cfg && cfg->output_dir[0])
			snprintf(output_dir, output_sz, "%s", cfg->output_dir);
		else if (rec[0])
			snprintf(output_dir, output_sz, "%s", rec);
	}
	if (buffer_dir) {
		buffer_dir[0] = 0;
		if (cfg && cfg->buffer_dir[0])
			snprintf(buffer_dir, buffer_sz, "%s", cfg->buffer_dir);
		else if (rec[0])
			snprintf(buffer_dir, buffer_sz, "%s\\.rolling-replay-buffer", rec); /* D12 default */
	}
}

void rr_plugin_start(void)
{
	if (recording_blocks_start())
		return;
	/* Current settings snapshot, folders resolved here on the UI thread. */
	struct rr_config cfg = g_settings;
	char buf[RR_PATHLEN], out[RR_PATHLEN];
	rr_plugin_effective_dirs(&cfg, buf, sizeof(buf), out, sizeof(out));
	snprintf(cfg.buffer_dir, sizeof(cfg.buffer_dir), "%s", buf);
	snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", out);
	rr_settings_resolve_source(&cfg); /* OBS settings sources are read per start */
	rr_engine_start(&cfg);
}

void rr_plugin_stop(void)
{
	rr_engine_stop();
}

void rr_plugin_save(void)
{
	rr_engine_save();
}

bool rr_plugin_wait_off(uint32_t timeout_ms)
{
	return rr_engine_wait_off(timeout_ms);
}
