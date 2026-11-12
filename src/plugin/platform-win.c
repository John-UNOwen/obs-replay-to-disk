/* platform-win.c — Windows helpers (handoff snippet 8.4, decision D8).
 * License: GPL-2.0-or-later.
 */
#include "platform-win.h"

#include <util/bmem.h>
#include <util/platform.h>

#ifdef _WIN32

#include <windows.h>

bool rr_file_closed(const char *utf8_path)
{
	wchar_t *w = NULL;
	if (!os_utf8_to_wcs_ptr(utf8_path, 0, &w))
		return false;
	HANDLE h = CreateFileW(w, GENERIC_READ, 0 /* no sharing */, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bfree(w);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	CloseHandle(h);
	return true;
}

void rr_play_save_sound(bool success)
{
	MessageBeep(success ? MB_OK : MB_ICONERROR);
}

#else /* !_WIN32 */

bool rr_file_closed(const char *utf8_path)
{
	(void)utf8_path; /* no mandatory file locks to test on this platform */
	return true;
}

void rr_play_save_sound(bool success)
{
	(void)success; /* no MessageBeep on this platform */
}

#endif
