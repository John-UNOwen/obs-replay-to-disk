/* ledger.h — the plugin's in-memory list of segments (handoff 5.10).
 * Pure C: no libobs. The ledger never reads a clock or the disk: every time
 * is passed in by the caller (the worker thread, monotonic clock, D7).
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rr_ledger rr_ledger_t;

struct rr_segment_info {
	const char *path;          /* owned by the ledger; valid until the ledger changes */
	uint64_t start_ns, end_ns; /* end 0 = open */
	bool closed;               /* the file is closed for reading (D8), not merely ended */
	int64_t bytes;             /* file size, recorded when it became closed */
	int pins;                  /* > 0 while a save is reading the segment */
};

rr_ledger_t *rr_ledger_create(int window_sec);
void rr_ledger_destroy(rr_ledger_t *l);
void rr_ledger_seed(rr_ledger_t *l, const char *first_path, uint64_t now_ns);
/* Ends the open segment at t_ns and opens next_path at t_ns (same instant, D7).
 * Returns the index of the ended one, or -1 if no segment was open. */
int rr_ledger_boundary(rr_ledger_t *l, const char *next_path, uint64_t t_ns);
void rr_ledger_mark_closed(rr_ledger_t *l, int index, int64_t bytes);
size_t rr_ledger_count(const rr_ledger_t *l);
bool rr_ledger_get(const rr_ledger_t *l, size_t index, struct rr_segment_info *out);
/* Fills idx[] with segments to delete now (rule D10: closed, not pinned, and
 * end strictly older than now_ns - (window_sec + 15 s); never the open
 * segment; never filtered by size, D9). Returns how many (at most max). */
size_t rr_ledger_prunable(const rr_ledger_t *l, uint64_t now_ns, int *idx, size_t max);
void rr_ledger_remove(rr_ledger_t *l, const char *path); /* after a successful delete */
void rr_ledger_pin(rr_ledger_t *l, const int *idx, size_t n);
void rr_ledger_unpin(rr_ledger_t *l, const int *idx, size_t n);
double rr_ledger_buffered_sec(const rr_ledger_t *l, uint64_t now_ns); /* capped at window */
int64_t rr_ledger_bytes(const rr_ledger_t *l);                        /* closed segments only */
