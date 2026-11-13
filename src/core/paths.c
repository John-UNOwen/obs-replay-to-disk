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

enum rr_path_error rr_paths_check_buffer_dir(const char *buffer_dir, const char *output_dir)
{
	if (!buffer_dir || !buffer_dir[0])
		return RR_PATH_EMPTY;
	if (!path_is_absolute(buffer_dir))
		return RR_PATH_NOT_ABSOLUTE;
	if (path_is_drive_root(buffer_dir))
		return RR_PATH_DRIVE_ROOT;
	/* An empty output_dir means "OBS's recording path", resolved by the caller;
	 * it cannot be compared here. */
	if (output_dir && output_dir[0] && paths_equal_ci(buffer_dir, output_dir))
		return RR_PATH_SAME_AS_OUTPUT;
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
