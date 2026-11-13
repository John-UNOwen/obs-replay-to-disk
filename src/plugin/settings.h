/* settings.h — plugin settings: settings.json load/save/defaults/clamp
 * (handoff 5.8, D15: global settings at obs_module_config_path("settings.json"),
 * clamped on load and on set; int clamps go through core/settings-clamp).
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RR_PATHLEN 1024
#define RR_ENC_ID_LEN 64
#define RR_FILENAME_FMT_LEN 256
#define RR_EXTRA_JSON_LEN 4096

/* 5.8 default filename format (settings.c default/clamp; engine.c fallback
 * when a configured format generates an unsafe name). */
#define RR_FILENAME_FMT_DEFAULT "Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"

enum rr_save_scope { RR_SCOPE_WINDOW = 0, RR_SCOPE_SINCE_LAST_SAVE = 1 };
enum rr_rate_control { RR_RC_CBR = 0, RR_RC_CQP = 1 };

/* Everything in handoff 5.8. buffer_dir/output_dir are the raw configured
 * strings: empty means "resolve from the OBS recording path", and only the UI
 * thread may resolve them (the worker never calls obs_frontend_*). */
struct rr_config {
	bool autostart;
	int buffer_seconds;          /* 30–1800 (clamped) */
	int save_scope;              /* enum rr_save_scope */
	char buffer_dir[RR_PATHLEN]; /* "" = <OBS recording path>/.rolling-replay-buffer */
	char output_dir[RR_PATHLEN]; /* "" = OBS recording path */
	char filename_format[RR_FILENAME_FMT_LEN];
	char video_encoder[RR_ENC_ID_LEN];              /* "" = first available (5.8 order) */
	int rate_control;                               /* enum rr_rate_control */
	int bitrate_kbps;                               /* 1000–150000 (clamped) */
	int cq_level;                                   /* 1–51 (clamped) */
	char extra_encoder_settings[RR_EXTRA_JSON_LEN]; /* JSON merged into encoder settings */
	int audio_tracks;                               /* bit mask: bit 0 = track 1 … bit 5 = track 6, ≥ 1 bit */
	int audio_bitrate_kbps;                         /* 64–320 (clamped) */
	bool save_sound;
};

/* Fill cfg with the 5.8 defaults. */
void rr_settings_default(struct rr_config *cfg);

/* Clamp every range/enum/mask in place (call on load and on set). */
void rr_settings_clamp(struct rr_config *cfg);

/* Load settings.json over the defaults. A missing or broken file leaves the
 * defaults. The module config directory is created if absent. */
void rr_settings_load(struct rr_config *cfg);

/* Clamp a copy and write settings.json. Returns false on write failure. */
bool rr_settings_save(const struct rr_config *cfg);

/* Count the selected audio tracks (bits in the mask). */
int rr_config_track_count(const struct rr_config *cfg);

/* Total encoder bitrate for the disk estimate: video bitrate (or 50 Mbps
 * assumed for constant quality, 5.5 step 3) plus the audio tracks. */
int rr_config_total_bitrate_kbps(const struct rr_config *cfg);

#ifdef __cplusplus
}
#endif
