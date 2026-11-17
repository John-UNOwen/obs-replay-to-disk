/* status-dock.hpp — the optional status dock (handoff 6 WP5 step 3):
 * status line + Start/Stop + Save, refreshed by the same 500 ms QTimer.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

/* The dock content widget: OBS wraps it in a QDockWidget titled "Replay to
 * Disk" under the id "replay_to_disk_dock" (Docks menu). */
class ReplayToDiskDock : public QWidget {
	Q_OBJECT

public:
	ReplayToDiskDock();
	~ReplayToDiskDock() override;

	void flash(const QString &msg);

private:
	void refresh();
	void on_start_stop();

	QLabel *m_status = nullptr;
	QPushButton *m_startStop = nullptr;
	QPushButton *m_save = nullptr;
	QTimer *m_timer = nullptr;
	QString m_flash;
	qint64 m_flash_until = 0;
};

extern "C" {

/* obs_frontend_add_dock_by_id / remove at module load/unload. UI thread. */
void rr_status_dock_install(void);
void rr_status_dock_uninstall(void);

/* Show msg as an extra dock line for 8 s (UI thread). */
void rr_status_dock_flash(const char *msg);
}
