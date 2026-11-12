/* settings.c — settings.json load/save/defaults/clamp (handoff 5.8, D15).
 * License: GPL-2.0-or-later.
 */
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "core/settings-clamp.h"

#define RR_FILENAME_FMT_DEFAULT "Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"

/* 5.8 ranges */
#define RR_BITRATE_MIN 1000
#define RR_BITRATE_MAX 150000
#define RR_CQ_MIN 1
#define RR_CQ_MAX 51
#define RR_AUDIO_BITRATE_MIN 64
#define RR_AUDIO_BITRATE_MAX 320
#define RR_TRACK_MASK 0x3F /* bits 0..5 = tracks 1..6 */

void rr_settings_default(struct rr_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->autostart = false;
	cfg->buffer_seconds = 1800;
	cfg->save_scope = RR_SCOPE_WINDOW;
	cfg->buffer_dir[0] = 0;
	cfg->output_dir[0] = 0;
	snprintf(cfg->filename_format, sizeof(cfg->filename_format), "%s", RR_FILENAME_FMT_DEFAULT);
	cfg->video_encoder[0] = 0;
	cfg->rate_control = RR_RC_CBR;
	cfg->bitrate_kbps = 30000;
	cfg->cq_level = 20;
	snprintf(cfg->extra_encoder_settings, sizeof(cfg->extra_encoder_settings), "%s", "{}");
	cfg->audio_tracks = 1;
	cfg->audio_bitrate_kbps = 160;
	cfg->save_sound = true;
}

static int clamp_int(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

void rr_settings_clamp(struct rr_config *cfg)
{
	cfg->buffer_seconds = rr_settings_clamp_window_sec(cfg->buffer_seconds);
	if (cfg->save_scope != RR_SCOPE_WINDOW && cfg->save_scope != RR_SCOPE_SINCE_LAST_SAVE)
		cfg->save_scope = RR_SCOPE_WINDOW;
	if (cfg->rate_control != RR_RC_CBR && cfg->rate_control != RR_RC_CQP)
		cfg->rate_control = RR_RC_CBR;
	cfg->bitrate_kbps = clamp_int(cfg->bitrate_kbps, RR_BITRATE_MIN, RR_BITRATE_MAX);
	cfg->cq_level = clamp_int(cfg->cq_level, RR_CQ_MIN, RR_CQ_MAX);
	cfg->audio_tracks &= RR_TRACK_MASK;
	if (cfg->audio_tracks == 0)
		cfg->audio_tracks = 1; /* at least one bit (5.8) */
	cfg->audio_bitrate_kbps = clamp_int(cfg->audio_bitrate_kbps, RR_AUDIO_BITRATE_MIN, RR_AUDIO_BITRATE_MAX);
	if (!cfg->filename_format[0])
		snprintf(cfg->filename_format, sizeof(cfg->filename_format), "%s", RR_FILENAME_FMT_DEFAULT);
	if (!cfg->extra_encoder_settings[0])
		snprintf(cfg->extra_encoder_settings, sizeof(cfg->extra_encoder_settings), "%s", "{}");
}

/* The full path to settings.json, or NULL if libobs could not provide one.
 * The module config directory is created when missing. */
static char *settings_json_path(void)
{
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return NULL;

	/* obs_module_config_path does not create the directory; do it here. */
	char *dir = bstrdup(path);
	if (dir) {
		char *slash = strrchr(dir, '/');
		char *bslash = strrchr(dir, '\\');
		if (bslash && (!slash || bslash > slash))
			slash = bslash;
		if (slash) {
			*slash = 0;
			os_mkdirs(dir);
		}
		bfree(dir);
	}
	return path;
}

static const char *json_str(obs_data_t *data, const char *key, char *dst, size_t dst_sz)
{
	const char *s = obs_data_get_string(data, key);
	if (s && *s)
		snprintf(dst, dst_sz, "%s", s);
	return dst;
}

void rr_settings_load(struct rr_config *cfg)
{
	rr_settings_default(cfg);

	char *path = settings_json_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	if (!data)
		return; /* missing or unreadable: the defaults stay in place */

	cfg->autostart = obs_data_get_bool(data, "autostart");
	cfg->buffer_seconds = (int)obs_data_get_int(data, "buffer_seconds");
	if (strcmp(obs_data_get_string(data, "save_scope"), "since_last_save") == 0)
		cfg->save_scope = RR_SCOPE_SINCE_LAST_SAVE;
	else
		cfg->save_scope = RR_SCOPE_WINDOW;
	json_str(data, "buffer_dir", cfg->buffer_dir, sizeof(cfg->buffer_dir));
	json_str(data, "output_dir", cfg->output_dir, sizeof(cfg->output_dir));
	json_str(data, "filename_format", cfg->filename_format, sizeof(cfg->filename_format));
	json_str(data, "video_encoder", cfg->video_encoder, sizeof(cfg->video_encoder));
	if (strcmp(obs_data_get_string(data, "rate_control"), "CQP") == 0)
		cfg->rate_control = RR_RC_CQP;
	else
		cfg->rate_control = RR_RC_CBR;
	cfg->bitrate_kbps = (int)obs_data_get_int(data, "bitrate_kbps");
	cfg->cq_level = (int)obs_data_get_int(data, "cq_level");
	json_str(data, "extra_encoder_settings", cfg->extra_encoder_settings, sizeof(cfg->extra_encoder_settings));
	cfg->audio_tracks = (int)obs_data_get_int(data, "audio_tracks");
	cfg->audio_bitrate_kbps = (int)obs_data_get_int(data, "audio_bitrate_kbps");
	cfg->save_sound = obs_data_get_bool(data, "save_sound");

	obs_data_release(data);

	rr_settings_clamp(cfg); /* clamp on load (5.8) */
}

bool rr_settings_save(const struct rr_config *cfg)
{
	struct rr_config copy = *cfg;
	rr_settings_clamp(&copy); /* clamp on set (5.8) */

	char *path = settings_json_path();
	if (!path)
		return false;

	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "autostart", copy.autostart);
	obs_data_set_int(data, "buffer_seconds", copy.buffer_seconds);
	obs_data_set_string(data, "save_scope",
			    copy.save_scope == RR_SCOPE_SINCE_LAST_SAVE ? "since_last_save" : "window");
	obs_data_set_string(data, "buffer_dir", copy.buffer_dir);
	obs_data_set_string(data, "output_dir", copy.output_dir);
	obs_data_set_string(data, "filename_format", copy.filename_format);
	obs_data_set_string(data, "video_encoder", copy.video_encoder);
	obs_data_set_string(data, "rate_control", copy.rate_control == RR_RC_CQP ? "CQP" : "CBR");
	obs_data_set_int(data, "bitrate_kbps", copy.bitrate_kbps);
	obs_data_set_int(data, "cq_level", copy.cq_level);
	obs_data_set_string(data, "extra_encoder_settings", copy.extra_encoder_settings);
	obs_data_set_int(data, "audio_tracks", copy.audio_tracks);
	obs_data_set_int(data, "audio_bitrate_kbps", copy.audio_bitrate_kbps);
	obs_data_set_bool(data, "save_sound", copy.save_sound);

	const bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
	return ok;
}

int rr_config_track_count(const struct rr_config *cfg)
{
	int n = 0;
	for (int b = 0; b < 6; b++) {
		if (cfg->audio_tracks & (1 << b))
			n++;
	}
	return n;
}

int rr_config_total_bitrate_kbps(const struct rr_config *cfg)
{
	/* Constant quality: assume 50 Mbps (5.5 step 3). */
	const int video_kbps = cfg->rate_control == RR_RC_CQP ? 50000 : cfg->bitrate_kbps;
	return video_kbps + rr_config_track_count(cfg) * cfg->audio_bitrate_kbps;
}
