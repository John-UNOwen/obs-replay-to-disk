/*
Replay to Disk
Copyright (C) 2026 John-UNOwen

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * FFmpeg preload for the delay-loaded save path (docs/STATUS.md
 * "Widening OBS compatibility").
 *
 * The plugin imports avformat/avcodec/avutil by exact soname and delay-loads
 * them, so the DLL itself loads even on an OBS that ships a different FFmpeg
 * major. Before the first FFmpeg call, rr_ffmpeg_preload() loads the exact
 * closure from the plugin's own folder, dependency-first: a child must be a
 * loaded module before a parent is mapped, because Windows searches the
 * loader's flags (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) only for the DLL named
 * in LoadLibraryEx, not for that DLL's own imports. A copy OBS already
 * loaded (same major) is reused instead of loading ours.
 */

#include "ffmpeg-load.h"

#ifdef _WIN32

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Topological order: every entry's imports appear earlier in the list.
 * Must stay in sync with the bundle list in CMakeLists.txt. */
static const wchar_t *const ffmpeg_closure[] = {
	L"avutil-60.dll", L"zlib.dll",    L"swresample-6.dll", L"libx264-164.dll",
	L"srt.dll",       L"librist.dll", L"avcodec-62.dll",   L"avformat-62.dll",
};

size_t rr_ffmpeg_closure_count(void)
{
	return sizeof(ffmpeg_closure) / sizeof(ffmpeg_closure[0]);
}

const wchar_t *rr_ffmpeg_closure_at(size_t i)
{
	return ffmpeg_closure[i];
}

bool rr_ffmpeg_preload_dir(const wchar_t *dir, char *err, size_t err_size)
{
	wchar_t path[32768];
	const size_t cap = sizeof(path) / sizeof(path[0]);
	const size_t dir_len = wcslen(dir);

	if (err && err_size)
		err[0] = 0;

	for (size_t i = 0; i < rr_ffmpeg_closure_count(); i++) {
		const wchar_t *name = ffmpeg_closure[i];

		if (GetModuleHandleW(name))
			continue; /* loaded by OBS or an earlier pass: reuse it */

		const size_t name_len = wcslen(name);
		if (dir_len + 1 + name_len + 1 > cap) {
			if (err && err_size)
				snprintf(err, err_size, "path too long for %S", name);
			return false;
		}
		wmemcpy(path, dir, dir_len);
		path[dir_len] = L'\\';
		wmemcpy(path + dir_len + 1, name, name_len + 1);

		if (!LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
			if (err && err_size)
				snprintf(err, err_size, "load of %S failed (err %lu)", name, GetLastError());
			return false;
		}
	}

	return true;
}

bool rr_ffmpeg_preload(char *err, size_t err_size)
{
	wchar_t path[32768];
	HMODULE self = NULL;

	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)&rr_ffmpeg_preload, &self)) {
		if (err && err_size)
			snprintf(err, err_size, "own module handle failed (err %lu)", GetLastError());
		return false;
	}

	const DWORD n = GetModuleFileNameW(self, path, (DWORD)(sizeof(path) / sizeof(path[0])));
	if (n == 0 || n >= sizeof(path) / sizeof(path[0])) {
		if (err && err_size)
			snprintf(err, err_size, "own module path failed (err %lu)", GetLastError());
		return false;
	}

	wchar_t *slash = wcsrchr(path, L'\\');
	if (!slash) {
		if (err && err_size)
			snprintf(err, err_size, "own module path has no directory");
		return false;
	}
	*slash = 0;

	return rr_ffmpeg_preload_dir(path, err, err_size);
}

#else /* !_WIN32 */

size_t rr_ffmpeg_closure_count(void)
{
	return 0;
}

const wchar_t *rr_ffmpeg_closure_at(size_t i)
{
	(void)i;
	return 0;
}

bool rr_ffmpeg_preload_dir(const wchar_t *dir, char *err, size_t err_size)
{
	(void)dir;
	if (err && err_size)
		err[0] = 0;
	return true;
}

bool rr_ffmpeg_preload(char *err, size_t err_size)
{
	(void)err;
	(void)err_size;
	return true;
}

#endif
