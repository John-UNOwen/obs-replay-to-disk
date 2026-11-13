/* settings-clamp.h — settings value clamps (handoff 5.8: clamp on load and on
 * set). Pure C: no libobs, so the core tests can use it; plugin/settings.c
 * (WP3) uses the same helpers.
 * License: GPL-2.0-or-later.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RR_WINDOW_SEC_MIN 30
#define RR_WINDOW_SEC_MAX 1800

/* Clamp a buffer length in seconds to 30–1800. */
int rr_settings_clamp_window_sec(int sec);

#ifdef __cplusplus
}
#endif
