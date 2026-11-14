/* settings-source.h — resolve the "OBS streaming/recording settings" buffer
 * source into an rr_config (settings.h enum rr_settings_source).
 * UI thread only (obs_frontend_*); a no-op when the source is manual.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include "settings.h"

/* Replace the video encoder, rate control, bitrate/quality level, extra
 * encoder JSON, audio tracks and audio bitrate of cfg with what OBS's own
 * Output settings say for the selected source, then clamp. Everything else
 * (folders, window, scope, filename, sounds) stays as configured.
 * Logs the resolved encoder and audio choices (LOG_INFO/WARNING). */
void rr_settings_resolve_source(struct rr_config *cfg);
