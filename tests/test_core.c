/* test_core.c — tests for the pure-C core (WP2). Plain assert, one function
 * per required test, no framework. Expected numbers are hand-written from the
 * handoff evidence (E6 variable segment lengths, E16 small segments) and the
 * WP2 table, never recomputed the way the code under test computes them.
 * License: GPL-2.0-or-later.
 */
#include "core/ledger.h"
#include "core/paths.h"
#include "core/settings-clamp.h"
#include "core/space.h"
#include "core/window.h"

/* RelWithDebInfo builds define NDEBUG; these tests assert regardless of the
 * build configuration, so the assert header is included with NDEBUG undef. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SEC(s) ((uint64_t)(s) * 1000000000ULL)
#define MIB(n) ((int64_t)(n) * 1024 * 1024)

#define RUN(test)            \
	do {                 \
		printf(#test "\n"); \
		fflush(stdout);     \
		test();           \
	} while (0)

static bool secs_eq(double got, double want)
{
	return fabs(got - want) < 1e-9;
}

/* Seed the ledger and close/open segments at the given times. Every time except
 * the first ends one segment and opens the next; each ended segment is marked
 * closed with `bytes` (except a segment the test closes by hand). */
static rr_ledger_t *make_chain(int window_sec, size_t n_times, const uint64_t *times, int64_t bytes)
{
	rr_ledger_t *l = rr_ledger_create(window_sec);
	char path[64];

	snprintf(path, sizeof(path), "C:/buf/rrseg_00.mkv");
	rr_ledger_seed(l, path, times[0]);
	for (size_t i = 1; i < n_times; i++) {
		snprintf(path, sizeof(path), "C:/buf/rrseg_%02zu.mkv", i);
		int ended = rr_ledger_boundary(l, path, times[i]);
		assert(ended == (int)(i - 1));
		rr_ledger_mark_closed(l, ended, bytes);
	}
	return l;
}

static void boundary_closes_open_and_opens_next_at_same_instant(void)
{
	rr_ledger_t *l = rr_ledger_create(60);
	struct rr_segment_info a, b;

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", SEC(1));
	assert(rr_ledger_count(l) == 1);
	assert(rr_ledger_get(l, 0, &a));
	assert(a.start_ns == SEC(1));
	assert(a.end_ns == 0); /* open */
	assert(!a.closed);
	assert(a.pins == 0);

	int ended = rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", SEC(11));
	assert(ended == 0);
	assert(rr_ledger_count(l) == 2);

	assert(rr_ledger_get(l, 0, &a));
	assert(a.end_ns == SEC(11)); /* the old one ends exactly at t_ns ... */
	assert(a.start_ns == SEC(1));
	assert(!a.closed); /* boundary only ends; closing is mark_closed */

	assert(rr_ledger_get(l, 1, &b));
	assert(b.start_ns == SEC(11)); /* ... and the new one starts at the same instant (D7) */
	assert(b.end_ns == 0);
	assert(!b.closed);
	assert(b.bytes == 0);

	assert(rr_ledger_get(l, 2, &a) == false);

	rr_ledger_destroy(l);
}

static void open_segment_is_never_prunable(void)
{
	rr_ledger_t *l = rr_ledger_create(10);
	int idx[8];

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", 0);
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", SEC(10)) == 0);
	rr_ledger_mark_closed(l, 0, MIB(4));
	/* seg 1 has been open since t=10 s; at t=200 s it is 20× the window old. */

	/* A lone open segment is never prunable, at any age. */
	rr_ledger_t *l2 = rr_ledger_create(10);
	rr_ledger_seed(l2, "C:/buf/rrseg_00.mkv", 0);
	assert(rr_ledger_prunable(l2, SEC(1000), idx, 8) == 0);
	rr_ledger_destroy(l2);

	/* Next to a prunable closed segment, still only the closed one is returned. */
	size_t n = rr_ledger_prunable(l, SEC(200), idx, 8);
	assert(n == 1);
	assert(idx[0] == 0);

	rr_ledger_destroy(l);
}

static void prune_takes_closed_segments_past_window_plus_margin(void)
{
	/* D10: margin = window + 15 s = 45 s, strict "older than". */
	const uint64_t times[] = {0, SEC(10), SEC(10) + 1};
	rr_ledger_t *l = make_chain(30, 3, times, MIB(4));
	int idx[4];

	/* now = 55.000000001 s: seg0 is 45.000000001 s past its end (strictly older
	 * than the margin → pruned); seg1 is exactly 45.000000000 s past its end
	 * (at the boundary, "older than" is strict → kept); seg2 is open. */
	assert(rr_ledger_prunable(l, SEC(55) + 1, idx, 4) == 1);
	assert(idx[0] == 0);

	/* One ns more and seg1 crosses the boundary too. */
	size_t n = rr_ledger_prunable(l, SEC(55) + 2, idx, 4);
	assert(n == 2);
	assert(idx[0] == 0 && idx[1] == 1);

	/* The result is capped at max. */
	assert(rr_ledger_prunable(l, SEC(55) + 2, idx, 1) == 1);

	/* Removing after a successful delete compacts the ledger (indices shift). */
	rr_ledger_remove(l, "C:/buf/rrseg_00.mkv");
	assert(rr_ledger_count(l) == 2);
	struct rr_segment_info seg;
	assert(rr_ledger_get(l, 0, &seg));
	assert(strcmp(seg.path, "C:/buf/rrseg_01.mkv") == 0);
	rr_ledger_remove(l, "does-not-exist"); /* harmless */
	assert(rr_ledger_count(l) == 2);

	rr_ledger_destroy(l);
}

static void pinned_segment_is_not_prunable_until_unpinned(void)
{
	/* window 10 → margin 25 s. */
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30)};
	rr_ledger_t *l = make_chain(10, 4, times, MIB(4));
	int idx[4];
	const int pin01[] = {0, 1};
	const int pin0[] = {0};
	const int pin1[] = {1};

	/* now = 50 s: closed segs 0 and 1 are 40 s and 30 s past their ends (margin
	 * 25 s) → prunable when unpinned; seg 2 (20 s) is still too young. */
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 2);

	rr_ledger_pin(l, pin01, 2);
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 0);

	rr_ledger_unpin(l, pin01, 1); /* releases seg 0 only */
	size_t n = rr_ledger_prunable(l, SEC(50), idx, 4);
	assert(n == 1);
	assert(idx[0] == 0);

	rr_ledger_unpin(l, pin1, 1); /* seg 1 released too */
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 2);

	/* A double pin needs two unpin calls. */
	rr_ledger_pin(l, pin0, 1);
	rr_ledger_pin(l, pin0, 1); /* seg 0: 2 pins */
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 1);
	assert(idx[0] == 1);
	rr_ledger_unpin(l, pin0, 1); /* one pin left → still held */
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 1);
	assert(idx[0] == 1);
	rr_ledger_unpin(l, pin0, 1);
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 2);

	rr_ledger_destroy(l);
}

static void unclosed_segment_is_not_prunable(void)
{
	const uint64_t times[] = {0, SEC(10)};
	rr_ledger_t *l = rr_ledger_create(10);
	int idx[4];

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", times[0]);
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", times[1]) == 0);
	/* seg 0 ended long ago but the muxer never released the file: closed == false. */

	assert(rr_ledger_prunable(l, SEC(1000), idx, 4) == 0);

	rr_ledger_mark_closed(l, 0, MIB(4));
	assert(rr_ledger_prunable(l, SEC(1000), idx, 4) == 1);

	rr_ledger_destroy(l);
}

static void first_segment_longer_than_nominal_is_handled(void)
{
	/* 11.7 s first segment, 10 s after (E6, first segment at 60 fps). window 30
	 * → margin 45 s. */
	const uint64_t times[] = {0, SEC(117) / 10, SEC(217) / 10, SEC(317) / 10};
	rr_ledger_t *l = make_chain(30, 4, times, MIB(40));
	int idx[4];

	/* At t = 57 s seg0 is 45.3 s past its end → pruned even though it was
	 * longer than nominal; seg1 (35.3 s) stays. */
	assert(rr_ledger_prunable(l, SEC(57), idx, 4) == 1);
	assert(idx[0] == 0);

	/* A 30 s window ending at 31.7 s starts at 1.7 s; seg0 (0–11.7 s) still
	 * overlaps it and the inpoint lands inside the long first segment. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(317) / 10, 30, 0, &w));
	assert(w.first == 0);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 1.7));
	assert(w.start_ns == SEC(17) / 10);
	assert(w.end_ns == SEC(317) / 10);

	rr_ledger_destroy(l);
}

static void variable_segment_lengths_10_83s(void)
{
	/* 59.94 fps with keyint_sec=1 gives 10.83 s segments (E6); the core must
	 * use the actual end times, never a nominal 10 s. window 30 → margin 45. */
	const uint64_t times[] = {0, SEC(1083) / 100, SEC(2166) / 100, SEC(3249) / 100, SEC(4332) / 100};
	rr_ledger_t *l = make_chain(30, 5, times, MIB(40));
	int idx[4];

	/* At t = 57 s the ages past each closed end are 46.17 s / 35.34 s / 24.51 s
	 * / 13.68 s → only seg0 is past the margin. */
	assert(rr_ledger_prunable(l, SEC(57), idx, 4) == 1);
	assert(idx[0] == 0);

	/* Window at 43.32 s: starts at 13.32 s → segs 1..3; inpoint = 13.32 − 10.83
	 * = 2.49 s into seg1. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(4332) / 100, 30, 0, &w));
	assert(w.first == 1);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 2.49));
	assert(w.start_ns == SEC(1332) / 100);

	rr_ledger_destroy(l);
}

static void small_segment_is_kept(void)
{
	/* E16/D9: a 298 KB–300 KB, 1 s segment is real footage and is never
	 * filtered by size. */
	const uint64_t times[] = {0, SEC(1), SEC(11), SEC(21)};
	rr_ledger_t *l = rr_ledger_create(30);

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", 0);
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", SEC(1)) == 0);
	rr_ledger_mark_closed(l, 0, 300 * 1024); /* 300 KiB in 1 s */
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_02.mkv", SEC(11)) == 1);
	rr_ledger_mark_closed(l, 1, MIB(40));
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_03.mkv", SEC(21)) == 2);
	rr_ledger_mark_closed(l, 2, MIB(40));

	/* Selection picks it like any other segment (window covers the whole 21 s). */
	struct rr_window w;
	assert(rr_window_select(l, SEC(21), 30, 0, &w));
	assert(w.first == 0);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 0.0));

	/* It is pruned only by age: at t = 30 s it is 29 s past its end (margin 45)
	 * → kept; at t = 50 s → 49 s → pruned. Size never enters the rule (seg1 is
	 * 40× bigger and gets pruned after it, at 39 s past its end not yet). */
	int idx[4];
	assert(rr_ledger_prunable(l, SEC(30), idx, 4) == 0);
	assert(rr_ledger_prunable(l, SEC(50), idx, 4) == 1);
	assert(idx[0] == 0);

	/* Its bytes count like any other segment's. */
	assert(rr_ledger_bytes(l) == 300 * 1024 + 2 * MIB(40));

	rr_ledger_destroy(l);
}

static void window_selects_only_overlapping_closed_segments(void)
{
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30), SEC(40), SEC(50)};
	rr_ledger_t *l = make_chain(25, 6, times, MIB(40));

	/* 25 s window ending at 50 s starts at 25 s: segs 2,3,4 (ends 30/40/50)
	 * overlap; segs 0,1 ended at or before 25 s and the open seg 5 is not
	 * closed. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(50), 25, 0, &w));
	assert(w.first == 2);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 5.0)); /* 25 − 20, into seg2 */

	rr_ledger_destroy(l);
}

static void window_skips_segment_still_being_written(void)
{
	/* A segment whose release wait failed: its boundary ended it (end_ns set)
	 * but it was never marked closed — the file may still be written. It must
	 * stay out of the save even though a later segment closed normally, and
	 * first/count must stay a contiguous range of closed (readable) entries. */
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30)};
	rr_ledger_t *l = rr_ledger_create(30);

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", times[0]);
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", times[1]) == 0);
	rr_ledger_mark_closed(l, 0, MIB(40));
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_02.mkv", times[2]) == 1);
	/* seg1 stays unclosed: held by the writer past its boundary */
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_03.mkv", times[3]) == 2);
	rr_ledger_mark_closed(l, 2, MIB(40));

	/* A 30 s window ending at 30 s covers the whole ledger, but the save can
	 * only take the closed run after the held segment: seg2 alone. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(30), 30, 0, &w));
	assert(w.first == 2);
	assert(w.count == 1);
	assert(secs_eq(w.inpoint_sec, 0.0));

	/* Whatever range is selected, the save's index mapping (first .. first+
	 * count-1) must contain only closed segments. */
	for (int i = w.first; i < w.first + w.count; i++) {
		struct rr_segment_info seg;
		assert(rr_ledger_get(l, (size_t)i, &seg));
		assert(seg.closed);
	}

	/* Prune skips it the same way: only the two closed segments, never 1. */
	int idx[4];
	size_t n = rr_ledger_prunable(l, SEC(200), idx, 4);
	assert(n == 2);
	assert(idx[0] == 0 && idx[1] == 2);

	/* Once released it becomes saveable: mark it closed and the same window
	 * now takes the whole run seg0..seg2. */
	rr_ledger_mark_closed(l, 1, MIB(40));
	assert(rr_window_select(l, SEC(30), 30, 0, &w));
	assert(w.first == 0);
	assert(w.count == 3);

	rr_ledger_destroy(l);
}

static void window_inpoint_is_offset_into_first_segment(void)
{
	/* The buffer starts at 2 s: seed at 2 s, closed at 12/22/32 s. */
	const uint64_t times[] = {SEC(2), SEC(12), SEC(22), SEC(32)};
	rr_ledger_t *l = make_chain(22, 4, times, MIB(40));

	/* 22 s window ending at 32 s starts at 10 s → all three segments overlap
	 * (seg0 runs 2–12 s) and the inpoint is 10 − 2 = 8 s into seg0. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(32), 22, 0, &w));
	assert(w.first == 0);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 8.0));
	assert(w.start_ns == SEC(10));
	assert(w.end_ns == SEC(32));

	rr_ledger_destroy(l);
}

static void window_larger_than_buffer_selects_everything_inpoint_zero(void)
{
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30), SEC(40)};
	rr_ledger_t *l = make_chain(600, 5, times, MIB(40));

	/* 600 s window ending at 40 s: the start clamps to 0 (no underflow) and the
	 * whole closed buffer is selected with inpoint 0. */
	struct rr_window w;
	assert(rr_window_select(l, SEC(40), 600, 0, &w));
	assert(w.first == 0);
	assert(w.count == 4);
	assert(secs_eq(w.inpoint_sec, 0.0));
	assert(w.start_ns == 0);
	assert(w.end_ns == SEC(40));

	rr_ledger_destroy(l);
}

static void since_last_save_moves_window_start(void)
{
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30)};
	rr_ledger_t *l = make_chain(30, 4, times, MIB(40));
	struct rr_window w;

	/* Window scope: everything (start would be 0). */
	assert(rr_window_select(l, SEC(30), 30, 0, &w));
	assert(w.first == 0);
	assert(w.count == 3);
	assert(secs_eq(w.inpoint_sec, 0.0));

	/* Since the last save ended at 15 s: start moves 0 → 15 s → segs 1,2,
	 * inpoint 15 − 10 = 5 s. */
	assert(rr_window_select(l, SEC(30), 30, SEC(15), &w));
	assert(w.first == 1);
	assert(w.count == 2);
	assert(secs_eq(w.inpoint_sec, 5.0));
	assert(w.start_ns == SEC(15));
	assert(w.end_ns == SEC(30));

	/* The shift only moves the start later: a short 10 s window already starts
	 * at 20 s, past last_save_end 5 s → same selection as without it. */
	assert(rr_window_select(l, SEC(30), 10, SEC(5), &w));
	assert(w.first == 2);
	assert(w.count == 1);
	assert(secs_eq(w.inpoint_sec, 0.0));

	/* Nothing new was closed since the last save at 30 s → no overlap → false
	 * ("nothing to save"). */
	assert(!rr_window_select(l, SEC(30), 30, SEC(30), &w));

	rr_ledger_destroy(l);
}

static void saved_end_advances_only_over_written_footage(void)
{
	/* The round-2 checkup probe, on the real core: seg0 [0,10) closed,
	 * seg1 [10,20) ended NOT closed (release wait failed), seg2 [20,30)
	 * closed; 30 s window ending at 30 s. The run stops at the hole and
	 * covers seg2 alone — footage below the hole is in the window but not
	 * in the clip. */
	const uint64_t times[] = {0, SEC(10), SEC(20), SEC(30)};
	rr_ledger_t *l = rr_ledger_create(30);

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", times[0]);
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", times[1]) == 0);
	rr_ledger_mark_closed(l, 0, MIB(40));
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_02.mkv", times[2]) == 1); /* seg1 never released yet */
	assert(rr_ledger_boundary(l, "C:/buf/rrseg_03.mkv", times[3]) == 2);
	rr_ledger_mark_closed(l, 2, MIB(40));

	struct rr_window w;
	assert(rr_window_select(l, SEC(30), 30, 0, &w));
	assert(w.first == 2 && w.count == 1);

	/* The clip does not reach back to the window start while older
	 * in-window segments stay unsaved: the since-last-save boundary must
	 * NOT move (0) — advancing it would skip seg0/seg1 forever. */
	assert(rr_window_saved_end(l, &w, 0) == 0);

	/* Once seg1 releases (the idle retry closes it), a save from the
	 * unchanged boundary takes the whole run again — seg1 included. */
	rr_ledger_mark_closed(l, 1, MIB(40));
	assert(rr_window_select(l, SEC(30), 30, 0, &w));
	assert(w.first == 0 && w.count == 3);
	assert(rr_window_saved_end(l, &w, 0) == SEC(30));
	rr_ledger_destroy(l);

	/* Normal full run, nothing skipped: the boundary may reach window_end
	 * (unchanged behavior). */
	rr_ledger_t *l2 = make_chain(30, 4, times, MIB(40));
	assert(rr_window_select(l2, SEC(30), 30, 0, &w));
	assert(w.first == 0 && w.count == 3);
	assert(rr_window_saved_end(l2, &w, 0) == SEC(30));

	/* skipped_tail: rr_remux left the newest selected segment out of the
	 * clip (unreadable stub) — only the footage up to the predecessor's
	 * end was written, and the stub stays ahead of the next save's start. */
	assert(rr_window_saved_end(l2, &w, 1) == SEC(20));

	/* Truncated at the newest end (the hold-scenario shape): a run that
	 * reaches the window start but stops below window_end at an unreleased
	 * segment above it — advance to the run's end so the held segment is
	 * offered to the next save once it releases. */
	w.first = 0;
	w.count = 2;
	assert(rr_window_saved_end(l2, &w, 0) == SEC(20));

	/* Degenerate and out-of-range shapes: a single-segment save cannot
	 * skip its own tail (the remux fails instead), and a range outside
	 * the ledger yields 0 = do not advance. */
	w.start_ns = SEC(20);
	w.end_ns = SEC(30);
	w.first = 2;
	w.count = 1;
	assert(rr_window_saved_end(l2, &w, 1) == SEC(30));
	w.first = 5;
	assert(rr_window_saved_end(l2, &w, 0) == 0);

	rr_ledger_destroy(l2);
}

static void buffered_seconds_is_capped_at_window(void)
{
	rr_ledger_t *l = rr_ledger_create(10);

	assert(secs_eq(rr_ledger_buffered_sec(l, SEC(5)), 0.0)); /* empty ledger */

	rr_ledger_seed(l, "C:/buf/rrseg_00.mkv", 0);
	rr_ledger_boundary(l, "C:/buf/rrseg_01.mkv", SEC(5));

	assert(secs_eq(rr_ledger_buffered_sec(l, SEC(5)), 5.0));   /* under the cap */
	assert(secs_eq(rr_ledger_buffered_sec(l, SEC(12)), 10.0)); /* capped */
	assert(secs_eq(rr_ledger_buffered_sec(l, SEC(1000)), 10.0));

	rr_ledger_destroy(l);
}

static void space_estimate_and_preflight(void)
{
	/* 30 Mbps × 1800 s = 6.75e9 bytes (handoff section 10: ≈ 6.8 GB). */
	assert(rr_space_estimate_bytes(30000, 1800) == 6750000000LL);
	assert(rr_space_estimate_bytes(1, 1) == 125LL); /* 1 kbit = 125 bytes */

	const int64_t gib = 1073741824;
	const int64_t est = 1000000000;

	/* Same drive: 2 × estimate + 1 GiB needed on the shared drive. */
	assert(rr_space_preflight(est, 2 * est + gib, 2 * est + gib, true));
	assert(!rr_space_preflight(est, 2 * est + gib - 1, 2 * est + gib, true));
	assert(!rr_space_preflight(est, 2 * est + gib, 2 * est + gib - 1, true));
	assert(!rr_space_preflight(est, est + gib, est + gib, true)); /* 1× is not enough */

	/* Split drives: 1 × estimate + 1 GiB on each. */
	assert(rr_space_preflight(est, est + gib, est + gib, false));
	assert(!rr_space_preflight(est, est + gib - 1, est + gib, false));
	assert(!rr_space_preflight(est, est + gib, est + gib - 1, false));
}

static void buffer_dir_drive_root_is_refused(void)
{
	assert(rr_paths_check_buffer_dir("C:\\", "C:\\Users\\<username>\\Record") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_buffer_dir("D:/", "D:\\videos") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_buffer_dir("C:\\\\", "C:\\out") == RR_PATH_DRIVE_ROOT);

	/* Right next to a root but acceptable. */
	assert(rr_paths_check_buffer_dir("C:\\buffer", "C:\\Users\\<username>\\Record") == RR_PATH_OK);
	assert(rr_paths_check_buffer_dir("D:/videos/.rr-buffer", "D:\\videos") == RR_PATH_OK);
}

static void buffer_dir_equal_to_output_dir_is_refused(void)
{
	assert(rr_paths_check_buffer_dir("C:\\Users\\<username>\\Record", "c:\\users\\<username>\\record\\") ==
	       RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("c:/users/<username>/record", "C:\\Users\\<username>\\Record") ==
	       RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:\\Users\\<username>\\Record\\.rr-buffer",
					 "C:\\Users\\<username>\\Record") == RR_PATH_OK);

	/* Other refusals of the same checker. */
	assert(rr_paths_check_buffer_dir("", "C:\\out") == RR_PATH_EMPTY);
	assert(rr_paths_check_buffer_dir(NULL, "C:\\out") == RR_PATH_EMPTY);
	assert(rr_paths_check_buffer_dir("buffer", "C:\\out") == RR_PATH_NOT_ABSOLUTE);
	assert(rr_paths_check_buffer_dir("C:buffer", "C:\\out") == RR_PATH_NOT_ABSOLUTE); /* drive-relative */

	/* An empty output dir ("OBS recording path", resolved by the caller) skips
	 * only the equality check. */
	assert(rr_paths_check_buffer_dir("C:\\buffer", "") == RR_PATH_OK);
	assert(rr_paths_check_buffer_dir("C:\\", "") == RR_PATH_DRIVE_ROOT);
}

static void buffer_dir_dot_segments_are_folded(void)
{
	/* A spelling that resolves to the output folder is refused, whatever
	 * '.', '..' or slash style it hides behind. */
	assert(rr_paths_check_buffer_dir("C:\\rec\\..\\rec", "C:\\rec") == RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:/rec/../rec", "C:\\rec\\") == RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:\\rec\\.", "C:\\rec") == RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:\\rec\\sub\\..", "C:\\rec") == RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:\\rec\\..\\..\\rec", "C:\\rec") == RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("C:\\\\rec\\\\x\\\\..\\\\y", "C:\\rec\\y") == RR_PATH_SAME_AS_OUTPUT);

	/* '..' clamps at the root: that is a drive root, not a hidden folder. */
	assert(rr_paths_check_buffer_dir("C:\\rec\\..", "C:\\out") == RR_PATH_DRIVE_ROOT);

	/* Different folders after folding stay OK. */
	assert(rr_paths_check_buffer_dir("C:\\rec\\..\\other", "C:\\rec") == RR_PATH_OK);
	assert(rr_paths_check_buffer_dir("D:\\a\\b\\..\\c", "D:\\a\\b") == RR_PATH_OK);

	/* UNC spellings fold too; '..' cannot climb out of the share. */
	assert(rr_paths_check_buffer_dir("\\\\server\\share\\a\\..\\b", "\\\\server\\share\\b") ==
	       RR_PATH_SAME_AS_OUTPUT);
	assert(rr_paths_check_buffer_dir("\\\\server\\share\\..\\b", "\\\\server\\share\\b") == RR_PATH_SAME_AS_OUTPUT);

	/* Degenerate spellings fall back to the raw comparison (today's rule). */
	assert(rr_paths_check_buffer_dir("\\\\server", "C:\\out") == RR_PATH_OK);
}

static void output_dir_absolute_not_drive_root(void)
{
	/* Empty means "the OBS recording path", resolved by the caller. */
	assert(rr_paths_check_output_dir("") == RR_PATH_OK);
	assert(rr_paths_check_output_dir(NULL) == RR_PATH_OK);

	/* Ordinary folders, in every accepted spelling. */
	assert(rr_paths_check_output_dir("C:\\Users\\<username>\\Record") == RR_PATH_OK);
	assert(rr_paths_check_output_dir("D:/videos/.rr-out") == RR_PATH_OK);
	assert(rr_paths_check_output_dir("\\\\server\\share\\record") == RR_PATH_OK);
	assert(rr_paths_check_output_dir("/home/user/record") == RR_PATH_OK);

	/* A drive root would be purged and written into as if it were a folder. */
	assert(rr_paths_check_output_dir("C:\\") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_output_dir("D:/") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_output_dir("C:\\\\") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_output_dir("C:\\videos\\..") == RR_PATH_DRIVE_ROOT);
	assert(rr_paths_check_output_dir("/") == RR_PATH_DRIVE_ROOT);

	/* Relative spellings act on the process working directory (OBS's install
	 * directory for obs64), so they are refused — "." most of all, the value
	 * a crafted settings.json carries. */
	assert(rr_paths_check_output_dir(".") == RR_PATH_NOT_ABSOLUTE);
	assert(rr_paths_check_output_dir("..") == RR_PATH_NOT_ABSOLUTE);
	assert(rr_paths_check_output_dir("out") == RR_PATH_NOT_ABSOLUTE);
	assert(rr_paths_check_output_dir("out\\sub") == RR_PATH_NOT_ABSOLUTE);
	assert(rr_paths_check_output_dir("C:out") == RR_PATH_NOT_ABSOLUTE); /* drive-relative */
}

static void segment_name_pattern(void)
{
	assert(rr_paths_is_segment_name("rrseg_x.mkv"));
	assert(!rr_paths_is_segment_name("Replay_x.mp4"));
	assert(!rr_paths_is_segment_name("x.mkv"));
	assert(!rr_paths_is_segment_name("rrseg_x.mp4"));
	assert(!rr_paths_is_segment_name("rrseg_.mkv"));      /* no middle */
	assert(!rr_paths_is_segment_name("rrseg_x.mkv.txt")); /* wrong suffix */
	assert(!rr_paths_is_segment_name("dir/rrseg_x.mkv")); /* not a basename */
	assert(!rr_paths_is_segment_name("RRSEG_x.mkv"));     /* exact prefix (D12) */
	assert(rr_paths_is_segment_name("rrseg_2026-11-12_05-53-14.mkv"));

	assert(rr_paths_is_part_name("Replay_2026-11-12_05-53-14.mp4.rrpart"));
	assert(!rr_paths_is_part_name("Replay_x.mp4"));
	assert(!rr_paths_is_part_name(".rrpart")); /* no stem */
	assert(!rr_paths_is_part_name("x.rrpartx"));
	assert(!rr_paths_is_part_name("out/.rrpart"));
	assert(!rr_paths_is_part_name("rrseg_x.mkv"));
}

static void save_filename_must_be_bare(void)
{
	/* Bare names, including the 5.8 default format (with % tokens). */
	assert(rr_paths_is_safe_filename("Replay_2026-11-13_06-58-43.mp4"));
	assert(rr_paths_is_safe_filename("Replay_%CCYY-%MM-%DD_%hh-%mm-%ss.mp4"));
	assert(rr_paths_is_safe_filename("my replay (2).mp4"));

	/* Path separators and drive/stream colons escape the output folder. */
	assert(!rr_paths_is_safe_filename("..\\Replay_escape"));
	assert(!rr_paths_is_safe_filename("../Replay_escape"));
	assert(!rr_paths_is_safe_filename("sub\\Replay"));
	assert(!rr_paths_is_safe_filename("sub/Replay"));
	assert(!rr_paths_is_safe_filename("C:\\Replay"));
	assert(!rr_paths_is_safe_filename("C:Replay_stream"));
	assert(!rr_paths_is_safe_filename("\\\\server\\share\\Replay"));

	/* Current/parent directory as the whole name. */
	assert(!rr_paths_is_safe_filename("."));
	assert(!rr_paths_is_safe_filename(".."));
	assert(rr_paths_is_safe_filename("..mp4")); /* a benign name, not a dir ref */

	assert(!rr_paths_is_safe_filename(""));
	assert(!rr_paths_is_safe_filename(NULL));
}

static void window_seconds_clamped_30_1800(void)
{
	assert(rr_settings_clamp_window_sec(10) == 30);
	assert(rr_settings_clamp_window_sec(29) == 30);
	assert(rr_settings_clamp_window_sec(30) == 30);
	assert(rr_settings_clamp_window_sec(900) == 900);
	assert(rr_settings_clamp_window_sec(1800) == 1800);
	assert(rr_settings_clamp_window_sec(1801) == 1800);
	assert(rr_settings_clamp_window_sec(0) == 30);
	assert(rr_settings_clamp_window_sec(-100) == 30);
}

int main(void)
{
	RUN(boundary_closes_open_and_opens_next_at_same_instant);
	RUN(open_segment_is_never_prunable);
	RUN(prune_takes_closed_segments_past_window_plus_margin);
	RUN(pinned_segment_is_not_prunable_until_unpinned);
	RUN(unclosed_segment_is_not_prunable);
	RUN(first_segment_longer_than_nominal_is_handled);
	RUN(variable_segment_lengths_10_83s);
	RUN(small_segment_is_kept);
	RUN(window_selects_only_overlapping_closed_segments);
	RUN(window_skips_segment_still_being_written);
	RUN(window_inpoint_is_offset_into_first_segment);
	RUN(window_larger_than_buffer_selects_everything_inpoint_zero);
	RUN(since_last_save_moves_window_start);
	RUN(saved_end_advances_only_over_written_footage);
	RUN(buffered_seconds_is_capped_at_window);
	RUN(space_estimate_and_preflight);
	RUN(buffer_dir_drive_root_is_refused);
	RUN(buffer_dir_equal_to_output_dir_is_refused);
	RUN(buffer_dir_dot_segments_are_folded);
	RUN(output_dir_absolute_not_drive_root);
	RUN(segment_name_pattern);
	RUN(save_filename_must_be_bare);
	RUN(window_seconds_clamped_30_1800);

	printf("all tests passed\n");
	return 0;
}
