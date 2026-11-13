/* rename.h — exclusive file rename (the D13 save rename must never
 * overwrite an existing final file). Kept free of libobs on purpose so the
 * unit tests link it without OBS.
 * License: GPL-2.0-or-later.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Move from_utf8 to to_utf8 without ever replacing an existing destination
 * (libobs os_rename passes MOVEFILE_REPLACE_EXISTING and would clobber).
 * Returns 0 on success, 1 when the destination already exists (the caller
 * may retry under another name), -1 on any other failure. */
int rr_rename_exclusive(const char *from_utf8, const char *to_utf8);

#ifdef __cplusplus
}
#endif
