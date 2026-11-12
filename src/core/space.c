/* space.c — disk sizing arithmetic. See space.h.
 * License: GPL-2.0-or-later.
 */
#include "space.h"

#define RR_ONE_GIB ((int64_t)1024 * 1024 * 1024)

int64_t rr_space_estimate_bytes(int bitrate_kbps_total, int window_sec)
{
	if (bitrate_kbps_total <= 0 || window_sec <= 0)
		return 0;
	/* kbit/s → bytes over the window; the int64 cast keeps the product safe at
	 * the top settings (150000 kbit/s × 1800 s ≈ 3.4e10). */
	return (int64_t)bitrate_kbps_total * 1000 * window_sec / 8;
}

bool rr_space_preflight(int64_t estimate, int64_t free_buffer, int64_t free_output, bool same_drive)
{
	if (estimate < 0)
		return false;

	int64_t need = estimate + RR_ONE_GIB;
	if (same_drive)
		need = estimate * 2 + RR_ONE_GIB; /* the rolling buffer plus the worst-case save on one drive */

	return free_buffer >= need && free_output >= need;
}
