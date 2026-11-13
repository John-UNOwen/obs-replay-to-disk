/* window.c — segment selection for a save. See window.h (flow 5.6 step 5).
 * License: GPL-2.0-or-later.
 */
#include "window.h"

bool rr_window_select(const rr_ledger_t *l, uint64_t window_end_ns, int window_sec, uint64_t last_save_end_ns,
		      struct rr_window *out)
{
	uint64_t span = (uint64_t)(window_sec > 0 ? window_sec : 0) * 1000000000ULL;
	uint64_t start = window_end_ns > span ? window_end_ns - span : 0;

	/* Scope "since_last_save": the window start never moves before the end of
	 * the previous save. 0 means "window" scope or no earlier save. */
	if (last_save_end_ns > start)
		start = last_save_end_ns;

	int first = -1, count = 0;
	size_t n = rr_ledger_count(l);

	/* Walk newest→oldest and take the contiguous run of saveable segments
	 * ending at the newest closed one: first/count must stay a contiguous
	 * ledger index range (the caller pins and saves exactly that range).
	 * A segment that ended but is not closed (its boundary release wait
	 * failed, the file may still be written) ends the run — the footage
	 * older than it is not saveable together with newer segments. */
	for (size_t i = n; i > 0; i--) {
		struct rr_segment_info seg;
		if (!rr_ledger_get(l, i - 1, &seg))
			continue;
		if (seg.end_ns == 0)
			continue; /* the open tail segment: it has not ended yet */
		if (!seg.closed || seg.end_ns <= start)
			break; /* not released, or ended before the window: run over */
		first = (int)(i - 1);
		count++;
	}

	if (first < 0)
		return false; /* "nothing to save" */

	struct rr_segment_info f;
	rr_ledger_get(l, (size_t)first, &f);

	out->first = first;
	out->count = count;
	out->inpoint_sec = start > f.start_ns ? (double)(start - f.start_ns) / 1e9 : 0.0;
	out->start_ns = start;
	out->end_ns = window_end_ns;
	return true;
}

uint64_t rr_window_saved_end(const rr_ledger_t *l, const struct rr_window *w, int skipped_tail)
{
	if (w->count < 1)
		return 0;

	struct rr_segment_info first, last;
	if (!rr_ledger_get(l, (size_t)w->first, &first))
		return 0;

	/* The run starts past the window start although older segments exist:
	 * the walk broke at an unreleased segment, so footage below the run was
	 * in the window and is not in the clip. The boundary must not move —
	 * whatever releases later stays ahead of the next save's start. */
	if (w->first > 0 && first.start_ns > w->start_ns)
		return 0;

	int idx = w->first + w->count - 1;
	if (skipped_tail && idx > w->first)
		idx--; /* the newest selected segment was an unreadable stub, not written */

	if (!rr_ledger_get(l, (size_t)idx, &last))
		return 0;
	return last.end_ns;
}
