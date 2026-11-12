/* platform-win.h — platform helpers (handoff 5.2; the D8 closed-check is 8.4).
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <stdbool.h>

/* True when the segment file is released by its writer and complete: an
 * exclusive read-open succeeds (D8, E15). On platforms without mandatory
 * file locks this returns true (the writer closing is taken as given). */
bool rr_file_closed(const char *utf8_path);

/* Save sound (5.8 save_sound): a Windows MessageBeep — OK on a finished
 * save, the error icon sound on a failed one. Caller checks the setting. */
void rr_play_save_sound(bool success);
