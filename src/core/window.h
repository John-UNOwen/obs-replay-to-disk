/* window.h — which segments a save covers (handoff 5.10, flow 5.6 step 5).
 * Pure C: no libobs. Selection takes the contiguous index range of closed
 * segments whose end is after the window start, ending at the newest closed
 * segment, and computes the offset into the first one. A segment that ended
 * but is not yet closed (possibly still being written) ends the range below
 * it; one above the range — newer than the newest closed segment, the usual
 * just-rotated file a scanner holds — is skipped, not a refusal: the run
 * under it is saveable on its own and the held footage, which starts after
 * the run's end, stays ahead of every later boundary.
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

/* End of the footage a successful save actually wrote, for the since-last-save
 * bookkeeping (5.6 step 8). It is the end of the last segment written — the
 * last of the selected range, its predecessor when rr_remux reported
 * skipped_tail (an unreadable stub left out of the clip) — which can sit
 * below the requested window end. 0 when the save must not advance the
 * boundary at all: an unreleased segment cut the run off while unsaved
 * in-window footage stayed below it, or the range is not (anymore) in the
 * ledger. Advancing past the written end over such footage would claim it
 * saved, so no later save offers it again and prune eventually deletes it. */
uint64_t rr_window_saved_end(const rr_ledger_t *l, const struct rr_window *w, int skipped_tail);
