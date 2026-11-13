/* remux.h — interface of save/remux.c. Design notes and remux test results
 * are in docs/wp1-findings.md and docs/STATUS.md.
 * The struct definitions mirror the declarations at the top of remux.c exactly.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define RR_MAX_STREAMS 8 /* 1 video + up to 6 audio, with headroom */

struct rr_remux_params {
	const char *const *segments; /* absolute paths, oldest first, all closed */
	int num_segments;
	const char *out_path; /* final .mp4 path; caller handles .part + rename */
	int fps_num, fps_den; /* OBS video fps at record time */
	double inpoint_sec;   /* seconds into segment 0 to start at; 0 = from its start */
	/* optional: return non-zero from cancel() to abort */
	int (*cancel)(void *ud);
	void (*progress)(void *ud, int segment_index, int num_segments);
	void *ud;
};

struct rr_remux_result {
	int64_t video_frames;
	int64_t audio_packets[RR_MAX_STREAMS];
	double duration_sec; /* video_frames / fps */
	int reorder_delay;   /* frames */
	int grid_warnings;   /* packets whose pts was not on the expected grid */
	int skipped_tail;    /* 1 if the last segment was an unreadable stub and was left out */
};

/* Joins the split MKV segments into one MP4 by stream copy with regenerated
 * timestamps. Returns 0 on success, a negative AVERROR on failure (message in
 * err). On failure the partly written out_path is left in place; the caller
 * deletes it. */
int rr_remux(const struct rr_remux_params *p, struct rr_remux_result *res, char *err, size_t errlen);
