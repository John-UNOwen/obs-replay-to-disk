/* settings-clamp.c — settings value clamps. See settings-clamp.h.
 * License: GPL-2.0-or-later.
 */
#include "settings-clamp.h"

int rr_settings_clamp_window_sec(int sec)
{
	if (sec < RR_WINDOW_SEC_MIN)
		return RR_WINDOW_SEC_MIN;
	if (sec > RR_WINDOW_SEC_MAX)
		return RR_WINDOW_SEC_MAX;
	return sec;
}
