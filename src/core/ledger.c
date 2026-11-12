/* ledger.c — the plugin's in-memory list of segments. See ledger.h.
 * License: GPL-2.0-or-later.
 */
#include "ledger.h"

#include <stdlib.h>
#include <string.h>

/* D10: the prune margin beyond the window, covering one segment plus the
 * long first segment (E6). */
#define RR_PRUNE_MARGIN_NS (15ULL * 1000000000ULL)

struct rr_segment {
	char *path; /* owned copy */
	uint64_t start_ns, end_ns;
	bool closed;
	int64_t bytes;
	int pins;
};

struct rr_ledger {
	int window_sec;
	struct rr_segment *items;
	size_t count, cap;
};

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p)
		abort(); /* the ledger is small and fixed-shape; an OOM here is fatal anyway */
	return p;
}

static char *dup_path(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

rr_ledger_t *rr_ledger_create(int window_sec)
{
	rr_ledger_t *l = xmalloc(sizeof(*l));
	l->window_sec = window_sec;
	l->items = NULL;
	l->count = 0;
	l->cap = 0;
	return l;
}

void rr_ledger_destroy(rr_ledger_t *l)
{
	if (!l)
		return;
	for (size_t i = 0; i < l->count; i++)
		free(l->items[i].path);
	free(l->items);
	free(l);
}

static void ledger_append(rr_ledger_t *l, const char *path, uint64_t start_ns)
{
	if (l->count == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 16;
		struct rr_segment *grown = realloc(l->items, l->cap * sizeof(*grown));
		if (!grown)
			abort();
		l->items = grown;
	}
	struct rr_segment *seg = &l->items[l->count++];
	seg->path = dup_path(path);
	seg->start_ns = start_ns;
	seg->end_ns = 0; /* open */
	seg->closed = false;
	seg->bytes = 0;
	seg->pins = 0;
}

void rr_ledger_seed(rr_ledger_t *l, const char *first_path, uint64_t now_ns)
{
	ledger_append(l, first_path, now_ns);
}

int rr_ledger_boundary(rr_ledger_t *l, const char *next_path, uint64_t t_ns)
{
	int ended = -1;
	if (l->count > 0) {
		struct rr_segment *last = &l->items[l->count - 1];
		if (last->end_ns == 0) {
			last->end_ns = t_ns;
			ended = (int)(l->count - 1);
		}
	}
	ledger_append(l, next_path, t_ns); /* the new segment starts at the same instant (D7) */
	return ended;
}

void rr_ledger_mark_closed(rr_ledger_t *l, int index, int64_t bytes)
{
	if (index < 0 || (size_t)index >= l->count)
		return;
	l->items[index].closed = true;
	l->items[index].bytes = bytes;
}

size_t rr_ledger_count(const rr_ledger_t *l)
{
	return l->count;
}

bool rr_ledger_get(const rr_ledger_t *l, size_t index, struct rr_segment_info *out)
{
	if (index >= l->count)
		return false;
	const struct rr_segment *seg = &l->items[index];
	out->path = seg->path;
	out->start_ns = seg->start_ns;
	out->end_ns = seg->end_ns;
	out->closed = seg->closed;
	out->bytes = seg->bytes;
	out->pins = seg->pins;
	return true;
}

size_t rr_ledger_prunable(const rr_ledger_t *l, uint64_t now_ns, int *idx, size_t max)
{
	uint64_t margin = (uint64_t)(l->window_sec > 0 ? l->window_sec : 0) * 1000000000ULL + RR_PRUNE_MARGIN_NS;
	size_t n = 0;

	for (size_t i = 0; i < l->count && n < max; i++) {
		const struct rr_segment *seg = &l->items[i];
		if (seg->end_ns == 0)
			continue; /* the open segment is never pruned (D10) */
		if (!seg->closed)
			continue; /* the muxer may still be writing it */
		if (seg->pins > 0)
			continue; /* a save is reading it */
		/* D10: pruned only when end is strictly older than now − margin;
		 * size never matters (D9). */
		if (now_ns > margin && seg->end_ns < now_ns - margin)
			idx[n++] = (int)i;
	}
	return n;
}

void rr_ledger_remove(rr_ledger_t *l, const char *path)
{
	for (size_t i = 0; i < l->count; i++) {
		if (strcmp(l->items[i].path, path) != 0)
			continue;
		free(l->items[i].path);
		memmove(&l->items[i], &l->items[i + 1], (l->count - i - 1) * sizeof(*l->items));
		l->count--;
		return;
	}
}

void rr_ledger_pin(rr_ledger_t *l, const int *idx, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (idx[i] >= 0 && (size_t)idx[i] < l->count)
			l->items[idx[i]].pins++;
	}
}

void rr_ledger_unpin(rr_ledger_t *l, const int *idx, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (idx[i] >= 0 && (size_t)idx[i] < l->count && l->items[idx[i]].pins > 0)
			l->items[idx[i]].pins--;
	}
}

double rr_ledger_buffered_sec(const rr_ledger_t *l, uint64_t now_ns)
{
	if (l->count == 0)
		return 0.0;

	uint64_t first_start = l->items[0].start_ns;
	double sec = now_ns > first_start ? (double)(now_ns - first_start) / 1e9 : 0.0;
	return sec < (double)l->window_sec ? sec : (double)l->window_sec;
}

int64_t rr_ledger_bytes(const rr_ledger_t *l)
{
	int64_t total = 0;
	for (size_t i = 0; i < l->count; i++) {
		if (l->items[i].closed)
			total += l->items[i].bytes;
	}
	return total;
}
