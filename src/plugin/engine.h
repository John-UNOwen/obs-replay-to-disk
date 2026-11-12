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

/* States (handoff 5.3). SAVING is entered by the save flow (5.6) only; a
 * SAVE accepted in any other state is logged and ignored (5.3). */
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

/* Self-test only (handoff 7.2 RR_SELFTEST_REFERENCE): the worker also records
 * an unsplit reference.mkv on the same encoders into this output folder. Call
 * before rr_engine_start; an empty string disables it. Not used in normal
 * operation. */
void rr_engine_selftest_reference(const char *out_dir);

#ifdef __cplusplus
}
#endif
