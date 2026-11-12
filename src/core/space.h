/* space.h — disk sizing for the buffer (handoff 5.10, flow 5.5 step 3).
 * Pure C: no libobs. The caller reads the free space; this module only does
 * the arithmetic.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Estimated bytes a full window of segments takes: bitrate × window. */
int64_t rr_space_estimate_bytes(int bitrate_kbps_total, int window_sec);

/* same_drive: buffer and output folders share a drive. Returns true when there
 * is room: 2 × estimate + 1 GiB on the shared drive (the buffer plus the
 * worst-case save), else 1 × estimate + 1 GiB on each drive. */
bool rr_space_preflight(int64_t estimate, int64_t free_buffer, int64_t free_output, bool same_drive);
