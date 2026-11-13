/* paths.h — folder validation and filename matching (handoff 5.10, D12/D13).
 * Pure C: no libobs. Windows path semantics ('\' or '/'); a leading '/' is
 * treated as absolute so the checks stay testable on POSIX too.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

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

/* The volume-root test both checkers run on the canonicalized spelling,
 * exposed so its own contract can be tested directly: the checkers gate
 * relative spellings as NOT_ABSOLUTE before this is ever reached. Roots:
 * "C:\", "C:/", "D:\" (any number of trailing slashes), "/", "\\" (the
 * raw-fallback spelling), and the UNC share root "\\server\share" (likewise
 * with trailing slashes). Not roots: anything below them, the UNC server
 * listing "\\server", and the drive-relative spellings "C:" and "C:file" —
 * Windows resolves those against the drive's current directory, so they
 * name no root at all. */
bool rr_paths_is_drive_root(const char *p);

bool rr_paths_is_segment_name(const char *file_name); /* "rrseg_*.mkv" only */
bool rr_paths_is_part_name(const char *file_name);    /* "*.rrpart" only */

enum { RR_SUFFIX_N_FIRST = 2, RR_SUFFIX_N_LAST = 999 };

/* One walk of the D13 save-suffix policy "<stem>_N.mp4", N from 2. The save
 * runs it twice — the advisory name pick before the remux, and the exclusive
 * rename walk after it — so the passes must continue one sequence over the
 * same bare stem: the rename pass starts at the first N the advisory pass
 * did not use, so an already chosen "_2" that gets taken mid-remux continues
 * at "_3" and never chains into "_2_2". claim(user, candidate) attempts to
 * take a candidate: 0 = claimed, 1 = the name is taken (walk on), negative =
 * hard error (stop). Returns true when a candidate was claimed; `out` holds
 * the path and, if not NULL, `used_n` its number. False when every number
 * up to RR_SUFFIX_N_LAST is taken, `claim` reports an error, or `out` cannot
 * hold a candidate. first_n below RR_SUFFIX_N_FIRST is clamped up to it. */
typedef int (*rr_suffix_claim_fn)(void *user, const char *candidate);
bool rr_paths_take_suffix(const char *stem, int first_n, rr_suffix_claim_fn claim, void *user, char *out, size_t sz,
			  int *used_n);

/* True when name is a valid Windows file name for the output folder: no '/'
 * or '\\' (would escape the folder), no ':' (drive prefix or NTFS stream),
 * none of the Windows-illegal '< > " | ? *', no trailing '.' or space (Windows
 * strips them, so the stored name never matches the one later opened), not a
 * bare "." or "..", and not a reserved device base name (CON, PRN, AUX, NUL,
 * COM1-9, LPT1-9, case-insensitive, with or without an extension). Used for
 * the configured filename_format at dialog Apply and settings load, and for
 * the generated save file name, so a bad format is refused where it is set
 * instead of failing every save at the file open. */
bool rr_paths_is_safe_filename(const char *name);

#ifdef __cplusplus
}
#endif
