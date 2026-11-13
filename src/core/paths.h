/* paths.h — folder validation and filename matching (handoff 5.10, D12/D13).
 * Pure C: no libobs. Windows path semantics ('\' or '/'); a leading '/' is
 * treated as absolute so the checks stay testable on POSIX too.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rr_path_error { RR_PATH_OK, RR_PATH_EMPTY, RR_PATH_DRIVE_ROOT, RR_PATH_SAME_AS_OUTPUT, RR_PATH_NOT_ABSOLUTE };

/* A buffer folder may not be empty, a drive root (C:\, D:/), the same folder
 * as the output folder (case-insensitive, either slash), or relative. */
enum rr_path_error rr_paths_check_buffer_dir(const char *buffer_dir, const char *output_dir);

bool rr_paths_is_segment_name(const char *file_name); /* "rrseg_*.mkv" only */
bool rr_paths_is_part_name(const char *file_name);    /* "*.rrpart" only */

/* True when name is safe to append to one folder: a bare name with no '/' or
 * '\\', no ':' (drive prefix or NTFS stream), and not "." or "..". Used for
 * the generated save file name and for the configured filename_format, so a
 * crafted format can never write outside the output folder. */
bool rr_paths_is_safe_filename(const char *name);

#ifdef __cplusplus
}
#endif
