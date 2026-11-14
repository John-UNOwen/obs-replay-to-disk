#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Load the plugin's FFmpeg dependency closure before the first FFmpeg call.
 * rr_ffmpeg_preload() uses the calling module's own directory; the _dir()
 * variant takes it explicitly (used by the unit test). Every closure member
 * already loaded in the process (e.g. OBS's own same-major copies) is reused.
 * On failure, err holds a short reason and the return value is false.
 */
bool rr_ffmpeg_preload(char *err, size_t err_size);
bool rr_ffmpeg_preload_dir(const wchar_t *dir, char *err, size_t err_size);

size_t rr_ffmpeg_closure_count(void);
const wchar_t *rr_ffmpeg_closure_at(size_t i);
