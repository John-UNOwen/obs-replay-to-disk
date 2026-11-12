/* engine.h — buffer engine interface (handoff 5.10; states 5.3).
 * All functions are called from the UI thread, are thread-safe and return at
 * once; the worker thread owns the encoders, the output, the ledger (D5).
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "plugin/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* States (handoff 5.3). SAVING is entered by the WP4 save flow only; in WP3
 * the SAVE command is logged and ignored. */
enum rr_state {
	RR_STATE_OFF = 0,
	RR_STATE_STARTING,
	RR_STATE_BUFFERING,
	RR_STATE_SAVING,
	RR_STATE_STOPPING,
};

struct rr_status {
	int state;
	double buffered_sec;
	int64_t buffer_bytes;
	char last_saved[512];
	char last_error[256];
};

void rr_engine_init(void);                         /* starts the worker */
void rr_engine_shutdown(void);                     /* QUIT + join, bounded */
void rr_engine_start(const struct rr_config *cfg); /* cfg is copied */
void rr_engine_stop(void);
void rr_engine_save(void);
bool rr_engine_wait_off(uint32_t timeout_ms);     /* for PROFILE_CHANGING */
void rr_engine_get_status(struct rr_status *out); /* copies a mutex-guarded snapshot */

#ifdef __cplusplus
}
#endif
