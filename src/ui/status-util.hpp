/* status-util.hpp — shared status formatting for the dialog and the dock
 * (handoff 6 WP5). All text comes from the locale (obs_module_text).
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <QString>
#include <QStringList>

#include <obs-module.h>

#include "plugin/engine.h"

/* Locale text for a plugin string key. */
inline QString rr_qtext(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

inline QString rr_state_text(int state)
{
	switch (state) {
	case RR_STATE_STARTING:
		return rr_qtext("State.Starting");
	case RR_STATE_BUFFERING:
		return rr_qtext("State.Buffering");
	case RR_STATE_SAVING:
		return rr_qtext("State.Saving");
	case RR_STATE_STOPPING:
		return rr_qtext("State.Stopping");
	default:
		return rr_qtext("State.Off");
	}
}

/* Buffered time as "M:SS". */
inline QString rr_buffered_time_text(double sec)
{
	const int total = (int)(sec + 0.5);
	return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

/* "State | buffered | disk used" — the first line of every status line. */
inline QString rr_status_first_line(const struct rr_status &st)
{
	QString line = rr_state_text(st.state);
	if (st.state != RR_STATE_OFF) {
		line += QString(" | ") + rr_qtext("Status.Buffered").arg(rr_buffered_time_text(st.buffered_sec));
		const double gb = (double)st.buffer_bytes / (1024.0 * 1024.0 * 1024.0);
		line += QString(" | ") + rr_qtext("Status.Disk").arg(gb, 0, 'f', 2);
	}
	return line;
}

/* State line, plus a last-saved line and a last-error line when set. */
inline QStringList rr_status_lines(const struct rr_status &st)
{
	QStringList out;
	out << rr_status_first_line(st);
	if (st.last_saved[0])
		out << rr_qtext("Status.LastSaved").arg(QString::fromUtf8(st.last_saved));
	if (st.last_error[0])
		out << rr_qtext("Status.LastError").arg(QString::fromUtf8(st.last_error));
	return out;
}
