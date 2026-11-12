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

	for (size_t i = 0; i < n; i++) {
		struct rr_segment_info seg;
		if (!rr_ledger_get(l, i, &seg))
			continue;
		if (!seg.closed || seg.end_ns == 0)
			continue; /* the open or still-written segment is not saveable */
		if (seg.end_ns <= start)
			continue; /* ended at or before the window start: no overlap */
		if (first < 0)
			first = (int)i;
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
