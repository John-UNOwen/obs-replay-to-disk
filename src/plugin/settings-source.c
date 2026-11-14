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
 *                       CQVBR, …) become CBR at the configured bitrate
 *                       (D14: the plugin has no VBR; CQVBR's bitrate is
 *                       hidden in OBS's UI but still drives its encoder);
 *                       CRF/CQP/ICQ/LDCQ become CQP at the configured quality
 *                       level; NVENC "lossless" encodes raw frames the CBR/
 *                       CQP model cannot mirror, so the plugin keeps its
 *                       manual encoder, rate control and bitrate (warned).
 *   audio               recording tracks come from the RecTracks mask (both
 *                       modes; an advanced-mode mask of 0 follows
 *                       AdvOut/TrackIndex as the mask, like OBS 33's legacy
 *                       fallback), streaming from the stream track selection:
 *                       in advanced mode the StreamMultiTrackAudioMixes mask
 *                       applies only for SRT/RIST services (OBS 33
 *                       AdvancedOutput::allowsMultiTrack), for everything
 *                       else OBS streams the single AdvOut/TrackIndex track;
 *                       the audio bitrate comes from SimpleOutput/ABitrate in
 *                       simple mode, streaming and recording alike (OBS 33
 *                       creates the simple stream encoder and the per-track
 *                       recording encoders the replay buffer selects with the
 *                       same GetAudioBitrate()), or from the first selected
 *                       track's AdvOut/Track<N>Bitrate in advanced mode.
 *   extra encoder JSON  advanced mode's x264 raw options box: OBS 33 encoder
 *                       JSON holds the encoder's own property values, and that
 *                       box is the string key "x264opts". The plugin inherits
 *                       exactly that key so the engine's settings merge
 *                       reproduces OBS's encoder settings; encoder JSONs
 *                       without a raw options box reset the extra JSON. Simple
 *                       mode's x264Settings is a raw options string, not JSON,
 *                       so it is not inherited.
 * License: GPL-2.0-or-later.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/dstr.h> /* astrcmpi_n */
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

/* NVENC's lossless rate control (obs-studio 33 plugins/obs-nvenc/
 * nvenc-properties.c:120-122, compat list nvenc-compat.c:169): the encoder
 * switches to NVENC's raw lossless preset/tune and OBS hides the bitrate
 * property (nvenc-properties.c:87-90, nvenc.c:259-265, nvenc-compat.c:143-146),
 * so the bitrate left in the encoder JSON belongs to whatever rate control the
 * profile used before. CBR/CQP cannot express lossless encoding: the caller
 * keeps the plugin's manual video settings and warns. */
static bool rc_is_lossless(const char *rc)
{
	return rc && ci_equal(rc, "lossless");
}

/* The encoder property that holds the quality level for a rate control: x264
 * "crf" (plugins/obs-x264/obs-x264.c:195), QSV "cqp" and "icq_quality"
 * (plugins/obs-qsv11/obs-qsv11.c:381, :388). The saved encoder JSON keeps
 * every key the user ever set (libobs obs-data.c:578 saves all user values,
 * and nothing removes the old key when the rate control changes), so a
 * profile that was CQP and is now ICQ still carries the stale "cqp": the
 * quality level has to be read from the key of the rate control actually in
 * the JSON, not from whichever key happens to be present. NULL for a rate
 * control this plugin has no level key for. */
static const char *rc_quality_key(const char *rc)
{
	if (!rc || !*rc)
		return NULL;
	if (ci_equal(rc, "CRF"))
		return "crf";
	if (ci_equal(rc, "CQP"))
		return "cqp";
	if (ci_equal(rc, "ICQ"))
		return "icq_quality";
	if (ci_equal(rc, "LDCQ"))
		return "ldcq";
	return NULL;
}

static void apply_advanced_json(struct rr_config *cfg, obs_data_t *j)
{
	const char *rc = obs_data_get_string(j, "rate_control");
	if (rc_is_quality(rc)) {
		static const char *qkeys[] = {"crf", "cqp", "icq_quality", "ldcq"};
		const char *want = rc_quality_key(rc);
		int q = 0;

		if (want && obs_data_has_user_value(j, want)) {
			q = (int)obs_data_get_int(j, want);
		} else {
			/* a profile written by an older OBS or a plugin build that named the
			 * key differently: fall back to the first quality value present */
			if (want)
				obs_log(LOG_WARNING,
					"settings source: rate control '%s' has no '%s' value in the profile encoder "
					"settings, inheriting the first quality level found instead",
					rc, want);
			for (size_t i = 0; i < sizeof(qkeys) / sizeof(qkeys[0]) && !q; i++) {
				if (obs_data_has_user_value(j, qkeys[i]))
					q = (int)obs_data_get_int(j, qkeys[i]);
			}
		}
		cfg->rate_control = RR_RC_CQP;
		if (q > 0)
			cfg->cq_level = q;
	} else {
		/* Everything bitrate-based inherits as CBR at the JSON's bitrate
		 * (D14: the plugin has no VBR). NVENC CQVBR belongs here even though
		 * its UI hides the bitrate row (nvenc-properties.c:89-90): OBS feeds
		 * that bitrate into the encoder's rate-control parameters anyway
		 * (nvenc.c:357-358, :112-113) and CQVBR's target_quality is not a
		 * QP constant quality could express. NVENC "lossless" never reaches
		 * this branch (apply_advanced_encoder keeps the plugin values). */
		const int br = (int)obs_data_get_int(j, "bitrate");
		cfg->rate_control = RR_RC_CBR;
		if (br > 0)
			cfg->bitrate_kbps = br;
	}

	/* OBS 33 writes the encoder's own property values into streamEncoder.json /
	 * recordEncoder.json and feeds the file straight into obs_encoder_update
	 * (frontend/utility/AdvancedOutput.cpp:208-209, :260, :268-269). x264's raw
	 * options box is the string key "x264opts" (plugins/obs-x264/obs-x264.c:115,
	 * :217, read at :544), so inheriting exactly that key reproduces OBS's
	 * settings through the engine's obs_data_apply. Serialize the one key with
	 * obs_data so the raw string is JSON-escaped; other encoders have no raw
	 * options box in the JSON and the extra settings reset as before. */
	const char *xopts = obs_data_get_string(j, "x264opts");
	if (xopts && *xopts) {
		obs_data_t *extra = obs_data_create();
		obs_data_set_string(extra, "x264opts", xopts);
		const char *json = obs_data_get_json(extra);
		if (json && strlen(json) < sizeof(cfg->extra_encoder_settings)) {
			set_extra(cfg, json);
		} else {
			set_extra(cfg, "{}");
			obs_log(LOG_WARNING,
				"settings source: the profile's x264 options are too long to inherit, keeping the extra "
				"encoder settings empty");
		}
		obs_data_release(extra);
	} else {
		set_extra(cfg, "{}");
	}
}

/* The advanced-mode video half in one place: the OBS encoder id plus the
 * encoder JSON's rate control and options. With no encoder JSON the id is
 * still inherited and the rate control stays manual (warned, as before). A
 * lossless rate control cannot be replay-buffered at all (rc_is_lossless):
 * warn and leave the plugin's manual encoder, rate control and bitrate
 * untouched — inheriting the encoder with a stale hidden bitrate would be the
 * silent mis-mapping OBS's own lossless UI makes impossible to notice. */
static void apply_advanced_encoder(struct rr_config *cfg, const char *id, obs_data_t *j, const char *json_name)
{
	if (j) {
		const char *rc = obs_data_get_string(j, "rate_control");
		if (rc_is_lossless(rc)) {
			obs_log(LOG_WARNING,
				"settings source: the encoder settings in %s use rate control '%s', which records "
				"raw frames the replay buffer cannot mirror: keeping the plugin encoder, rate "
				"control and bitrate",
				json_name, rc);
			return;
		}
	}

	if (id && *id && strcmp(id, "none") != 0)
		set_id(cfg, id);
	else
		cfg->video_encoder[0] = 0; /* no encoder configured: auto */

	if (!j) {
		set_extra(cfg, "{}");
		obs_log(LOG_WARNING,
			"settings source: the profile has no %s, keeping the plugin rate control and bitrate",
			json_name);
		return;
	}

	apply_advanced_json(cfg, j);
}

static obs_data_t *profile_json(const char *profile_path, const char *name)
{
	if (!profile_path || !*profile_path)
		return NULL;
	char path[RR_PATHLEN];
	snprintf(path, sizeof(path), "%s/%s.json", profile_path, name);
	return obs_data_create_from_json_file(path);
}

/* Audio bitrate for the selected tracks; 0 means "keep the plugin value".
 * Simple mode has one audio bitrate setting for streaming and recording
 * alike: OBS 33 creates the simple streaming encoder (SimpleOutput.cpp:200,
 * :202) and the per-track recording encoders that the replay buffer selects
 * by RecTracks (:175, :178, connected at :804-808, :816-820) all with
 * GetAudioBitrate() = SimpleOutput/ABitrate (:260-269). Reading the advanced
 * per-track keys there is wrong: OBSBasic seeds AdvOut/Track1Bitrate with a
 * 160 default in every profile (widgets/OBSBasic.cpp:674), so a pure simple
 * profile would silently override the configured ABitrate with 160. */
static int inherited_audio_bitrate(config_t *pc, bool advanced, int mask)
{
	if (!advanced)
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

/* Mirror of OBS 33 AdvancedOutput::allowsMultiTrack() (frontend/utility/
 * AdvancedOutput.cpp:281-291): the StreamMultiTrackAudioMixes mask applies
 * only when the current service's protocol starts with "srt" or "rist"
 * (case-insensitive). For every other service (RTMP/RTMPS/WHIP, or no
 * protocol) OBS streams only the single AdvOut/TrackIndex track
 * (AdvancedOutput.cpp:310-318, :176-178). obs_frontend_get_streaming_service
 * () is the OBS 33 frontend API for the current service: OBSStudioAPI.cpp
 * :529-532 returns OBSBasic::GetService(), the same object allowsMultiTrack
 * checks. The resolver runs on the UI thread, where obs_frontend_* calls
 * belong. */
static bool service_allows_multi_track(const char **protocol_out)
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	const char *protocol = service ? obs_service_get_protocol(service) : NULL;

	if (protocol_out)
		*protocol_out = protocol;
	return protocol && (astrcmpi_n(protocol, "srt", 3) == 0 || astrcmpi_n(protocol, "rist", 4) == 0);
}

static int streaming_audio_mask(config_t *pc, bool advanced)
{
	if (!advanced)
		return 1; /* simple mode streams track 1 */

	const char *protocol = NULL;
	if (service_allows_multi_track(&protocol)) {
		const int multi = (int)config_get_uint(pc, "AdvOut", "StreamMultiTrackAudioMixes") & 0x3F;
		if (multi) {
			obs_log(LOG_INFO,
				"settings source: service protocol '%s' carries multi-track streaming audio, "
				"following the StreamMultiTrackAudioMixes mask 0x%02x",
				protocol, multi);
			return multi;
		}
		/* OBS would stream no audio with a 0 mask; keep the single-track fallback */
	}

	int idx = (int)config_get_uint(pc, "AdvOut", "TrackIndex");
	if (idx < 1 || idx > 6)
		idx = 1;
	obs_log(LOG_INFO,
		"settings source: service protocol '%s' does not carry multi-track streaming audio, "
		"following AdvOut/TrackIndex (track %d)",
		protocol ? protocol : "(none)", idx);
	return 1 << (idx - 1);
}

/* Recording tracks come from the RecTracks mask in both modes, but the two
 * modes diverge on a zero mask (legacy configurations): OBS 33 advanced
 * recording falls back to AdvOut/TrackIndex and uses that value as the track
 * mask itself (frontend/utility/AdvancedOutput.cpp:356-362, "Hack to allow
 * recordings without any audio tracks selected ... legacy configurations";
 * the selection loop at :386-394 tests the low MAX_AUDIO_MIXES bits, so the
 * value acts truncated to 6 bits — TrackIndex 3 becomes mask 0x03), while OBS
 * 33 simple recording has no fallback at all and records without audio
 * (frontend/utility/SimpleOutput.cpp:804-808). A mirror that is still 0 (no
 * legacy TrackIndex key either) keeps the plugin's track 1 through the clamp
 * (settings.c, 5.8); OBS would record silent video there. */
static int recording_audio_mask(config_t *pc, bool advanced)
{
	const int mask = (int)config_get_uint(pc, advanced ? "AdvOut" : "SimpleOutput", "RecTracks") & 0x3F;
	if (mask)
		return mask;
	if (advanced) {
		const int legacy = (int)config_get_uint(pc, "AdvOut", "TrackIndex") & 0x3F;
		obs_log(LOG_INFO,
			"settings source: the recording has no RecTracks mask (legacy configuration), following "
			"AdvOut/TrackIndex as the track mask 0x%02x like OBS",
			legacy);
		return legacy;
	}
	return 1;
}

static void inherit_audio(struct rr_config *cfg, config_t *pc, bool advanced, bool streaming)
{
	cfg->audio_tracks = streaming ? streaming_audio_mask(pc, advanced) : recording_audio_mask(pc, advanced);
	const int abr = inherited_audio_bitrate(pc, advanced, cfg->audio_tracks);
	if (abr > 0)
		cfg->audio_bitrate_kbps = abr;
}

/* Video half of the streaming settings: encoder id plus streamEncoder.json
 * rate control. A recording that shares the streaming video encoder must
 * inherit exactly this and nothing else (see resolve_recording). */
static void resolve_streaming_video(struct rr_config *cfg, config_t *pc, const char *profile_path, bool advanced)
{
	if (advanced) {
		obs_data_t *j = profile_json(profile_path, "streamEncoder");
		apply_advanced_encoder(cfg, config_get_string(pc, "AdvOut", "Encoder"), j, "streamEncoder.json");
		if (j)
			obs_data_release(j);
	} else {
		set_id(cfg, simple_encoder_id(config_get_string(pc, "SimpleOutput", "StreamEncoder")));
		const int br = (int)config_get_uint(pc, "SimpleOutput", "VBitrate");
		cfg->rate_control = RR_RC_CBR;
		if (br > 0)
			cfg->bitrate_kbps = br;
		set_extra(cfg, "{}"); /* simple mode has no JSON extra settings */
	}
}

static void resolve_streaming(struct rr_config *cfg, config_t *pc, const char *profile_path, bool advanced)
{
	resolve_streaming_video(cfg, pc, profile_path, advanced);
	inherit_audio(cfg, pc, advanced, true);
}

static void resolve_recording(struct rr_config *cfg, config_t *pc, const char *profile_path, bool advanced)
{
	if (advanced) {
		const char *id = config_get_string(pc, "AdvOut", "RecEncoder");
		if (!id || !*id || ci_equal(id, "none")) {
			/* "same as stream" shares only the video encoder: OBS still feeds
			 * the recording output from the RecTracks-selected recordTrack[]
			 * encoders at Track<i>Bitrate (AdvancedOutput.cpp:348, :364-394,
			 * :522-527), so only the video half follows the streaming
			 * settings; the audio below comes from the recording settings. */
			obs_log(LOG_INFO,
				"settings source: the recording reuses the streaming video encoder, following the "
				"streaming video settings only");
			resolve_streaming_video(cfg, pc, profile_path, true);
		} else {
			obs_data_t *j = profile_json(profile_path, "recordEncoder");
			apply_advanced_encoder(cfg, id, j, "recordEncoder.json");
			if (j)
				obs_data_release(j);
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
