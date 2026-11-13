/* engine.c — the buffer engine worker thread (handoff 5.3-5.7, D5-D8,
 * D10-D13). One worker thread owns the output, the encoders, the ledger and
 * the save (D5); every other thread (UI, encoder/signal threads) only pushes
 * commands into a mutex-guarded queue and signals os_event (D6). Times for
 * segments are captured inside the signal handlers (D7). "Segment closed" is
 * an exclusive-open test polled at 20 ms up to 2 s (D8). The save flow is 5.6:
 * split_file, window selection with pins, rr_remux to <final>.rrpart, rename
 * (D13), the rolling_replay_saved signal and the save sound.
 * License: GPL-2.0-or-later.
 */
#include <obs-module.h>
#include <plugin-support.h> /* obs_log (template helper) */
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "engine.h"
#include "platform-win.h"
#include "core/ledger.h"
#include "core/paths.h"
#include "core/space.h"
#include "core/window.h"
#include "save/remux.h"

#define RR_CMDQ_CAP 256
#define RR_MAX_AUDIO 6
#define RR_PENDING_DELETES 16
#define RR_PRUNE_MAX 64
#define RR_GIB ((int64_t)1 << 30)

#define SEGMENT_FORMAT "rrseg_%CCYY-%MM-%DD_%hh-%mm-%ss"
#define SEGMENT_SPLIT_SEC 10 /* D2 */

#define RR_MAX_SAVE_SEGS 512                /* > 1800 s / 10 s + headroom */
#define RR_SAVE_FREE_MARGIN (256 << 20)     /* 5.6 step 6 */
#define RR_SAVE_SPLIT_TIMEOUT_NS 3000000000ull /* 5.6 step 3: 3 s */
#define RR_SAVE_DEFER_CAP 32                /* STOPs parked while draining (5.3) */

/* B8/D16: the bounded phases of the halt before a video reset (a profile
 * switch, rr_engine_halt_for_video_reset). Phase 1 is a normal stop (up to
 * 5 s graceful + 2 s force in stop_session); a save defers STOP by design
 * (5.3) and can outlast any wait, so phase 2 cancels it (rr_remux polls the
 * cancel flag once per segment, so the abort is bounded by one segment) and
 * waits longer; phase 3 forces the stop on the worker. */
#define RR_RESET_STOP_WAIT_MS 6000u
#define RR_RESET_CANCEL_WAIT_MS 12000u
#define RR_RESET_FORCE_WAIT_MS 15000u

/* ------------------------------------------------------- commands and queue */

enum rr_cmd_type {
	CMD_START,
	CMD_STOP,
	CMD_SAVE,
	CMD_QUIT,
	CMD_BOUNDARY,       /* from file_changed (D6/D7) */
	CMD_OUTPUT_STOPPED, /* from the stop signal */
	CMD_FORCE_STOP,     /* a video reset forces the stop (B8/D16) */
};

struct rr_cmd {
	enum rr_cmd_type type;
	struct rr_config *cfg; /* CMD_START: copied snapshot, freed by the worker */
	char path[RR_PATHLEN]; /* CMD_BOUNDARY: next_file */
	uint64_t t_ns;         /* CMD_BOUNDARY: time taken in the handler (D7) */
	int code;              /* CMD_OUTPUT_STOPPED: signal code */
};

static CRITICAL_SECTION q_cs;
static struct rr_cmd q[RR_CMDQ_CAP];
static size_t q_head, q_tail, q_count; /* guarded by q_cs */
static os_event_t *q_wake;
static os_event_t *g_done_ev; /* MANUAL: the worker exited */

static CRITICAL_SECTION st_cs;
static struct rr_status g_status; /* the mutex-guarded snapshot (5.10) */

static HANDLE g_thread;
static volatile LONG g_quit;        /* QUIT requested (atomic) */
static volatile LONG g_save_cancel; /* a video reset cancels the running save (B8/D16) */
static bool g_inited, g_shutdown_done;

/* ------------------------------------------------------ worker-only state */

static struct {
	obs_output_t *out;
	obs_output_t *ref_out; /* self-test only: unsplit reference.mkv, same encoders (7.2) */
	obs_encoder_t *venc;
	obs_encoder_t *aenc[RR_MAX_AUDIO];
	size_t aenc_count;
	rr_ledger_t *ledger;
	struct rr_config cfg;
	char buffer_dir[RR_PATHLEN];
	char output_dir[RR_PATHLEN];
	int fps_num, fps_den;

	int state;           /* worker-owned mirror of the published state */
	bool stop_requested; /* the stop sequence is waiting for OUTPUT_STOPPED */
	bool stop_seen;      /* OUTPUT_STOPPED arrived */

	uint64_t last_save_end_ns; /* end of the last successful save (scope since_last_save) */

	char pending_del[RR_PENDING_DELETES][RR_PATHLEN]; /* failed prunes, retried on idle (5.4) */
	size_t pending_del_count;
} s;

static char prune_paths[RR_PRUNE_MAX][RR_PATHLEN]; /* prune collects paths here (worker-only) */

/* RR_SELFTEST_REFERENCE (7.2): full path of the unsplit reference output, or
 * empty. Written on the UI thread before rr_engine_start, read by the worker. */
static char g_ref_mkv[RR_PATHLEN];

/* Save selection scratch (worker-only): copied paths so the ledger may shift
 * while rr_remux runs, and the index range to unpin afterwards. */
static char save_paths[RR_MAX_SAVE_SEGS][RR_PATHLEN];
static const char *save_ptrs[RR_MAX_SAVE_SEGS];
static int save_idx[RR_MAX_SAVE_SEGS];

static void stop_session(bool force, bool stop_seen);
static bool wait_output_stopped(unsigned long timeout_ms);
static void handle_boundary(const char *path, uint64_t t_ns);
static void handle_save(void);
static void on_file_changed(void *data, calldata_t *cd);
static void on_output_stopped(void *data, calldata_t *cd);

/* ------------------------------------------------------------------ helpers */

static void enqueue_cmd(const struct rr_cmd *cmd)
{
	bool overflow = false;

	EnterCriticalSection(&q_cs);
	if (q_count < RR_CMDQ_CAP) {
		q[q_tail] = *cmd;
		q_tail = (q_tail + 1) % RR_CMDQ_CAP;
		q_count++;
	} else {
		overflow = true;
	}
	LeaveCriticalSection(&q_cs);

	if (overflow) {
		obs_log(LOG_WARNING, "engine: command queue full, dropped command %d", (int)cmd->type);
		if (cmd->type == CMD_START && cmd->cfg)
			free(cmd->cfg);
	}
	if (q_wake)
		os_event_signal(q_wake);
}

static bool dequeue_cmd(struct rr_cmd *cmd)
{
	bool have = false;

	EnterCriticalSection(&q_cs);
	if (q_count > 0) {
		*cmd = q[q_head];
		q_head = (q_head + 1) % RR_CMDQ_CAP;
		q_count--;
		have = true;
	}
	LeaveCriticalSection(&q_cs);
	return have;
}

static void publish_state(int state)
{
	s.state = state; /* worker-owned mirror */

	EnterCriticalSection(&st_cs);
	g_status.state = state;
	if (state == RR_STATE_STARTING)
		g_status.last_error[0] = 0; /* a new attempt clears the old error */
	if (state == RR_STATE_OFF) {
		g_status.buffered_sec = 0;
		g_status.buffer_bytes = 0;
	}
	LeaveCriticalSection(&st_cs);
}

static const char *state_name(int state)
{
	switch (state) {
	case RR_STATE_OFF:
		return "OFF";
	case RR_STATE_STARTING:
		return "STARTING";
	case RR_STATE_BUFFERING:
		return "BUFFERING";
	case RR_STATE_SAVING:
		return "SAVING";
	case RR_STATE_STOPPING:
		return "STOPPING";
	default:
		return "?";
	}
}

static void publish_status_values(void)
{
	EnterCriticalSection(&st_cs);
	if (s.ledger) {
		g_status.buffered_sec = rr_ledger_buffered_sec(s.ledger, os_gettime_ns());
		g_status.buffer_bytes = rr_ledger_bytes(s.ledger);
	} else {
		g_status.buffered_sec = 0;
		g_status.buffer_bytes = 0;
	}
	LeaveCriticalSection(&st_cs);
}

static void set_error(const char *text)
{
	EnterCriticalSection(&st_cs);
	snprintf(g_status.last_error, sizeof(g_status.last_error), "%s", text ? text : "");
	LeaveCriticalSection(&st_cs);
}

/* --------------------------------------------------------- folder helpers */

static bool same_drive(const char *a, const char *b)
{
	/* Conservative: same drive unless both are "X:" drive-qualified paths
	 * with different letters. UNC paths are treated as the same drive
	 * (the stricter 2x preflight). */
	if (a[0] && b[0] && a[1] == ':' && b[1] == ':')
		return tolower((unsigned char)a[0]) == tolower((unsigned char)b[0]);
	return true;
}

/* D12: delete only files named rrseg_*.mkv. per_file logs each deletion.
 * A file the muxer still holds (crash leftover racing shutdown) is retried
 * for ~2 s; failures are logged, never fatal. Returns the delete count. */
static int purge_buffer_dir(const char *dir, bool per_file)
{
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return 0;

	int deleted = 0;
	struct os_dirent *de;
	while ((de = os_readdir(d)) != NULL) {
		if (de->directory || !rr_paths_is_segment_name(de->d_name))
			continue;

		char path[RR_PATHLEN];
		snprintf(path, sizeof(path), "%s\\%s", dir, de->d_name);

		struct stat st;
		bool gone = false;
		for (int tries = 0; tries < 20; tries++) {
			if (os_unlink(path) == 0) {
				gone = true;
				break;
			}
			if (os_stat(path, &st) != 0) { /* vanished meanwhile */
				gone = true;
				break;
			}
			os_sleep_ms(100);
		}
		if (gone) {
			deleted++;
			if (per_file)
				obs_log(LOG_INFO, "engine: purged: %s", path);
		} else {
			obs_log(LOG_WARNING, "engine: could not delete %s", path);
		}
	}
	os_closedir(d);
	return deleted;
}

/* D13: a crash can leave <final>.rrpart in the output folder; stale ones are
 * deleted at start. Only names ending in .rrpart are touched. */
static void purge_part_files(const char *dir)
{
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return;

	struct os_dirent *de;
	while ((de = os_readdir(d)) != NULL) {
		if (de->directory || !rr_paths_is_part_name(de->d_name))
			continue;
		char path[RR_PATHLEN];
		snprintf(path, sizeof(path), "%s\\%s", dir, de->d_name);
		if (os_unlink(path) == 0)
			obs_log(LOG_INFO, "engine: removed stale part: %s", path);
	}
	os_closedir(d);
}

/* ------------------------------------------------------------ encoders etc. */

/* 5.8: an encoder is available when it is listed and not DEPRECATED/INTERNAL. */
static bool encoder_id_available(const char *id)
{
	const char *type;
	bool found = false;

	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (strcmp(type, id) == 0) {
			found = true;
			break;
		}
	}
	if (!found)
		return false;

	const uint32_t caps = obs_get_encoder_caps(id);
	return (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) == 0;
}

static void resolve_video_encoder(struct rr_config *cfg)
{
	if (cfg->video_encoder[0])
		return; /* explicit choice; a failure falls back to obs_x264 (5.5) */

	static const char *order[] = {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (encoder_id_available(order[i])) {
			snprintf(cfg->video_encoder, sizeof(cfg->video_encoder), "%s", order[i]);
			return;
		}
	}
	snprintf(cfg->video_encoder, sizeof(cfg->video_encoder), "%s", "obs_x264");
}

/* 5.5 step 4: start from obs_encoder_defaults(id) (pitfall 10), set only the
 * keys of 5.5, apply the extra JSON, then keyint_sec=1 last (D1). */
static obs_encoder_t *create_video_encoder(const char *id, const struct rr_config *cfg)
{
	obs_data_t *vs = obs_encoder_defaults(id);
	if (!vs) {
		obs_log(LOG_WARNING, "engine: no defaults for encoder %s", id);
		return NULL;
	}

	if (cfg->rate_control == RR_RC_CQP && strcmp(id, "obs_x264") == 0) {
		obs_data_set_string(vs, "rate_control", "CRF"); /* 5.8: crf for obs_x264 */
		obs_data_set_int(vs, "bitrate", cfg->bitrate_kbps);
		obs_data_set_int(vs, "crf", cfg->cq_level);
	} else if (cfg->rate_control == RR_RC_CQP) {
		obs_data_set_string(vs, "rate_control", "CQP");
		obs_data_set_int(vs, "bitrate", cfg->bitrate_kbps);
		obs_data_set_int(vs, "cqp", cfg->cq_level);
	} else {
		obs_data_set_string(vs, "rate_control", "CBR"); /* D14: no VBR */
		obs_data_set_int(vs, "bitrate", cfg->bitrate_kbps);
	}

	if (strcmp(cfg->extra_encoder_settings, "{}") != 0) {
		obs_data_t *extra = obs_data_create_from_json(cfg->extra_encoder_settings);
		if (extra) {
			obs_data_apply(vs, extra);
			obs_data_release(extra);
		}
	}

	obs_data_set_int(vs, "keyint_sec", 1); /* last (5.5) */

	obs_encoder_t *enc = obs_video_encoder_create(id, "rr_video", vs, NULL);
	obs_data_release(vs);
	if (!enc) {
		obs_log(LOG_WARNING, "engine: could not create video encoder %s", id);
		return NULL;
	}
	obs_encoder_set_video(enc, obs_get_video());
	return enc;
}

/* 5.5 step 5: one ffmpeg_aac per selected mixer track, ascending. */
static bool create_audio_encoders(const struct rr_config *cfg)
{
	size_t n = 0;
	for (int b = 0; b < 6; b++) {
		if (!(cfg->audio_tracks & (1 << b)))
			continue;

		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", cfg->audio_bitrate_kbps);

		char name[32];
		snprintf(name, sizeof(name), "rr_audio_%zu", n);
		s.aenc[n] = obs_audio_encoder_create("ffmpeg_aac", name, as, (size_t)b, NULL);
		obs_data_release(as);

		if (!s.aenc[n]) {
			obs_log(LOG_WARNING, "engine: could not create audio encoder for track %d", b + 1);
			s.aenc_count = n;
			return false;
		}
		obs_encoder_set_audio(s.aenc[n], obs_get_audio());
		n++;
	}
	s.aenc_count = n;
	return n > 0;
}

/* 5.5 steps 6-9: first file name, the D2 output settings, encoders, signals,
 * start. Returns true when the output is live. */
static bool create_output_and_start(const struct rr_config *cfg, char *first_path, size_t first_sz)
{
	char *fname = os_generate_formatted_filename("mkv", false, SEGMENT_FORMAT);
	if (!fname) {
		obs_log(LOG_WARNING, "engine: os_generate_formatted_filename failed");
		return false;
	}
	snprintf(first_path, first_sz, "%s\\%s", cfg->buffer_dir, fname);
	bfree(fname);

	obs_data_t *osettings = obs_data_create();
	obs_data_set_string(osettings, "path", first_path);
	obs_data_set_bool(osettings, "split_file", true);
	obs_data_set_int(osettings, "max_time_sec", SEGMENT_SPLIT_SEC);
	obs_data_set_int(osettings, "max_size_mb", 0);
	obs_data_set_bool(osettings, "allow_overwrite", false);
	obs_data_set_string(osettings, "directory", cfg->buffer_dir);
	obs_data_set_string(osettings, "format", SEGMENT_FORMAT);
	obs_data_set_string(osettings, "extension", "mkv");
	obs_data_set_bool(osettings, "allow_spaces", false);
	s.out = obs_output_create("ffmpeg_muxer", "rr_output", osettings, NULL);
	obs_data_release(osettings);
	if (!s.out) {
		obs_log(LOG_WARNING, "engine: could not create the ffmpeg_muxer output");
		return false;
	}

	obs_output_set_video_encoder(s.out, s.venc);
	for (size_t n = 0; n < s.aenc_count; n++)
		obs_output_set_audio_encoder(s.out, s.aenc[n], n);

	signal_handler_t *sh = obs_output_get_signal_handler(s.out);
	signal_handler_connect(sh, "file_changed", on_file_changed, NULL);
	signal_handler_connect(sh, "stop", on_output_stopped, NULL);

	/* Self-test only (7.2 RR_SELFTEST_REFERENCE): an unsplit reference.mkv
	 * on the same encoders, started before the split output so it holds
	 * every packet the split output sees (as in the WP1 spike). */
	if (g_ref_mkv[0]) {
		obs_data_t *rs = obs_data_create();
		obs_data_set_string(rs, "path", g_ref_mkv);
		s.ref_out = obs_output_create("ffmpeg_muxer", "rr_reference", rs, NULL);
		obs_data_release(rs);
		if (!s.ref_out) {
			obs_log(LOG_WARNING, "selftest: could not create the reference output");
		} else {
			obs_output_set_video_encoder(s.ref_out, s.venc);
			for (size_t n = 0; n < s.aenc_count; n++)
				obs_output_set_audio_encoder(s.ref_out, s.aenc[n], n);
			if (!obs_output_start(s.ref_out)) {
				obs_log(LOG_WARNING, "selftest: reference output start failed: %s",
					obs_output_get_last_error(s.ref_out));
				obs_output_release(s.ref_out);
				s.ref_out = NULL;
			} else {
				obs_log(LOG_INFO, "selftest: reference output: %s", g_ref_mkv);
			}
		}
	}

	if (!obs_output_start(s.out)) {
		obs_log(LOG_WARNING, "engine: output start failed: %s", obs_output_get_last_error(s.out));
		return false;
	}
	return true;
}

static void release_session_objects(void)
{
	if (s.ref_out) {
		/* self-test reference output: graceful stop so the MKV is
		 * finalized, bounded, then force as the last resort. */
		if (obs_output_active(s.ref_out)) {
			obs_output_stop(s.ref_out);
			for (int i = 0; i < 250 && obs_output_active(s.ref_out); i++)
				os_sleep_ms(20);
			if (obs_output_active(s.ref_out))
				obs_output_force_stop(s.ref_out);
		}
		obs_output_release(s.ref_out);
		s.ref_out = NULL;
	}
	if (s.out) {
		signal_handler_t *sh = obs_output_get_signal_handler(s.out);
		signal_handler_disconnect(sh, "file_changed", on_file_changed, NULL);
		signal_handler_disconnect(sh, "stop", on_output_stopped, NULL);
		obs_output_release(s.out);
		s.out = NULL;
	}
	for (size_t i = 0; i < RR_MAX_AUDIO; i++) {
		if (s.aenc[i]) {
			obs_encoder_release(s.aenc[i]);
			s.aenc[i] = NULL;
		}
	}
	s.aenc_count = 0;
	if (s.venc) {
		obs_encoder_release(s.venc);
		s.venc = NULL;
	}
}

/* ------------------------------------------------------------- start (5.5) */

static void fail_start(const char *msg)
{
	release_session_objects();
	if (s.ledger) {
		rr_ledger_destroy(s.ledger);
		s.ledger = NULL;
	}
	s.pending_del_count = 0;
	publish_state(RR_STATE_OFF);
	obs_log(LOG_INFO, "buffer error: %s", msg);
	set_error(msg);
}

static bool start_session(const struct rr_config *cfg_in)
{
	struct rr_config cfg = *cfg_in;
	rr_settings_clamp(&cfg);

	InterlockedExchange(&g_save_cancel, 0); /* a new session starts un-cancelled (B8/D16) */
	publish_state(RR_STATE_STARTING);

	char first_path[RR_PATHLEN] = "";
	const char *used_enc = cfg.video_encoder;
	bool started = false;

	/* 5.5 step 1: fps is pinned for the session (E10). */
	struct obs_video_info ovi = {0};
	if (!obs_get_video_info(&ovi)) {
		fail_start("could not read the video mode");
		return false;
	}
	s.fps_num = (int)ovi.fps_num;
	s.fps_den = (int)ovi.fps_den;

	/* 5.5 step 2: folders (core/paths), mkdirs, purges (D12, D13). */
	const enum rr_path_error perr = rr_paths_check_buffer_dir(cfg.buffer_dir, cfg.output_dir);
	if (perr != RR_PATH_OK) {
		char msg[192];
		switch (perr) {
		case RR_PATH_EMPTY:
			snprintf(msg, sizeof(msg), "buffer folder is empty (not configured)");
			break;
		case RR_PATH_DRIVE_ROOT:
			snprintf(msg, sizeof(msg), "buffer folder is a drive root");
			break;
		case RR_PATH_SAME_AS_OUTPUT:
			snprintf(msg, sizeof(msg), "buffer folder is the same as the output folder");
			break;
		default:
			snprintf(msg, sizeof(msg), "buffer folder is not an absolute path");
			break;
		}
		fail_start(msg);
		return false;
	}
	if (os_mkdirs(cfg.buffer_dir) == MKDIR_ERROR) {
		fail_start("could not create the buffer folder");
		return false;
	}
	purge_buffer_dir(cfg.buffer_dir, true); /* stale segments of a killed run */
	purge_part_files(cfg.output_dir);

	/* 5.5 step 3: preflight (core/space). */
	resolve_video_encoder(&cfg);
	const int total_kbps = rr_config_total_bitrate_kbps(&cfg);
	const int64_t estimate = rr_space_estimate_bytes(total_kbps, cfg.buffer_seconds);
	const uint64_t free_b = os_get_free_disk_space(cfg.buffer_dir);
	const uint64_t free_o = os_get_free_disk_space(cfg.output_dir);
	if (!rr_space_preflight(estimate, (int64_t)free_b, (int64_t)free_o,
				same_drive(cfg.buffer_dir, cfg.output_dir))) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "not enough free disk: window needs ~%lld MB, buffer drive has %llu MB, output drive %llu MB",
			 (long long)(estimate / (1024 * 1024)), (unsigned long long)(free_b / (1024 * 1024)),
			 (unsigned long long)(free_o / (1024 * 1024)));
		fail_start(msg);
		return false;
	}

	s.cfg = cfg;
	snprintf(s.buffer_dir, sizeof(s.buffer_dir), "%s", cfg.buffer_dir);
	snprintf(s.output_dir, sizeof(s.output_dir), "%s", cfg.output_dir);
	s.last_save_end_ns = 0; /* a new session has no previous save (5.6 step 4) */
	s.ledger = rr_ledger_create(cfg.buffer_seconds);
	if (!s.ledger) {
		fail_start("out of memory");
		return false;
	}

	/* 5.5 steps 4-9, with one retry on obs_x264 when the chosen encoder
	 * fails to create or to start (5.5 note). */
	for (int attempt = 0; attempt < 2 && !started; attempt++) {
		release_session_objects(); /* drop any objects of a failed attempt */
		if (attempt) {
			if (strcmp(used_enc, "obs_x264") == 0)
				break; /* the first attempt already was the fallback */
			obs_log(LOG_INFO, "engine: video encoder %s failed, retrying once with obs_x264", used_enc);
		}
		const char *enc_id = attempt ? "obs_x264" : cfg.video_encoder;
		used_enc = enc_id;

		s.venc = create_video_encoder(enc_id, &cfg);
		if (!s.venc)
			continue;
		if (!create_audio_encoders(&cfg))
			continue;
		started = create_output_and_start(&cfg, first_path, sizeof(first_path));
		if (!started)
			continue;
	}

	if (!started) {
		fail_start("could not start the buffer output");
		return false;
	}

	/* 5.5 step 10: seed the ledger now (after start returned: encoder init
	 * on hardware can take seconds, wp1-findings), state BUFFERING. */
	rr_ledger_seed(s.ledger, first_path, os_gettime_ns());
	publish_state(RR_STATE_BUFFERING);
	obs_log(LOG_INFO, "buffer started: encoder=%s fps=%d/%d tracks=%d window=%ds dir=%s", used_enc, s.fps_num,
		s.fps_den, rr_config_track_count(&cfg), cfg.buffer_seconds, s.buffer_dir);
	return true;
}

/* ---------------------------------------------------------------- stop (5.7) */

/* stop_seen: the "stop" signal for this stop has already arrived (the
 * unexpected-death path runs this sequence *because* of that signal, which
 * libobs emits exactly once). A fresh graceful stop starts with false and
 * waits for it. */
static void stop_session(bool force, bool stop_seen)
{
	if (!s.out && !s.ledger) {
		publish_state(RR_STATE_OFF);
		return;
	}

	publish_state(RR_STATE_STOPPING);
	s.stop_seen = stop_seen;
	s.stop_requested = true;

	if (s.out) {
		if (force) {
			/* QUIT: skip the graceful stop (5.7 note) */
			obs_output_force_stop(s.out);
		} else if (!obs_output_active(s.out)) {
			/* The output already died on its own. libobs's obs_output_stop
			 * returns early on an inactive output without emitting "stop"
			 * (obs-output.c:536-537), so waiting for the signal here could
			 * only ever burn the full timeout: nothing left to wait for. */
			s.stop_seen = true;
		} else {
			/* 5.7 step 1-2 */
			obs_output_stop(s.out);
			if (!wait_output_stopped(5000)) {
				obs_log(LOG_WARNING, "engine: graceful stop took over 5 s, forcing");
				obs_output_force_stop(s.out);
				if (!wait_output_stopped(2000))
					obs_log(LOG_WARNING, "engine: the output never reported its stop signal");
			}
		}
	}

	/* 5.7 step 3 */
	release_session_objects();

	/* 5.7 step 4: clear the ledger, purge (D12) */
	if (s.buffer_dir[0]) {
		const int n = purge_buffer_dir(s.buffer_dir, false);
		if (n > 0)
			obs_log(LOG_INFO, "engine: purged %d segment file(s) on stop", n);
	}
	if (s.ledger) {
		rr_ledger_destroy(s.ledger);
		s.ledger = NULL;
	}
	s.pending_del_count = 0;
	s.stop_requested = false;
	publish_state(RR_STATE_OFF);
	obs_log(LOG_INFO, "buffer stopped");
}

/* Drains the queue while waiting for the stop signal; boundaries keep being
 * handled so the ledger stays correct. Returns true when OUTPUT_STOPPED came. */
static bool wait_output_stopped(unsigned long timeout_ms)
{
	const uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ull;

	while (!s.stop_seen) {
		struct rr_cmd cmd;
		if (dequeue_cmd(&cmd)) {
			switch (cmd.type) {
			case CMD_BOUNDARY:
				handle_boundary(cmd.path, cmd.t_ns);
				break;
			case CMD_OUTPUT_STOPPED:
				s.stop_seen = true;
				break;
			case CMD_QUIT:
				InterlockedExchange(&g_quit, 1);
				if (s.out)
					obs_output_force_stop(s.out);
				break;
			case CMD_START:
				obs_log(LOG_INFO, "engine: START ignored (buffer stopping)");
				if (cmd.cfg)
					free(cmd.cfg);
				break;
			case CMD_FORCE_STOP:
				/* Already stopping: a video reset wants it done
				 * now, so skip what is left of the graceful wait. */
				if (s.out)
					obs_output_force_stop(s.out);
				break;
			default:
				break; /* STOP/SAVE: already stopping */
			}
		} else {
			os_sleep_ms(10);
		}
		if (!s.stop_seen && os_gettime_ns() >= deadline)
			break;
	}
	return s.stop_seen;
}

/* ------------------------------------------------------ ledger + pruning */

static void remember_pending_delete(const char *path)
{
	if (s.pending_del_count >= RR_PENDING_DELETES) {
		obs_log(LOG_WARNING, "engine: pending-delete list full, giving up on %s", path);
		if (s.ledger)
			rr_ledger_remove(s.ledger, path);
		return;
	}
	snprintf(s.pending_del[s.pending_del_count++], RR_PATHLEN, "%s", path);
}

/* D10: ask the ledger which closed, unpinned, past-window segments may go;
 * delete only those (never filtered by size, D9). Paths are collected first
 * because rr_ledger_remove shifts indices. */
static void prune(void)
{
	if (!s.ledger)
		return;

	/* Pruning pauses during a save (5.6); the pins (D10) stay the ledger's
	 * own protection even outside the pause. */
	if (s.state == RR_STATE_SAVING)
		return;

	int idx[RR_PRUNE_MAX];
	const size_t n = rr_ledger_prunable(s.ledger, os_gettime_ns(), idx, RR_PRUNE_MAX);

	size_t have = 0;
	for (size_t i = 0; i < n && have < RR_PRUNE_MAX; i++) {
		struct rr_segment_info si;
		if (!rr_ledger_get(s.ledger, (size_t)idx[i], &si) || !si.path)
			continue;
		snprintf(prune_paths[have++], RR_PATHLEN, "%s", si.path);
	}

	for (size_t i = 0; i < have; i++) {
		if (os_unlink(prune_paths[i]) == 0) {
			obs_log(LOG_INFO, "pruned: %s", prune_paths[i]);
			rr_ledger_remove(s.ledger, prune_paths[i]);
		} else {
			remember_pending_delete(prune_paths[i]);
		}
	}
}

static void retry_pending_deletes(void)
{
	for (size_t i = 0; i < s.pending_del_count;) {
		const char *path = s.pending_del[i];
		struct stat st;
		const bool gone = (os_unlink(path) == 0) || (os_stat(path, &st) != 0);
		if (gone) {
			obs_log(LOG_INFO, "pruned: %s", path);
			if (s.ledger)
				rr_ledger_remove(s.ledger, path);
			s.pending_del_count--;
			if (i < s.pending_del_count)
				snprintf(s.pending_del[i], RR_PATHLEN, "%s", s.pending_del[s.pending_del_count]);
			/* do not advance: the moved-in entry gets its turn too */
		} else {
			i++;
		}
	}
}

/* BOUNDARY (5.4): close the ledger at t_ns (D7), wait for the writer to
 * release the segment (D8); only a released segment is marked closed, sized
 * and logged (a held one is retried on idle ticks), then prune (D10). */
static void handle_boundary(const char *path, uint64_t t_ns)
{
	if (!s.ledger) {
		obs_log(LOG_WARNING, "engine: boundary without a session: %s", path);
		return;
	}

	const int idx = rr_ledger_boundary(s.ledger, path, t_ns);
	if (idx < 0) {
		obs_log(LOG_WARNING, "engine: boundary with no open segment: %s", path);
		return;
	}

	struct rr_segment_info si;
	if (!rr_ledger_get(s.ledger, (size_t)idx, &si) || !si.path)
		return;
	const double dur = (double)(t_ns - si.start_ns) / 1e9;
	const char *seg = si.path;

	/* D8: 20 ms poll up to 2 s until the exclusive open succeeds */
	const uint64_t cw0 = os_gettime_ns();
	bool closed = false;
	for (;;) {
		if (rr_file_closed(seg)) {
			closed = true;
			break;
		}
		if (InterlockedCompareExchange(&g_quit, 0, 0) != 0)
			break;
		if (os_gettime_ns() - cw0 > 2000000000ull)
			break;
		os_sleep_ms(20);
	}
	if (!closed) {
		/* The muxer (or a scanner on the file) still holds it. The segment
		 * stays NOT closed: save selection and prune skip it, so an MKV
		 * being written never reaches rr_remux (a save would read only the
		 * prefix valid at that moment and log as a clean success — silent
		 * tail loss). The idle-tick probe in retry_segment_releases()
		 * closes it once the release really happens. */
		obs_log(LOG_WARNING, "engine: segment not released within 2 s: %s", seg);
	} else {
		int64_t bytes = -1;
		struct stat st;
		if (os_stat(seg, &st) == 0)
			bytes = (int64_t)st.st_size;

		rr_ledger_mark_closed(s.ledger, idx, bytes);
		obs_log(LOG_INFO, "segment closed: %s dur=%.2fs bytes=%lld", seg, dur, (long long)bytes);
	}

	prune();
}

/* OUTPUT_STOPPED while BUFFERING/SAVING and nobody asked to stop (5.3):
 * run the stop sequence, set the error text from the output. */
static void handle_unexpected_stopped(int code)
{
	if (!s.out) {
		obs_log(LOG_INFO, "engine: OUTPUT_STOPPED ignored (no active output)");
		return;
	}

	const char *err = obs_output_get_last_error(s.out);
	char msg[224];
	if (err && *err)
		snprintf(msg, sizeof(msg), "%s", err);
	else
		snprintf(msg, sizeof(msg), "the output stopped unexpectedly (code %d)", code);

	obs_log(LOG_INFO, "buffer error: %s", msg);
	set_error(msg);
	/* This queued OUTPUT_STOPPED is the "stop" signal for the sequence we
	 * are about to run; libobs will not emit another one. */
	stop_session(false, true);
}

/* ------------------------------------------------------- save flow (5.6) */

static int save_cancel(void *ud)
{
	(void)ud;
	/* QUIT cancels (5.3); so does a video reset (B8/D16) without quitting:
	 * the halt sets the flag, rr_remux checks it per segment. */
	return InterlockedCompareExchange(&g_quit, 0, 0) != 0 || InterlockedCompareExchange(&g_save_cancel, 0, 0) != 0;
}

static size_t queue_count(void)
{
	EnterCriticalSection(&q_cs);
	const size_t n = q_count;
	LeaveCriticalSection(&q_cs);
	return n;
}

static uint64_t last_closed_end(void)
{
	uint64_t end = 0;
	const size_t n = rr_ledger_count(s.ledger);
	for (size_t i = 0; i < n; i++) {
		struct rr_segment_info seg;
		if (!rr_ledger_get(s.ledger, i, &seg))
			continue;
		if (seg.closed && seg.end_ns > end)
			end = seg.end_ns;
	}
	return end;
}

/* Commands that arrive while SAVING are resolved now, not by the main loop:
 * a second SAVE is ignored with a log line (5.3, "accepted only in
 * BUFFERING"); STOP stays queued and runs when the save ends (5.3); other
 * commands go back to the queue tail in order. Call while state == SAVING. */
static void sweep_save_queue(void)
{
	size_t n = queue_count();
	for (size_t i = 0; i < n; i++) {
		struct rr_cmd cmd;
		if (!dequeue_cmd(&cmd))
			break;
		switch (cmd.type) {
		case CMD_SAVE:
			obs_log(LOG_INFO, "engine: SAVE ignored (saving)");
			break;
		case CMD_START:
			obs_log(LOG_INFO, "engine: START ignored (saving)");
			if (cmd.cfg)
				free(cmd.cfg);
			break;
		case CMD_QUIT:
			InterlockedExchange(&g_quit, 1);
			break;
		default: /* BOUNDARY / STOP / FORCE_STOP / OUTPUT_STOPPED: back to the queue */
			enqueue_cmd(&cmd);
			break;
		}
	}
}

/* 5.6 step 3: keep reading the queue, handling BOUNDARYs normally, until one
 * arrives with t >= t_req, or 3 s pass (then a warning; the caller continues
 * with the closed segments). STOP is parked in a local list OUTSIDE the queue
 * (deferred to run when the save ends, 5.3): putting it back into the queue
 * keeps q_count >= 1 forever, so the loop never sleeps and the 3 s timeout
 * branch — which only runs when the dequeue comes up empty — is unreachable.
 * With the STOP parked, the queue drains empty, the loop sleeps on the queue
 * event, and the timeout fires on a stalled muxer. */
static void save_drain(uint64_t t_req, uint64_t *window_end, bool *stopped)
{
	const uint64_t deadline = t_req + RR_SAVE_SPLIT_TIMEOUT_NS;
	struct rr_cmd deferred[RR_SAVE_DEFER_CAP]; /* STOPs parked out of the queue (5.3) */
	size_t deferred_count = 0;
	bool finished = false;

	while (!finished) {
		struct rr_cmd cmd;
		if (dequeue_cmd(&cmd)) {
			switch (cmd.type) {
			case CMD_BOUNDARY:
				handle_boundary(cmd.path, cmd.t_ns);
				if (cmd.t_ns >= t_req) {
					*window_end = cmd.t_ns;
					finished = true;
				}
				break;
			case CMD_SAVE:
				obs_log(LOG_INFO, "engine: SAVE ignored (saving)");
				break;
			case CMD_STOP:
			case CMD_FORCE_STOP:
				/* Parked: runs when the save ends. A STOP past the
				 * parking capacity is redundant (one stop is enough). */
				if (deferred_count < RR_SAVE_DEFER_CAP)
					deferred[deferred_count++] = cmd;
				break;
			case CMD_START:
				obs_log(LOG_INFO, "engine: START ignored (saving)");
				if (cmd.cfg)
					free(cmd.cfg);
				break;
			case CMD_QUIT:
				InterlockedExchange(&g_quit, 1);
				deferred_count = 0; /* QUIT force-stops: no STOP to re-queue */
				finished = true;
				break;
			case CMD_OUTPUT_STOPPED:
				handle_unexpected_stopped(cmd.code); /* 5.3 */
				*stopped = true;
				deferred_count = 0; /* the stop sequence already ran */
				finished = true;
				break;
			}
		} else if (os_gettime_ns() >= deadline) {
			obs_log(LOG_WARNING, "engine: save: no boundary within 3 s, continuing with closed segments");
			finished = true;
		} else if (q_wake) {
			os_event_timedwait(q_wake, 100);
		} else {
			os_sleep_ms(10);
		}
	}

	/* Hand the deferred STOPs back to the queue tail: they run when the
	 * save ends (5.3), after the caller finishes the remux. */
	for (size_t i = 0; i < deferred_count; i++)
		enqueue_cmd(&deferred[i]);
}

/* 5.6 step 7: final path from the filename format, with _2/_3/… suffixes
 * while the plain name exists (D13's rename target). */
static bool build_final_path(const char *out_dir, const char *fmt, char *final_path, size_t sz)
{
	char *fname = os_generate_formatted_filename("mp4", false, fmt);
	if (!fname)
		return false;
	snprintf(final_path, sz, "%s\\%s", out_dir, fname);
	bfree(fname);

	struct stat st;
	if (os_stat(final_path, &st) != 0)
		return true;

	char stem[RR_PATHLEN];
	snprintf(stem, sizeof(stem), "%s", final_path);
	char *dot = strrchr(stem, '.');
	if (dot && strcmp(dot, ".mp4") == 0)
		*dot = 0;
	for (int n = 2; n < 1000; n++) {
		char cand[RR_PATHLEN + 8];
		snprintf(cand, sizeof(cand), "%s_%d.mp4", stem, n);
		if (os_stat(cand, &st) != 0) {
			snprintf(final_path, sz, "%s", cand);
			return true;
		}
	}
	return false;
}

static void handle_save(void)
{
	if (s.state != RR_STATE_BUFFERING) {
		obs_log(LOG_INFO, "engine: SAVE ignored (state %s)", state_name(s.state));
		return;
	}
	if (!s.out || !s.ledger) {
		obs_log(LOG_INFO, "engine: SAVE ignored (buffer not running)");
		return;
	}
	if (InterlockedCompareExchange(&g_quit, 0, 0) != 0) {
		obs_log(LOG_INFO, "engine: SAVE ignored (shutting down)");
		return;
	}

	/* 5.6 step 1 */
	const uint64_t t_req = os_gettime_ns();
	publish_state(RR_STATE_SAVING);

	/* Step 2: force a split at the next keyframe (snippet 8.2) */
	calldata_t cd = {0};
	const bool proc_ok = proc_handler_call(obs_output_get_proc_handler(s.out), "split_file", &cd);
	const bool split_enabled = calldata_bool(&cd, "split_file_enabled");
	calldata_free(&cd);
	if (!proc_ok || !split_enabled)
		obs_log(LOG_WARNING, "engine: save: the split_file proc did not take effect (ok=%d enabled=%d)",
			(int)proc_ok, (int)split_enabled);

	/* Step 3: boundaries until t >= t_req or 3 s */
	uint64_t window_end = 0;
	bool stopped = false;
	save_drain(t_req, &window_end, &stopped);
	if (stopped)
		return; /* the stop sequence ran; the save is done (state OFF) */

	if (window_end == 0)
		window_end = last_closed_end(); /* timeout path (5.6 step 4) */

	int sel_count = 0;
	bool pinned = false;
	bool ok = false;
	bool counted_fail = false; /* "save failed:" already logged */
	char rerr[256] = "";
	struct rr_remux_result res = {0};
	char final_path[RR_PATHLEN] = "";

	/* Steps 4-5: window, selection, pins */
	if (window_end == 0) {
		counted_fail = true;
		rerr[0] = 0;
	} else {
		const uint64_t lse = s.cfg.save_scope == RR_SCOPE_SINCE_LAST_SAVE ? s.last_save_end_ns : 0;
		struct rr_window w;
		if (!rr_window_select(s.ledger, window_end, s.cfg.buffer_seconds, lse, &w)) {
			counted_fail = true;
			rerr[0] = 0;
		} else if (w.count > RR_MAX_SAVE_SEGS) {
			snprintf(rerr, sizeof(rerr), "%d segments exceed the save limit of %d", w.count,
				 RR_MAX_SAVE_SEGS);
		} else {
			int64_t total_bytes = 0;
			bool vanished = false;
			for (int i = 0; i < w.count; i++) {
				save_idx[i] = w.first + i;
				struct rr_segment_info si;
				if (!rr_ledger_get(s.ledger, (size_t)save_idx[i], &si) || !si.path) {
					vanished = true;
					break;
				}
				snprintf(save_paths[i], RR_PATHLEN, "%s", si.path);
				save_ptrs[i] = save_paths[i];
				if (si.bytes > 0)
					total_bytes += si.bytes;
			}
			if (vanished) {
				snprintf(rerr, sizeof(rerr), "a selected segment vanished from the ledger");
			} else {
				rr_ledger_pin(s.ledger, save_idx, (size_t)w.count);
				pinned = true;
				sel_count = w.count;

				obs_log(LOG_INFO, "save started: segments=%d inpoint=%.2fs", w.count, w.inpoint_sec);

				/* Step 6: output drive room for the new file */
				const uint64_t need = (uint64_t)total_bytes + RR_SAVE_FREE_MARGIN;
				if (os_get_free_disk_space(s.output_dir) < need) {
					snprintf(rerr, sizeof(rerr), "the output drive lacks room for %lld MB",
						 (long long)(need / (1024 * 1024)));
				} else if (!build_final_path(s.output_dir, s.cfg.filename_format, final_path,
							     sizeof(final_path))) {
					snprintf(rerr, sizeof(rerr), "could not build an output file name");
				} else {
					/* Step 7: rr_remux to <final>.rrpart (D13);
					 * cancel returns the quit flag (5.3). */
					char part_path[RR_PATHLEN + 16];
					snprintf(part_path, sizeof(part_path), "%s.rrpart", final_path);

					struct rr_remux_params p = {0};
					p.segments = save_ptrs;
					p.num_segments = w.count;
					p.out_path = part_path;
					p.fps_num = s.fps_num;
					p.fps_den = s.fps_den;
					p.inpoint_sec = w.inpoint_sec;
					p.cancel = save_cancel;

					const uint64_t t0 = os_gettime_ns();
					const int rr_ret = rr_remux(&p, &res, rerr, sizeof(rerr));
					const double took_s = (double)(os_gettime_ns() - t0) / 1e9;

					ok = (rr_ret == 0);
					if (ok && os_rename(part_path, final_path) != 0) {
						ok = false;
						snprintf(rerr, sizeof(rerr), "could not rename the part file");
					}

					if (ok) {
						/* Step 8: success bookkeeping */
						s.last_save_end_ns = window_end;

						EnterCriticalSection(&st_cs);
						snprintf(g_status.last_saved, sizeof(g_status.last_saved), "%s",
							 final_path);
						LeaveCriticalSection(&st_cs);

						calldata_t scd = {0}; /* 8.5 */
						calldata_set_string(&scd, "path", final_path);
						signal_handler_signal(obs_get_signal_handler(), "rolling_replay_saved",
								      &scd);
						calldata_free(&scd);

						if (s.cfg.save_sound)
							rr_play_save_sound(true);

						obs_log(LOG_INFO,
							"save finished: %s frames=%lld duration=%.2fs took=%.2fs warnings=%d",
							final_path, (long long)res.video_frames, res.duration_sec,
							took_s, res.grid_warnings);
						if (res.grid_warnings > 0)
							obs_log(LOG_WARNING,
								"engine: save: %d packet(s) were off the frame grid",
								res.grid_warnings);
						if (res.skipped_tail)
							obs_log(LOG_WARNING,
								"engine: save: the unreadable last segment was skipped");
					} else {
						os_unlink(part_path); /* D13: the caller deletes the part */
						if (s.cfg.save_sound)
							rr_play_save_sound(false);
					}
				}
			}
		}
	}

	/* Step 9 */
	if (pinned)
		rr_ledger_unpin(s.ledger, save_idx, (size_t)sel_count);
	publish_state(RR_STATE_BUFFERING);
	sweep_save_queue(); /* saves enqueued while SAVING are ignored now (5.3) */
	prune();            /* resumes pruning; the backlog from the pause goes now */

	if (!ok) {
		if (counted_fail) {
			obs_log(LOG_INFO, "nothing to save");
		} else {
			if (InterlockedCompareExchange(&g_save_cancel, 0, 0) != 0)
				obs_log(LOG_INFO, "engine: save cancelled (a video reset asked for it)");
			obs_log(LOG_INFO, "save failed: %s", rerr);
			set_error(rerr);
		}
	}
}

/* ------------------------------------------------------- idle tasks (5.4) */

/* D8 retry: a segment that failed the 2 s release wait at its boundary stays
 * unclosed — invisible to save selection and prune — until an idle tick's
 * exclusive-open probe proves the release; only then is it marked closed and
 * logged with the usual "segment closed" line. */
static void retry_segment_releases(void)
{
	if (!s.ledger)
		return;

	const size_t n = rr_ledger_count(s.ledger);
	for (size_t i = 0; i < n; i++) {
		struct rr_segment_info si;
		if (!rr_ledger_get(s.ledger, i, &si) || !si.path || si.closed || si.end_ns == 0)
			continue;
		if (!rr_file_closed(si.path))
			continue;

		int64_t bytes = -1;
		struct stat st;
		if (os_stat(si.path, &st) == 0)
			bytes = (int64_t)st.st_size;
		rr_ledger_mark_closed(s.ledger, (int)i, bytes);
		obs_log(LOG_INFO, "segment closed: %s dur=%.2fs bytes=%lld", si.path,
			(double)(si.end_ns - si.start_ns) / 1e9, (long long)bytes);
	}
}

static void idle_tasks(void)
{
	retry_segment_releases();
	retry_pending_deletes();

	if (s.out) {
		const uint64_t free_b = os_get_free_disk_space(s.buffer_dir);
		if (free_b < RR_GIB) {
			obs_log(LOG_INFO, "buffer error: the buffer drive has less than 1 GiB free");
			set_error("the buffer drive has less than 1 GiB free");
			stop_session(false, false);
			return;
		}
	}

	publish_status_values(); /* refresh the snapshot for the UI */
}

/* --------------------------------------------------------- worker + API */

/* Signal handlers run on encoder/muxer threads (E4): they only capture the
 * time (D7) and enqueue (D6). */
static void on_file_changed(void *data, calldata_t *cd)
{
	struct rr_cmd cmd = {0};
	cmd.type = CMD_BOUNDARY;
	cmd.t_ns = os_gettime_ns();
	const char *next = calldata_string(cd, "next_file");
	if (next)
		snprintf(cmd.path, sizeof(cmd.path), "%s", next);
	enqueue_cmd(&cmd);
	UNUSED_PARAMETER(data);
}

static void on_output_stopped(void *data, calldata_t *cd)
{
	struct rr_cmd cmd = {0};
	cmd.type = CMD_OUTPUT_STOPPED;
	cmd.code = (int)calldata_int(cd, "code");
	enqueue_cmd(&cmd);
	UNUSED_PARAMETER(data);
}

static DWORD WINAPI worker_main(LPVOID unused)
{
	UNUSED_PARAMETER(unused);

	for (;;) {
		struct rr_cmd cmd;
		if (dequeue_cmd(&cmd)) {
			switch (cmd.type) {
			case CMD_START:
				if (cmd.cfg) {
					if (InterlockedCompareExchange(&g_quit, 0, 0) != 0)
						obs_log(LOG_INFO, "engine: START ignored (shutting down)");
					else if (s.out)
						obs_log(LOG_INFO, "engine: START ignored (buffer already running)");
					else
						start_session(cmd.cfg);
					free(cmd.cfg);
				}
				break;
			case CMD_STOP:
				if (s.out)
					stop_session(false, false);
				else
					obs_log(LOG_INFO, "engine: STOP ignored (buffer not running)");
				break;
			case CMD_FORCE_STOP:
				if (s.out)
					stop_session(true, false);
				else
					obs_log(LOG_INFO, "engine: FORCE STOP ignored (buffer not running)");
				break;
			case CMD_SAVE:
				handle_save();
				break;
			case CMD_QUIT:
				InterlockedExchange(&g_quit, 1);
				break;
			case CMD_BOUNDARY:
				handle_boundary(cmd.path, cmd.t_ns);
				break;
			case CMD_OUTPUT_STOPPED:
				handle_unexpected_stopped(cmd.code);
				break;
			}
			if (InterlockedCompareExchange(&g_quit, 0, 0) != 0)
				break;
		} else {
			/* idle: up to 1 s, then the 5.4 idle tasks */
			if (q_wake)
				os_event_timedwait(q_wake, 1000);
			else
				os_sleep_ms(1000);
			if (InterlockedCompareExchange(&g_quit, 0, 0) != 0)
				break;
			idle_tasks();
		}
	}

	/* QUIT: no graceful stop, force and purge (5.7 note, D12) */
	if (s.out) {
		obs_log(LOG_INFO, "engine: quitting with the output active, forcing stop");
		stop_session(true, false);
	}
	os_event_signal(g_done_ev);
	return 0;
}

void rr_engine_init(void)
{
	if (g_inited)
		return;

	InitializeCriticalSection(&q_cs);
	InitializeCriticalSection(&st_cs);
	if (os_event_init(&q_wake, OS_EVENT_TYPE_AUTO) != 0)
		q_wake = NULL;
	if (os_event_init(&g_done_ev, OS_EVENT_TYPE_MANUAL) != 0)
		g_done_ev = NULL;
	memset(&g_status, 0, sizeof(g_status));
	memset(&s, 0, sizeof(s));

	g_inited = true;
	g_thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
	if (!g_thread) {
		obs_log(LOG_ERROR, "engine: could not create the worker thread");
		return;
	}
	obs_log(LOG_INFO, "engine: worker started");
}

void rr_engine_shutdown(void)
{
	if (!g_inited || g_shutdown_done)
		return;
	g_shutdown_done = true;

	InterlockedExchange(&g_quit, 1);
	struct rr_cmd cmd = {0};
	cmd.type = CMD_QUIT;
	enqueue_cmd(&cmd);

	if (g_thread && g_done_ev) {
		if (os_event_timedwait(g_done_ev, 10000) != 0)
			obs_log(LOG_ERROR, "engine: the worker did not exit within 10 s");
	}
}

void rr_engine_start(const struct rr_config *cfg)
{
	if (!g_inited || !g_thread || !cfg)
		return;

	struct rr_cmd cmd = {0};
	cmd.type = CMD_START;
	cmd.cfg = malloc(sizeof(*cmd.cfg));
	if (!cmd.cfg) {
		obs_log(LOG_ERROR, "engine: out of memory for a start command");
		return;
	}
	*cmd.cfg = *cfg;
	enqueue_cmd(&cmd);
}

void rr_engine_stop(void)
{
	if (!g_inited)
		return;
	struct rr_cmd cmd = {0};
	cmd.type = CMD_STOP;
	enqueue_cmd(&cmd);
}

void rr_engine_save(void)
{
	if (!g_inited)
		return;
	struct rr_cmd cmd = {0};
	cmd.type = CMD_SAVE;
	enqueue_cmd(&cmd);
}

void rr_engine_selftest_reference(const char *out_dir)
{
	if (out_dir && *out_dir)
		snprintf(g_ref_mkv, sizeof(g_ref_mkv), "%s\\reference.mkv", out_dir);
	else
		g_ref_mkv[0] = 0;
}

/* Polls the published state until OFF, until the engine is going away, or
 * until timeout_ms passes (then false). */
static bool wait_off_for(uint32_t timeout_ms)
{
	const uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ull;
	for (;;) {
		struct rr_status snap;
		rr_engine_get_status(&snap);
		if (snap.state == RR_STATE_OFF)
			return true;
		if (InterlockedCompareExchange(&g_quit, 0, 0) != 0 || (g_done_ev && os_event_try(g_done_ev) == 0))
			return true; /* the engine is going away: it is as off as it gets */
		if (os_gettime_ns() >= deadline)
			return false;
		os_sleep_ms(50);
	}
}

bool rr_engine_wait_off(uint32_t timeout_ms)
{
	if (!g_inited)
		return true;
	return wait_off_for(timeout_ms);
}

bool rr_engine_halt_for_video_reset(void)
{
	if (!g_inited)
		return true;

	/* The stop request goes in before the state is sampled, so a SAVE that
	 * was already queued cannot be accepted after this point. */
	struct rr_cmd cmd = {0};
	cmd.type = CMD_STOP;
	enqueue_cmd(&cmd);

	struct rr_status snap;
	rr_engine_get_status(&snap);
	if (snap.state == RR_STATE_SAVING) {
		/* Waiting a long window out takes tens of seconds; cancel the
		 * remux instead (bounded by one segment of it). */
		obs_log(LOG_INFO, "engine: video reset: cancelling the in-flight save");
		InterlockedExchange(&g_save_cancel, 1);
	}

	if (wait_off_for(RR_RESET_STOP_WAIT_MS))
		return true;

	if (InterlockedCompareExchange(&g_save_cancel, 0, 0) == 0) {
		obs_log(LOG_WARNING, "engine: video reset: not OFF after %u s, cancelling any running save",
			RR_RESET_STOP_WAIT_MS / 1000u);
		InterlockedExchange(&g_save_cancel, 1);
	} else {
		obs_log(LOG_WARNING, "engine: video reset: not OFF after %u s although the save was cancelled",
			RR_RESET_STOP_WAIT_MS / 1000u);
	}
	if (wait_off_for(RR_RESET_CANCEL_WAIT_MS))
		return true;

	obs_log(LOG_WARNING, "engine: video reset: not OFF after %u s, forcing the buffer stop",
		(RR_RESET_STOP_WAIT_MS + RR_RESET_CANCEL_WAIT_MS) / 1000u);
	cmd.type = CMD_FORCE_STOP;
	enqueue_cmd(&cmd);

	if (wait_off_for(RR_RESET_FORCE_WAIT_MS))
		return true;

	obs_log(LOG_ERROR,
		"engine: video reset: the buffer is still not OFF after %u s (stop, save cancel, "
		"force stop): the plugin output and encoders are still holding the video mode",
		(RR_RESET_STOP_WAIT_MS + RR_RESET_CANCEL_WAIT_MS + RR_RESET_FORCE_WAIT_MS) / 1000u);
	return false;
}

void rr_engine_get_status(struct rr_status *out)
{
	if (!out)
		return;
	if (!g_inited) {
		memset(out, 0, sizeof(*out));
		return;
	}
	EnterCriticalSection(&st_cs);
	*out = g_status;
	LeaveCriticalSection(&st_cs);
}
