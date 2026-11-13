/* test_rename.c — tests for the exclusive rename (D13: the save rename must
 * never clobber a file that appeared between the name check and the rename).
 * Plain assert, test_core.c style. Runs against the real file system in the
 * OS temp folder.
 * License: GPL-2.0-or-later.
 */
#include "plugin/rename.h"

/* RelWithDebInfo builds define NDEBUG; these tests assert regardless of the
 * build configuration, so the assert header is included with NDEBUG undef. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define RUN(test)                 \
	do {                      \
		printf(#test "\n");   \
		fflush(stdout);       \
		test();             \
	} while (0)

static char dir[512]; /* OS temp folder, no trailing separator */
static char p_src[512], p_dst[512], p_missing[512];

static void tmp_dir(char *buf, size_t sz)
{
#ifdef _WIN32
	const DWORD n = GetTempPathA((DWORD)sz, buf);
	assert(n > 0 && n < sz);
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\\' || buf[len - 1] == '/'))
		buf[--len] = 0;
#else
	snprintf(buf, sz, "/tmp");
#endif
}

static void setup_paths(void)
{
	tmp_dir(dir, sizeof(dir));
#ifdef _WIN32
	const unsigned long pidv = GetCurrentProcessId();
#else
	const unsigned long pidv = (unsigned long)getpid();
#endif
	char leaf[128];
	snprintf(leaf, sizeof(leaf), "rr_rename_test_%lu", pidv);
	snprintf(p_src, sizeof(p_src), "%s/%s_src.dat", dir, leaf);
	snprintf(p_dst, sizeof(p_dst), "%s/%s_dst.dat", dir, leaf);
	snprintf(p_missing, sizeof(p_missing), "%s/%s_missing_%lu.dat", dir, leaf, pidv + 1u);
	remove(p_src);
	remove(p_dst);
}

static void cleanup_paths(void)
{
	remove(p_src);
	remove(p_dst);
}

static void write_file(const char *path, const char *text)
{
#ifdef _WIN32
	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	assert(h != INVALID_HANDLE_VALUE);
	DWORD wr = 0;
	const DWORD len = (DWORD)strlen(text);
	assert(WriteFile(h, text, len, &wr, NULL));
	assert(wr == len);
	assert(CloseHandle(h));
#else
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fputs(text, f) >= 0);
	assert(fclose(f) == 0);
#endif
}

static bool file_has_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	char buf[64];
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[got] = 0;
	return strcmp(buf, text) == 0;
}

static bool exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* A free destination takes the move: source gone, content preserved. */
static void moves_a_file_when_the_target_is_free(void)
{
	setup_paths();
	write_file(p_src, "SOURCE");
	assert(exists(p_src));
	assert(rr_rename_exclusive(p_src, p_dst) == 0);
	assert(!exists(p_src));
	assert(file_has_text(p_dst, "SOURCE"));
	cleanup_paths();
}

/* The race the fix is about: the destination appeared after the earlier
 * name check. The rename must refuse with 1, leave the destination's content
 * intact, and keep the source (the caller then takes the next _N name). */
static void refuses_an_existing_target_without_clobber(void)
{
	setup_paths();
	write_file(p_src, "SOURCE");
	write_file(p_dst, "PRE-EXISTING");
	assert(rr_rename_exclusive(p_src, p_dst) == 1);
	assert(exists(p_src)); /* source untouched: the caller retries elsewhere */
	assert(file_has_text(p_src, "SOURCE"));
	assert(file_has_text(p_dst, "PRE-EXISTING")); /* not replaced */
	cleanup_paths();
}

/* A vanished source is a plain failure (-1), not a collision, and creates
 * nothing. */
static void fails_on_a_missing_source(void)
{
	setup_paths();
	assert(!exists(p_missing));
	assert(rr_rename_exclusive(p_missing, p_dst) == -1);
	assert(!exists(p_dst));
	cleanup_paths();
}

int main(void)
{
	RUN(moves_a_file_when_the_target_is_free);
	RUN(refuses_an_existing_target_without_clobber);
	RUN(fails_on_a_missing_source);
	printf("all tests passed\n");
	return 0;
}
