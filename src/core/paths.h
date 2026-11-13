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

enum rr_path_error {
	RR_PATH_OK,
	RR_PATH_EMPTY,
	RR_PATH_DRIVE_ROOT,
	RR_PATH_SAME_AS_OUTPUT,
	RR_PATH_NOT_ABSOLUTE,
	RR_PATH_DEVICE_PATH
};

/* A buffer folder may not be empty, a drive root (C:\, D:/, or the UNC share
 * root \\server\share), the same folder as the output folder
 * (case-insensitive, either slash), a Windows device path (\\?\, \\.\,
 * \??\ — textual aliases a comparison cannot follow and no picker ever
 * produces), or relative. Both paths are canonicalized first ('.', '..',
 * repeated separators, slash style and each name's trailing dots and spaces
 * folded), so "C:\rec\..\rec", "C:\rec." and "C:\rec " all fail as the
 * output folder "C:\rec". */
enum rr_path_error rr_paths_check_buffer_dir(const char *buffer_dir, const char *output_dir);

/* An output folder may be empty — the caller resolves it from the OBS
 * recording path. A configured one must be absolute, not a drive root or
 * UNC share root, and not a Windows device path: the engine purges stale
 * "*.rrpart" files there and writes the finished saves into it, so a
 * relative value would act on the process working directory, a root would
 * sweep a whole volume or network share, and a device path never names a
 * folder at all. Canonicalized like the buffer check, so "D:\videos\.." is
 * judged as "D:\" and "D:\videos\.. " likewise. */
enum rr_path_error rr_paths_check_output_dir(const char *output_dir);

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
