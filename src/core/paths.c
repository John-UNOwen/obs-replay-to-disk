/* paths.c — folder validation and filename matching. See paths.h.
 * License: GPL-2.0-or-later.
 */
#include "paths.h"

#include <ctype.h>
#include <string.h>

static bool is_sep(char c)
{
	return c == '/' || c == '\\';
}

static char norm_char(char c)
{
	if (c == '\\')
		return '/';
	return (char)tolower((unsigned char)c);
}

/* Windows semantics: drive-absolute ("C:\..."), UNC ("\\...") — plus a
 * leading '/' as rooted, so the module stays testable on POSIX. */
static bool path_is_absolute(const char *p)
{
	if (is_sep(p[0]))
		return true;
	return isalpha((unsigned char)p[0]) && p[1] == ':' && is_sep(p[2]);
}

/* True for "C:\", "C:/", "D:\" (also with extra trailing slashes) and "/".
 * "C:" alone is drive-relative, not a root. */
static bool path_is_drive_root(const char *p)
{
	const char *rest;

	if (is_sep(p[0])) {
		if (p[0] == '\\')
			return p[1] == 0; /* "\\" alone; "\\server\share" is UNC, not a root */
		rest = p + 1;
	} else {
		rest = p + 2; /* C: */
	}

	while (*rest && is_sep(*rest))
		rest++;
	return *rest == 0;
}

static size_t trimmed_len(const char *p)
{
	size_t n = strlen(p);
	while (n > 1 && is_sep(p[n - 1]))
		n--;
	return n;
}

/* Case-insensitive comparison with '/' and '\\' interchangeable and trailing
 * separators ignored. */
static bool paths_equal_ci(const char *a, const char *b)
{
	size_t na = trimmed_len(a), nb = trimmed_len(b);
	if (na != nb)
		return false;
	for (size_t i = 0; i < na; i++) {
		if (norm_char(a[i]) != norm_char(b[i]))
			return false;
	}
	return true;
}

enum { RR_PATH_CANON_MAX = 4096 };

/* Canonical spelling for comparison: one separator style per root kind, empty
 * and "." segments dropped, ".." pops a segment and clamps at the root
 * (Windows: "cd .." on "C:\" stays on "C:\", out of "\\server\share" stays in
 * the share). Keeps the root spelling: "C:\", "\\server\share", "/". Purely
 * textual — like GetFullPathNameW, no short-name or case resolution, and no
 * CWD substitution for relative paths. Returns false for relative, over-long
 * or degenerate input; the caller then compares the raw text as before. */
static bool paths_canonicalize(char *out, size_t out_size, const char *in)
{
#define RR_MAX_SEGS 1024
	const char *seg[RR_MAX_SEGS];
	size_t seg_len[RR_MAX_SEGS];
	size_t nseg = 0, nprot = 0;
	const char *p = in;
	char sep = '\\';
	bool unc = false, drive = false;

	if (is_sep(p[0]) && is_sep(p[1])) {
		unc = true;
		p += 2;
	} else if (is_sep(p[0])) {
		sep = '/';
		p += 1;
	} else if (path_is_absolute(p)) {
		drive = true;
		p += 3; /* path_is_absolute() guarantees "C:\" */
	} else {
		return false;
	}

	while (*p) {
		while (is_sep(*p))
			p++;
		const char *start = p;
		while (*p && !is_sep(*p))
			p++;
		const size_t len = (size_t)(p - start);

		if (len == 0)
			continue;
		const bool dot = len == 1 && start[0] == '.';
		const bool dotdot = len == 2 && start[0] == '.' && start[1] == '.';
		if (dot || dotdot) {
			if (unc && nseg < 2)
				return false; /* "." or ".." where server\share belongs */
			if (dotdot && nseg > nprot)
				nseg--;
			continue;
		}
		if (nseg == RR_MAX_SEGS)
			return false;
		seg[nseg] = start;
		seg_len[nseg] = len;
		nseg++;
		if (unc && nseg == 2)
			nprot = 2; /* ".." may not climb out of "\\server\share" */
	}
	if (unc && nseg < 2)
		return false;

	size_t w = 0;
	if (drive) {
		if (out_size < 4)
			return false;
		out[w++] = in[0];
		out[w++] = ':';
		out[w++] = '\\';
	} else if (unc) {
		if (out_size < 3)
			return false;
		out[w++] = '\\';
		out[w++] = '\\';
	} else {
		if (out_size < 2)
			return false;
		out[w++] = '/';
	}
	for (size_t i = 0; i < nseg; i++) {
		if (i > 0) {
			if (w + 1 >= out_size)
				return false;
			out[w++] = sep;
		}
		if (w + seg_len[i] >= out_size)
			return false;
		memcpy(out + w, seg[i], seg_len[i]);
		w += seg_len[i];
	}
	out[w] = 0;
	return true;
#undef RR_MAX_SEGS
}

enum rr_path_error rr_paths_check_buffer_dir(const char *buffer_dir, const char *output_dir)
{
	char buf_canon[RR_PATH_CANON_MAX], out_canon[RR_PATH_CANON_MAX];

	if (!buffer_dir || !buffer_dir[0])
		return RR_PATH_EMPTY;
	if (!path_is_absolute(buffer_dir))
		return RR_PATH_NOT_ABSOLUTE;

	/* Compare canonical spellings so "C:\rec\..\rec" cannot hide the output
	 * folder — or itself hide a drive root — from the checks below. */
	const char *buf = buf_canon;
	if (!paths_canonicalize(buf_canon, sizeof(buf_canon), buffer_dir))
		buf = buffer_dir;
	if (path_is_drive_root(buf))
		return RR_PATH_DRIVE_ROOT;

	/* An empty output_dir means "OBS's recording path", resolved by the caller;
	 * it cannot be compared here. */
	if (output_dir && output_dir[0]) {
		const char *out = out_canon;
		if (!paths_canonicalize(out_canon, sizeof(out_canon), output_dir))
			out = output_dir;
		if (paths_equal_ci(buf, out))
			return RR_PATH_SAME_AS_OUTPUT;
	}
	return RR_PATH_OK;
}

enum rr_path_error rr_paths_check_output_dir(const char *output_dir)
{
	/* Empty = "the OBS recording path", resolved by the caller. */
	if (!output_dir || !output_dir[0])
		return RR_PATH_OK;
	if (!path_is_absolute(output_dir))
		return RR_PATH_NOT_ABSOLUTE;

	/* Same canonical spelling as the buffer check, so a folder that folds
	 * away ("D:\videos\..") cannot pass as anything but the drive root it
	 * is. */
	char canon[RR_PATH_CANON_MAX];
	const char *out = canon;
	if (!paths_canonicalize(canon, sizeof(canon), output_dir))
		out = output_dir;
	if (path_is_drive_root(out))
		return RR_PATH_DRIVE_ROOT;
	return RR_PATH_OK;
}

/* Basenames only: any slash makes it not a file name. Prefix and extension are
 * matched exactly (D12/D13 always produce lower case). */
bool rr_paths_is_segment_name(const char *file_name)
{
	if (!file_name)
		return false;

	static const char prefix[] = "rrseg_";
	static const char ext[] = ".mkv";
	const size_t prefix_len = sizeof(prefix) - 1, ext_len = sizeof(ext) - 1;

	if (strncmp(file_name, prefix, prefix_len) != 0)
		return false;

	size_t n = strlen(file_name);
	if (n <= prefix_len + ext_len)
		return false; /* no middle */
	if (strcmp(file_name + n - ext_len, ext) != 0)
		return false;

	for (const char *p = file_name; *p; p++) {
		if (is_sep(*p))
			return false;
	}
	return true;
}

bool rr_paths_is_safe_filename(const char *name)
{
	if (!name || !name[0])
		return false;

	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == ':')
			return false;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return false;
	return true;
}

bool rr_paths_is_part_name(const char *file_name)
{
	if (!file_name)
		return false;

	static const char ext[] = ".rrpart";
	const size_t ext_len = sizeof(ext) - 1;

	size_t n = strlen(file_name);
	if (n <= ext_len)
		return false; /* ".rrpart" alone has no stem */
	if (strcmp(file_name + n - ext_len, ext) != 0)
		return false;

	for (const char *p = file_name; *p; p++) {
		if (is_sep(*p))
			return false;
	}
	return true;
}
