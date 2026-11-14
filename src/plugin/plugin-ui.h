/* plugin-ui.h — UI bridge for the Qt dialog/dock (handoff 6 WP5).
 *
 * The worker owns all OBS objects (D5) and never calls obs_frontend_*. The
 * dialog and the dock therefore work through these functions: they read and
 * write the plugin's settings, resolve the OBS recording path (OBS 33 has no
 * obs_frontend_get_recording_directory — this is the WP3 resolver the
 * autostart and selftest paths use), and enqueue engine commands. All of
 * them must be called on the UI thread.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin/settings.h"

/* Canonical libobs handle name (obs-defs.h declares the same typedef; all
 * users of this header include obs-module.h first, an identical typedef is
 * fine in C and C++). */
typedef struct obs_source obs_source_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the plugin's current settings as saved (a buffer_dir/output_dir of
 * "" means "the OBS recording path", 5.8/D12). */
void rr_plugin_get_settings(struct rr_config *out);

/* Clamp cfg and persist it via rr_settings_save (5.8; settings.c does the
 * clamping through core/settings-clamp). Updates the plugin's copy so
 * autostart and the toggle hotkey use the new values and so the next start
 * resolves the folders again. Returns false on write failure. */
bool rr_plugin_set_settings(const struct rr_config *cfg);

/* The folders a start with cfg would actually use (empty fields resolved
 * from the OBS recording path). Used for the greyed default text and the
 * buffer-folder validation, so both match the engine exactly. */
void rr_plugin_effective_dirs(const struct rr_config *cfg, char *buffer_dir, size_t buffer_sz, char *output_dir,
			      size_t output_sz);

/* Enqueue START with a snapshot of the current settings whose folders are
 * resolved here, on the UI thread (5.4: the worker never resolves them).
 * rr_plugin_stop/save only enqueue; state rules are the engine's (5.3).
 * rr_plugin_wait_off mirrors rr_engine_wait_off. */
void rr_plugin_start(void);
void rr_plugin_stop(void);
void rr_plugin_save(void);
bool rr_plugin_wait_off(uint32_t timeout_ms);

/* UI install/teardown (handoff 6 WP5 steps 2-3). Defined by the Qt sources
 * under src/ui/, which build only when ENABLE_QT is on (RR_UI defined);
 * call them only under #ifdef RR_UI. They run on the UI thread at module
 * load/unload. */
void rr_settings_dialog_install(void);
void rr_settings_dialog_uninstall(void);
void rr_status_dock_install(void);
void rr_status_dock_uninstall(void);

/* Process watcher (src/ui/process-watch.cpp, RR_UI): a 2 s UI-thread timer
 * implementing the watch_follow / watch_autostart settings. install runs at
 * module load, uninstall at unload, refresh after rr_plugin_set_settings so
 * a new foreground is re-followed and the source cache is dropped.
 * The enumeration walks every scene for window_capture/game_capture sources
 * (uuid + "Scene/Name" display path) for the dialog picker. All UI thread. */
void rr_process_watch_install(void);
void rr_process_watch_uninstall(void);
void rr_process_watch_refresh(void);
void rr_process_watch_pin_source(obs_source_t *src, const char *uuid); /* selftest only */
void rr_process_watch_enum_capture_sources(void (*cb)(void *data, const char *uuid, const char *display), void *data);

#ifdef __cplusplus
}
#endif
