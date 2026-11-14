/* settings-dialog.hpp — the tools-menu settings dialog (handoff 6 WP5 step 2):
 * every 5.8 setting, engine controls and a 500 ms status line.
 * License: GPL-2.0-or-later.
 */
#pragma once

#include <QDialog>
#include <QPair>
#include <QString>
#include <QVector>

#include "plugin/settings.h"

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;

class ReplayToDiskDialog : public QDialog {
	Q_OBJECT

public:
	explicit ReplayToDiskDialog(QWidget *parent = nullptr);
	~ReplayToDiskDialog() override;

	/* Re-read the host settings into the widgets (on every open). */
	void reload();

private:
	void build_ui();
	QHBoxLayout *dir_row(QLineEdit *edit, bool for_buffer);
	void populate_encoders(const char *current);
	void set_buffer_seconds(int total_sec);
	int track_mask() const;
	void on_length_changed();
	void update_rc_enabled();
	void update_source_enabled();
	void populate_watch_sources();
	void collect(struct rr_config *cfg) const;
	void update_disk_line();
	bool apply();
	void refresh_status();
	void on_start_stop();

	QPushButton *m_startStop = nullptr;
	QPushButton *m_saveBtn = nullptr;
	QLabel *m_statusLine = nullptr;
	QLabel *m_savedLine = nullptr;
	QLabel *m_errorLine = nullptr;
	QLabel *m_diskLine = nullptr;

	QRadioButton *m_sourceManual = nullptr;
	QRadioButton *m_sourceStreaming = nullptr;
	QRadioButton *m_sourceRecording = nullptr;
	QCheckBox *m_autostart = nullptr;
	QSpinBox *m_minutes = nullptr;
	QSpinBox *m_seconds = nullptr;
	QComboBox *m_scope = nullptr;
	QLineEdit *m_bufferDir = nullptr;
	QLineEdit *m_outputDir = nullptr;
	QLineEdit *m_filename = nullptr;
	QComboBox *m_encoder = nullptr;
	QComboBox *m_rateControl = nullptr;
	QSpinBox *m_bitrate = nullptr;
	QSpinBox *m_cq = nullptr;
	QPlainTextEdit *m_extra = nullptr;
	QCheckBox *m_tracks[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
	QSpinBox *m_audioBitrate = nullptr;
	QCheckBox *m_saveSound = nullptr;

	QCheckBox *m_watchAutostart = nullptr;
	QCheckBox *m_watchFollow = nullptr;
	QLineEdit *m_watchNames = nullptr;
	QComboBox *m_watchSource = nullptr;
	/* combo rows, index-aligned with m_watchSource (row 0 = none): uuid, display */
	QVector<QPair<QString, QString>> m_watch_srcs;

	QTimer *m_timer = nullptr;
	int m_bufferSeconds = 1800;
	bool m_syncingLength = false;
	struct rr_config m_saved;
};

extern "C" {

/* Add the "Replay to Disk…" tools-menu item / tear it down. UI thread. */
void rr_settings_dialog_install(void);
void rr_settings_dialog_uninstall(void);
}
