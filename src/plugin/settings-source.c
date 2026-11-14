/* settings-source.c — resolve the OBS streaming/recording settings source.
 *
 * Reads the OBS profile the same way OBS itself does: the profile config
 * ("Output"/"Mode", the "SimpleOutput" or "AdvOut" sections) plus, in advanced
 * mode, the profile's streamEncoder.json / recordEncoder.json.
 * Observed key names and the simple-mode encoder id map mirror OBS 33
 * (frontend/utility/SimpleOutput.cpp, frontend/utility/AdvancedOutput.cpp).
 *
 * Mapping to the plugin's model:
 *   encoder id          simple mode stores symbolic names ("x264", "nvenc",
 *                       "qsv", "amd", … plus codec suffixes): mapped with the
 *                       same table OBS uses. Advanced mode stores the id.
 *   rate control        bitrate-based modes (CBR, VBR, QVBR, LA_CBR, NVENC
 *                       Quality/Latency, …) become CBR at the configured
 *                       bitrate (D14: the plugin has no VBR); CRF/CQP/ICQ/
 *                       LDCQ become CQP at the configured quality level.
 *   audio               recording tracks come from the RecTracks mask (both
 *                       modes), streaming from the stream track selection;
 *                       the audio bitrate comes from ABitrate (simple
 *                       streaming) or the first selected track's
 *                       AdvOut/Track<N>Bitrate.
 *   extra encoder JSON  advanced mode's "settings" string (the raw options
 *                       OBS passes through). Simple mode's x264Settings is a
 *                       raw options string, not JSON, so it is not inherited.
 * License: GPL-2.0-or-later.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <plugin-support.h> /* obs_log (template helper) */

#include <stdio.h>
#include <string.h>

#include "settings-source.h"

/* OBS 33 saves the advanced encoder settings as two JSON documents in the
 * profile folder (frontend/utility/AdvancedOutput.cpp: GetDataFromJsonFile
 * ("streamEncoder.json") / ("recordEncoder.json"), resolved against
 * OBSBasic::GetProfile()). */

/* same rule as the dialog and the engine (5.8) */
static bool id_available(const char *id)
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

static bool ci_equal(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca + 32);
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb + 32);
		if (ca != cb)
			return false;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static void set_id(struct rr_config *cfg, const char *id)
{
	snprintf(cfg->video_encoder, sizeof(cfg->video_encoder), "%s", id);
}

static void set_extra(struct rr_config *cfg, const char *json)
{
	snprintf(cfg->extra_encoder_settings, sizeof(cfg->extra_encoder_settings), "%s", (json && *json) ? json : "{}");
}

/* Mirror of OBS 33 get_simple_output_encoder() for the ids this platform
 * builds. Unknown or empty symbols fall back to obs_x264, like OBS. */
static const char *simple_encoder_id(const char *sym)
{
	if (!sym || !*sym)
		return "obs_x264";
	if (ci_equal(sym, "qsv"))
		return "obs_qsv11_v2";
	if (ci_equal(sym, "qsv_av1"))
		return "obs_qsv11_av1";
	if (ci_equal(sym, "amd"))
		return "h264_texture_amf";
	if (ci_equal(sym, "amd_hevc"))
		return "h265_texture_amf";
	if (ci_equal(sym, "amd_av1"))
		return "av1_texture_amf";
	if (ci_equal(sym, "nvenc"))
		return id_available("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	if (ci_equal(sym, "nvenc_hevc"))
		return id_available("obs_nvenc_hevc_tex") ? "obs_nvenc_hevc_tex" : "ffmpeg_hevc_nvenc";
	if (ci_equal(sym, "nvenc_av1"))
		return "obs_nvenc_av1_tex";
	return "obs_x264"; /* "x264", "x264_lowcpu" and unknown values */
}

/* advanced encoder JSON: quality-based rate controls, vs bitrate-based */
static bool rc_is_quality(const char *rc)
{
	return rc && (ci_equal(rc, "CQP") || ci_equal(rc, "CRF") || ci_equal(rc, "ICQ") || ci_equal(rc, "LDCQ"));
}

static void apply_advanced_json(struct rr_config *cfg, obs_data_t *j)
{
	const char *rc = obs_data_get_string(j, "rate_control");
	if (rc_is_quality(rc)) {
		static const char *qkeys[] = {"crf", "cqp", "icq_quality", "ldcq"};
		int q = 0;
		for (size_t i = 0; i < sizeof(qkeys) / sizeof(qkeys[0]) && !q; i++) {
			if (obs_data_has_user_value(j, qkeys[i]))
				q = (int)obs_data_get_int(j, qkeys[i]);
		}
		cfg->rate_control = RR_RC_CQP;
		if (q > 0)
			cfg->cq_level = q;
	} else {
		const int br = (int)obs_data_get_int(j, "bitrate");
		cfg->rate_control = RR_RC_CBR;
		if (br > 0)
			cfg->bitrate_kbps = br;
	}

	set_extra(cfg, obs_data_get_string(j, "settings"));
}

static obs_data_t *profile_json(const char *profile_path, const char *name)
{
	if (!profile_path || !*profile_path)
		return NULL;
	char path[RR_PATHLEN];
	snprintf(path, sizeof(path), "%s/%s.json", profile_path, name);
	return obs_data_create_from_json_file(path);
}

/* Audio bitrate for the selected tracks; 0 means "keep the plugin value". */
static int inherited_audio_bitrate(config_t *pc, bool advanced, bool streaming, int mask)
{
	if (!advanced && streaming)
		return (int)config_get_uint(pc, "SimpleOutput", "ABitrate");

	for (int b = 0; b < 6; b++) {
		if (!(mask & (1 << b)))
			continue;
		char key[32];
		snprintf(key, sizeof(key), "Track%dBitrate", b + 1);
		return (int)config_get_uint(pc, "AdvOut", key); /* first selected track */
	}
	return 0;
}

static int streaming_audio_mask(config_t *pc, bool advanced)
{
	if (!advanced)
		return 1; /* simple mode streams track 1 */
	const int multi = (int)config_get_uint(pc, "AdvOut", "StreamMultiTrackAudioMixes") & 0x3F;
	if (multi)
		return multi;
	int idx = (int)config_get_uint(pc, "AdvOut", "TrackIndex");
	if (idx < 1 || idx > 6)
		idx = 1;
	return 1 << (idx - 1);
}

static int recording_audio_mask(config_t *pc, bool advanced)
{
	const int mask = (int)config_get_uint(pc, advanced ? "AdvOut" : "SimpleOutput", "RecTracks") & 0x3F;
	return mask ? mask : 1;
}

static void inherit_audio(struct rr_config *cfg, config_t *pc, bool advanced, bool streaming)
{
	cfg->audio_tracks = streaming ? streaming_audio_mask(pc, advanced) : recording_audio_mask(pc, advanced);
	const int abr = inherited_audio_bitrate(pc, advanced, streaming, cfg->audio_tracks);
	if (abr > 0)
		cfg->audio_bitrate_kbps = abr;
}

static void resolve_streaming(struct rr_config *cfg, config_t *pc, const char *profile_path, bool advanced)
{
	if (advanced) {
		const char *id = config_get_string(pc, "AdvOut", "Encoder");
		if (id && *id && strcmp(id, "none") != 0)
			set_id(cfg, id);
		else
			cfg->video_encoder[0] = 0; /* no stream encoder configured: auto */

		obs_data_t *j = profile_json(profile_path, "streamEncoder");
		if (j) {
			apply_advanced_json(cfg, j);
			obs_data_release(j);
		} else {
			set_extra(cfg, "{}");
			obs_log(LOG_WARNING,
				"settings source: the profile has no streamEncoder.json, keeping the plugin rate "
				"control and bitrate");
		}
	} else {
		set_id(cfg, simple_encoder_id(config_get_string(pc, "SimpleOutput", "StreamEncoder")));
		const int br = (int)config_get_uint(pc, "SimpleOutput", "VBitrate");
		cfg->rate_control = RR_RC_CBR;
		if (br > 0)
			cfg->bitrate_kbps = br;
		set_extra(cfg, "{}"); /* simple mode has no JSON extra settings */
	}

	inherit_audio(cfg, pc, advanced, true);
}

static void resolve_recording(struct rr_config *cfg, config_t *pc, const char *profile_path, bool advanced)
{
	if (advanced) {
		const char *id = config_get_string(pc, "AdvOut", "RecEncoder");
		if (!id || !*id || ci_equal(id, "none")) {
			obs_log(LOG_INFO, "settings source: the recording reuses the streaming encoder, following the "
					  "streaming settings");
			resolve_streaming(cfg, pc, profile_path, true);
			return;
		}
		set_id(cfg, id);

		obs_data_t *j = profile_json(profile_path, "recordEncoder");
		if (j) {
			apply_advanced_json(cfg, j);
			obs_data_release(j);
		} else {
			set_extra(cfg, "{}");
			obs_log(LOG_WARNING,
				"settings source: the profile has no recordEncoder.json, keeping the plugin rate "
				"control and bitrate");
		}
	} else {
		const char *quality = config_get_string(pc, "SimpleOutput", "RecQuality");
		if (quality && strcmp(quality, "Stream") == 0) {
			obs_log(LOG_INFO, "settings source: the recording quality is 'same as stream', following the "
					  "streaming settings");
			resolve_streaming(cfg, pc, profile_path, false);
			return;
		}
		if (quality && strcmp(quality, "Lossless") == 0) {
			/* lossless records raw frames through an FFmpeg output the buffer
			 * cannot mirror: fall back to the plugin's automatic choice */
			obs_log(LOG_WARNING,
				"settings source: recording quality 'Lossless' cannot be used by the replay "
				"buffer, falling back to the automatic encoder and plugin bitrate");
			cfg->video_encoder[0] = 0;
			cfg->rate_control = RR_RC_CBR;
			cfg->bitrate_kbps = 30000;
			cfg->cq_level = 20;
			set_extra(cfg, "{}");
		} else {
			set_id(cfg, simple_encoder_id(config_get_string(pc, "SimpleOutput", "RecEncoder")));
			/* OBS 33 simple recording is quality-based (SimpleOutput::UpdateRecording
			 * Settings): HQ ~ CRF 16, Small ~ CRF 23 (its resolution tweak is not
			 * replicated). */
			cfg->rate_control = RR_RC_CQP;
			cfg->cq_level = (quality && strcmp(quality, "HQ") == 0) ? 16 : 23;
			set_extra(cfg, "{}");
		}
	}

	inherit_audio(cfg, pc, advanced, false);
}

void rr_settings_resolve_source(struct rr_config *cfg)
{
	if (cfg->settings_source == RR_SOURCE_MANUAL)
		return;

	config_t *pc = obs_frontend_get_profile_config();
	if (!pc) {
		obs_log(LOG_WARNING, "settings source: the OBS profile config is unavailable, using the plugin values");
		return;
	}

	char *profile_path = obs_frontend_get_current_profile_path();
	/* OBS 33 stores the output mode here (frontend/settings/OBSBasicSettings
	 * .cpp: config_get_string/main->Config(), "Output", "Mode"; the dialog
	 * writes "Simple" or "Advanced"). Anything else is simple mode, which
	 * is also OBS's default. */
	const char *mode = config_get_string(pc, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;

	if (cfg->settings_source == RR_SOURCE_STREAMING)
		resolve_streaming(cfg, pc, profile_path, advanced);
	else
		resolve_recording(cfg, pc, profile_path, advanced);

	bfree(profile_path);

	rr_settings_clamp(cfg);

	char video_desc[96];
	if (cfg->rate_control == RR_RC_CQP)
		snprintf(video_desc, sizeof(video_desc), "CQP quality %d", cfg->cq_level);
	else
		snprintf(video_desc, sizeof(video_desc), "CBR %d kbps", cfg->bitrate_kbps);
	obs_log(LOG_INFO, "settings source: %s settings (%s output): encoder %s, %s, audio tracks 0x%02x at %d kbps",
		cfg->settings_source == RR_SOURCE_STREAMING ? "streaming" : "recording",
		advanced ? "advanced" : "simple", cfg->video_encoder[0] ? cfg->video_encoder : "automatic", video_desc,
		cfg->audio_tracks, cfg->audio_bitrate_kbps);
}
