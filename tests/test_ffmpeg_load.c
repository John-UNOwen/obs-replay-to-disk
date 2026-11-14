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
 * Proves the FFmpeg preload closure: after rr_ffmpeg_preload_dir() every
 * member is a loaded module in this process (the test itself never links
 * FFmpeg), in dependency order, and the loaded avformat answers calls.
 */

#include <windows.h>
#include <stdio.h>

#include "plugin/ffmpeg-load.h"

#define WSTR2(x) L##x
#define WSTR(x) WSTR2(x)

int main(void)
{
	char err[256] = {0};

	if (!rr_ffmpeg_preload_dir(WSTR(RR_TEST_FFMPEG_DIR), err, sizeof(err))) {
		printf("FAIL: preload: %s\n", err);
		return 1;
	}

	const size_t n = rr_ffmpeg_closure_count();
	if (n != 8) {
		printf("FAIL: closure count %lu\n", (unsigned long)n);
		return 1;
	}

	for (size_t i = 0; i < n; i++) {
		const wchar_t *name = rr_ffmpeg_closure_at(i);
		if (!GetModuleHandleW(name)) {
			printf("FAIL: %S not loaded\n", name);
			return 1;
		}
	}

	HMODULE avf = GetModuleHandleW(L"avformat-62.dll");
	unsigned (*avformat_version_fn)(void) = (unsigned (*)(void))(void *)GetProcAddress(avf, "avformat_version");
	if (!avformat_version_fn) {
		puts("FAIL: avformat_version symbol");
		return 1;
	}

	const unsigned v = avformat_version_fn();
	printf("ok: %lu closure DLLs loaded; avformat %u.%u.%u\n", (unsigned long)n, v >> 16, (v >> 8) & 0xFF,
	       v & 0xFF);
	return 0;
}
