/* rename.c — exclusive file rename (D13: the finished save must never
 * overwrite an existing file). libobs os_rename is MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING (obs-studio 33.0.1
 * libobs/util/platform-windows.c:743), so it silently replaces the
 * destination; the stat-then-os_rename sequence in the save flow leaves a
 * window where a same-named file that appears mid-remux gets clobbered.
 * License: GPL-2.0-or-later.
 */
#include "rename.h"

#include <stdlib.h>

#ifdef _WIN32

#include <windows.h>

static wchar_t *dup_wide(const char *utf8)
{
	const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (need <= 0)
		return NULL;
	wchar_t *w = malloc((size_t)need * sizeof(wchar_t));
	if (!w)
		return NULL;
	if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, need) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

int rr_rename_exclusive(const char *from_utf8, const char *to_utf8)
{
	wchar_t *from = dup_wide(from_utf8);
	wchar_t *to = dup_wide(to_utf8);
	if (!from || !to) {
		free(from);
		free(to);
		return -1;
	}
	/* dwFlags 0: no MOVEFILE_REPLACE_EXISTING, so an existing destination
	 * fails the move instead of being replaced. Same-volume only, which
	 * the write-part-then-rename-in-the-same-folder save flow always is. */
	const BOOL moved = MoveFileExW(from, to, 0);
	const DWORD err = moved ? 0 : GetLastError();
	free(from);
	free(to);
	if (moved)
		return 0;
	if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
		return 1;
	return -1;
}

#else /* !_WIN32 */

#include <errno.h>
#include <unistd.h>

int rr_rename_exclusive(const char *from_utf8, const char *to_utf8)
{
	/* rename(2) would replace the destination; link(2) fails with EEXIST
	 * when it exists, so link first and drop the source afterwards. */
	if (link(from_utf8, to_utf8) != 0)
		return errno == EEXIST ? 1 : -1;
	unlink(from_utf8);
	return 0;
}

#endif
