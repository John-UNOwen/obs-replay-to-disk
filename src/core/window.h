/* window.h — which segments a save covers (handoff 5.10, flow 5.6 step 5).
 * Pure C: no libobs. Selection takes the contiguous index range of closed
 * segments whose end is after the window start, ending at the newest closed
 * segment, and computes the offset into the first one. A segment that ended
 * but is not yet closed (possibly still being written) ends the range.
 * The caller pins the result with rr_ledger_pin while the save runs.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include "ledger.h"

struct rr_window {
	int first, count;          /* ledger index range of the selected segments (count > 0) */
	double inpoint_sec;        /* seconds into segment `first` to start at */
	uint64_t start_ns, end_ns; /* the effective window (start_ns is after the since-last-save shift) */
};

/* last_save_end_ns = 0 when there was no earlier save or scope is "window".
 * With it, the window start moves to max(window_start, last_save_end_ns).
 * Returns false when no closed segment overlaps the window. */
bool rr_window_select(const rr_ledger_t *l, uint64_t window_end_ns, int window_sec, uint64_t last_save_end_ns,
		      struct rr_window *out);
