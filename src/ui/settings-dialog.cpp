/* settings-dialog.cpp — the tools-menu settings dialog (handoff 6 WP5 step 2).
 * Every 5.8 setting gets a widget; the status line refreshes from
 * rr_engine_get_status on a 500 ms QTimer; the buffer folder is validated
 * with rr_paths_check_buffer_dir (core/paths) against the effective output
 * folder, the output folder with rr_paths_check_output_dir; the disk line comes
 * from rr_space_estimate_bytes (core/space) with the total bitrate (50 Mbps
 * assumed for constant quality, 5.5 step 3);
 * applying changed settings while the buffer runs offers a restart.
 * Persisting goes through rr_plugin_set_settings -> rr_settings_save
 * (settings.c, clamped via core/settings-clamp). Everything here runs on the
 * UI thread and only enqueues engine commands (D5).
 * License: GPL-2.0-or-later.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QAbstractButton>
#include <QVBoxLayout>

#include <cstdio>
#include <cstring>

#include "plugin/engine.h"
#include "plugin/plugin-ui.h"
#include "core/paths.h"
#include "core/settings-clamp.h"
#include "core/space.h"
#include "settings-dialog.hpp"
#include "status-util.hpp"

static ReplayToDiskDialog *g_dialog = nullptr;

/* The widgets carry in-range values (spin ranges, 30..1800 length), so only
 * the free-text fields can differ from the saved copy after a clamp. */
static bool config_equal(const struct rr_config &a, const struct rr_config &b)
{
	return a.autostart == b.autostart && a.buffer_seconds == b.buffer_seconds && a.save_scope == b.save_scope &&
	       strcmp(a.buffer_dir, b.buffer_dir) == 0 && strcmp(a.output_dir, b.output_dir) == 0 &&
	       strcmp(a.filename_format, b.filename_format) == 0 && strcmp(a.video_encoder, b.video_encoder) == 0 &&
	       a.rate_control == b.rate_control && a.bitrate_kbps == b.bitrate_kbps && a.cq_level == b.cq_level &&
	       strcmp(a.extra_encoder_settings, b.extra_encoder_settings) == 0 && a.audio_tracks == b.audio_tracks &&
	       a.audio_bitrate_kbps == b.audio_bitrate_kbps && a.save_sound == b.save_sound;
}

ReplayToDiskDialog::ReplayToDiskDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(rr_qtext("Dialog.Title"));
	build_ui();
	reload();

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &ReplayToDiskDialog::refresh_status);
	m_timer->start(500);
	refresh_status();
}

ReplayToDiskDialog::~ReplayToDiskDialog()
{
	/* Also clears the global when OBS destroys the dialog with the main
	 * window at shutdown, before this module unloads. */
	if (g_dialog == this)
		g_dialog = nullptr;
}

void ReplayToDiskDialog::build_ui()
{
	auto *root = new QVBoxLayout(this);

	/* engine controls + status line (500 ms QTimer) */
	auto *controlRow = new QHBoxLayout();
	m_startStop = new QPushButton(this);
	m_saveBtn = new QPushButton(rr_qtext("Button.Save"), this);
	controlRow->addWidget(m_startStop);
	controlRow->addWidget(m_saveBtn);
	controlRow->addStretch();
	root->addLayout(controlRow);

	m_statusLine = new QLabel(this);
	m_savedLine = new QLabel(this);
	m_errorLine = new QLabel(this);
	m_errorLine->setStyleSheet("color: #c03030;");
	for (QLabel *label : {m_statusLine, m_savedLine, m_errorLine}) {
		label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		label->setWordWrap(false);
		root->addWidget(label);
	}

	connect(m_startStop, &QPushButton::clicked, this, [this]() { on_start_stop(); });
	connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
		rr_plugin_save();
		refresh_status();
	});

	/* live disk estimate + the settings-lock note (1.3 side effect 1) */
	m_diskLine = new QLabel(this);
	root->addWidget(m_diskLine);
	auto *note = new QLabel(rr_qtext("Note.SettingsLocked"), this);
	note->setWordWrap(true);
	root->addWidget(note);

	auto *form = new QFormLayout();

	m_autostart = new QCheckBox(rr_qtext("Label.Autostart"), this);
	form->addRow(m_autostart);

	m_minutes = new QSpinBox(this);
	m_minutes->setRange(0, 30);
	m_minutes->setSuffix(rr_qtext("Unit.Minutes"));
	m_seconds = new QSpinBox(this);
	m_seconds->setRange(0, 59);
	m_seconds->setSuffix(rr_qtext("Unit.Seconds"));
	auto *lengthRow = new QHBoxLayout();
	lengthRow->addWidget(m_minutes);
	lengthRow->addWidget(m_seconds);
	lengthRow->addStretch();
	form->addRow(new QLabel(rr_qtext("Label.BufferLength")), lengthRow);

	m_scope = new QComboBox(this);
	m_scope->addItem(rr_qtext("Scope.Window"), RR_SCOPE_WINDOW);
	m_scope->addItem(rr_qtext("Scope.SinceLastSave"), RR_SCOPE_SINCE_LAST_SAVE);
	form->addRow(new QLabel(rr_qtext("Label.SaveScope")), m_scope);

	m_bufferDir = new QLineEdit(this);
	m_outputDir = new QLineEdit(this);
	form->addRow(new QLabel(rr_qtext("Label.BufferDir")), dir_row(m_bufferDir, true));
	form->addRow(new QLabel(rr_qtext("Label.OutputDir")), dir_row(m_outputDir, false));

	m_filename = new QLineEdit(this);
	form->addRow(new QLabel(rr_qtext("Label.Filename")), m_filename);

	m_encoder = new QComboBox(this);
	form->addRow(new QLabel(rr_qtext("Label.Encoder")), m_encoder);

	m_rateControl = new QComboBox(this);
	m_rateControl->addItem(rr_qtext("RC.CBR"), RR_RC_CBR);
	m_rateControl->addItem(rr_qtext("RC.CQP"), RR_RC_CQP);
	form->addRow(new QLabel(rr_qtext("Label.RateControl")), m_rateControl);

	m_bitrate = new QSpinBox(this);
	m_bitrate->setRange(1000, 150000);
	m_bitrate->setSingleStep(1000);
	m_bitrate->setSuffix(rr_qtext("Unit.Kbps"));
	form->addRow(new QLabel(rr_qtext("Label.Bitrate")), m_bitrate);

	m_cq = new QSpinBox(this);
	m_cq->setRange(1, 51);
	form->addRow(new QLabel(rr_qtext("Label.CQ")), m_cq);

	m_extra = new QPlainTextEdit(this);
	m_extra->setMaximumHeight(88); /* ~3 lines */
	form->addRow(new QLabel(rr_qtext("Label.ExtraSettings")), m_extra);

	auto *trackRow = new QHBoxLayout();
	for (int i = 0; i < 6; i++) {
		m_tracks[i] = new QCheckBox(rr_qtext("Label.Track").arg(i + 1), this);
		trackRow->addWidget(m_tracks[i]);
	}
	trackRow->addStretch();
	form->addRow(new QLabel(rr_qtext("Label.AudioTracks")), trackRow);

	m_audioBitrate = new QSpinBox(this);
	m_audioBitrate->setRange(64, 320);
	m_audioBitrate->setSingleStep(16);
	m_audioBitrate->setSuffix(rr_qtext("Unit.Kbps"));
	form->addRow(new QLabel(rr_qtext("Label.AudioBitrate")), m_audioBitrate);

	m_saveSound = new QCheckBox(rr_qtext("Label.SaveSound"), this);
	form->addRow(m_saveSound);

	root->addLayout(form);

	auto *buttons =
		new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(rr_qtext("Button.OK"));
	buttons->button(QDialogButtonBox::Apply)->setText(rr_qtext("Button.Apply"));
	buttons->button(QDialogButtonBox::Cancel)->setText(rr_qtext("Button.Cancel"));
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		if (apply())
			accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(buttons->button(QDialogButtonBox::Apply), &QAbstractButton::clicked, this, [this]() { apply(); });
	root->addWidget(buttons);

	/* keeps the disk line and the enabled rate-control field live */
	connect(m_minutes, &QSpinBox::valueChanged, this, [this](int) { on_length_changed(); });
	connect(m_seconds, &QSpinBox::valueChanged, this, [this](int) { on_length_changed(); });
	connect(m_bitrate, &QSpinBox::valueChanged, this, [this](int) { update_disk_line(); });
	connect(m_cq, &QSpinBox::valueChanged, this, [this](int) { update_disk_line(); });
	connect(m_audioBitrate, &QSpinBox::valueChanged, this, [this](int) { update_disk_line(); });
	connect(m_rateControl, &QComboBox::currentIndexChanged, this, [this](int) {
		update_rc_enabled();
		update_disk_line();
	});
	for (int i = 0; i < 6; i++) {
		connect(m_tracks[i], &QCheckBox::toggled, this, [this, i](bool on) {
			if (!on && track_mask() == 0) {
				/* at least one bit (5.8) */
				QMessageBox::warning(this, windowTitle(), rr_qtext("Error.TrackRequired"));
				m_tracks[i]->blockSignals(true);
				m_tracks[i]->setChecked(true);
				m_tracks[i]->blockSignals(false);
				return;
			}
			update_disk_line();
		});
	}
}

QHBoxLayout *ReplayToDiskDialog::dir_row(QLineEdit *edit, bool for_buffer)
{
	auto *row = new QHBoxLayout();
	row->addWidget(edit);
	auto *browse = new QPushButton(rr_qtext("Button.Browse"), this);
	row->addWidget(browse);
	connect(browse, &QPushButton::clicked, this, [this, edit, for_buffer]() {
		QString start = edit->text();
		if (start.isEmpty())
			start = edit->placeholderText();
		const QString dir = QFileDialog::getExistingDirectory(
			this, rr_qtext(for_buffer ? "Picker.BufferDir" : "Picker.OutputDir"), start);
		if (!dir.isEmpty())
			edit->setText(QDir::toNativeSeparators(dir));
	});
	return row;
}

/* 5.8 availability rule: listed by obs_enum_encoder_types, a video encoder
 * whose codec is h264/hevc/av1, and neither DEPRECATED nor INTERNAL. */
void ReplayToDiskDialog::populate_encoders(const char *current)
{
	m_encoder->clear();
	m_encoder->addItem(rr_qtext("Encoder.Auto"), QString());

	const char *type = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (obs_get_encoder_type(type) != OBS_ENCODER_VIDEO)
			continue;
		const uint32_t caps = obs_get_encoder_caps(type);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *codec = obs_get_encoder_codec(type);
		if (!codec || (strcmp(codec, "h264") != 0 && strcmp(codec, "hevc") != 0 && strcmp(codec, "av1") != 0))
			continue;

		const QString id = QString::fromUtf8(type);
		QString name = QString::fromUtf8(obs_encoder_get_display_name(type));
		if (name.isEmpty())
			name = id;
		m_encoder->addItem(QStringLiteral("%1 (%2)").arg(name, id), id);
	}

	const QString cur = QString::fromUtf8(current ? current : "");
	int idx = m_encoder->findData(cur);
	if (idx < 0 && !cur.isEmpty()) {
		/* Configured but not offered right now: keep it selectable so
		 * the dialog never silently rewrites an explicit choice. */
		m_encoder->addItem(cur + rr_qtext("Encoder.Unavailable"), cur);
		idx = m_encoder->count() - 1;
	}
	m_encoder->setCurrentIndex(idx < 0 ? 0 : idx);
}

/* minutes + seconds are the same value (30..1800 s, clamped through the same
 * helper settings.c uses). */
void ReplayToDiskDialog::set_buffer_seconds(int total_sec)
{
	m_bufferSeconds = rr_settings_clamp_window_sec(total_sec);
	m_syncingLength = true;
	m_minutes->setValue(m_bufferSeconds / 60);
	m_seconds->setValue(m_bufferSeconds % 60);
	m_syncingLength = false;
}

void ReplayToDiskDialog::on_length_changed()
{
	if (m_syncingLength)
		return;
	set_buffer_seconds(m_minutes->value() * 60 + m_seconds->value());
	update_disk_line();
}

int ReplayToDiskDialog::track_mask() const
{
	int mask = 0;
	for (int i = 0; i < 6; i++) {
		if (m_tracks[i]->isChecked())
			mask |= 1 << i;
	}
	return mask;
}

void ReplayToDiskDialog::update_rc_enabled()
{
	const int rc = m_rateControl->currentData().toInt();
	m_bitrate->setEnabled(rc == RR_RC_CBR);
	m_cq->setEnabled(rc == RR_RC_CQP);
}

void ReplayToDiskDialog::reload()
{
	rr_plugin_get_settings(&m_saved);

	struct rr_config def;
	rr_settings_default(&def);
	m_filename->setPlaceholderText(QString::fromUtf8(def.filename_format));

	m_autostart->setChecked(m_saved.autostart);
	set_buffer_seconds(m_saved.buffer_seconds);
	m_scope->setCurrentIndex(m_scope->findData(m_saved.save_scope));
	m_bufferDir->setText(QString::fromUtf8(m_saved.buffer_dir));
	m_outputDir->setText(QString::fromUtf8(m_saved.output_dir));
	m_filename->setText(QString::fromUtf8(m_saved.filename_format));
	populate_encoders(m_saved.video_encoder);
	m_rateControl->setCurrentIndex(m_rateControl->findData(m_saved.rate_control));
	m_bitrate->setValue(m_saved.bitrate_kbps);
	m_cq->setValue(m_saved.cq_level);
	m_extra->setPlainText(QString::fromUtf8(m_saved.extra_encoder_settings));
	for (int i = 0; i < 6; i++) {
		m_tracks[i]->blockSignals(true);
		m_tracks[i]->setChecked((m_saved.audio_tracks & (1 << i)) != 0);
		m_tracks[i]->blockSignals(false);
	}
	m_audioBitrate->setValue(m_saved.audio_bitrate_kbps);
	m_saveSound->setChecked(m_saved.save_sound);

	/* empty folder fields show the OBS-recording-path default greyed */
	char buf[RR_PATHLEN] = "";
	char out[RR_PATHLEN] = "";
	rr_plugin_effective_dirs(&m_saved, buf, sizeof(buf), out, sizeof(out));
	m_bufferDir->setPlaceholderText(QString::fromUtf8(buf));
	m_outputDir->setPlaceholderText(QString::fromUtf8(out));

	update_rc_enabled();
	update_disk_line();
	refresh_status();
}

void ReplayToDiskDialog::collect(struct rr_config *cfg) const
{
	*cfg = m_saved;
	cfg->autostart = m_autostart->isChecked();
	cfg->buffer_seconds = m_bufferSeconds;
	cfg->save_scope = m_scope->currentData().toInt();
	const QByteArray buf = m_bufferDir->text().toUtf8();
	snprintf(cfg->buffer_dir, sizeof(cfg->buffer_dir), "%s", buf.constData());
	const QByteArray out = m_outputDir->text().toUtf8();
	snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", out.constData());
	const QByteArray fname = m_filename->text().toUtf8();
	snprintf(cfg->filename_format, sizeof(cfg->filename_format), "%s", fname.constData());
	const QByteArray enc = m_encoder->currentData().toString().toUtf8();
	snprintf(cfg->video_encoder, sizeof(cfg->video_encoder), "%s", enc.constData());
	cfg->rate_control = m_rateControl->currentData().toInt();
	cfg->bitrate_kbps = m_bitrate->value();
	cfg->cq_level = m_cq->value();
	const QByteArray extra = m_extra->toPlainText().toUtf8();
	snprintf(cfg->extra_encoder_settings, sizeof(cfg->extra_encoder_settings), "%s", extra.constData());
	cfg->audio_tracks = track_mask();
	cfg->audio_bitrate_kbps = m_audioBitrate->value();
	cfg->save_sound = m_saveSound->isChecked();
}

void ReplayToDiskDialog::update_disk_line()
{
	struct rr_config cfg;
	collect(&cfg);
	const int64_t est = rr_space_estimate_bytes(rr_config_total_bitrate_kbps(&cfg), cfg.buffer_seconds);
	const double gb = (double)est / (1024.0 * 1024.0 * 1024.0);
	m_diskLine->setText(rr_qtext("Disk.Estimate").arg(gb, 0, 'f', 1).arg(gb, 0, 'f', 1));
}

bool ReplayToDiskDialog::apply()
{
	struct rr_config cfg;
	collect(&cfg);

	/* buffer folder validated against the effective output folder,
	 * exactly as the engine will at start (5.5 step 2) */
	char buf[RR_PATHLEN] = "";
	char out[RR_PATHLEN] = "";
	rr_plugin_effective_dirs(&cfg, buf, sizeof(buf), out, sizeof(out));
	switch (rr_paths_check_buffer_dir(buf, out)) {
	case RR_PATH_OK:
		break;
	case RR_PATH_DRIVE_ROOT:
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.BufferDriveRoot"));
		return false;
	case RR_PATH_SAME_AS_OUTPUT:
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.BufferSameAsOutput"));
		return false;
	case RR_PATH_NOT_ABSOLUTE:
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.BufferRelative"));
		return false;
	default: /* RR_PATH_EMPTY: neither configured nor resolvable */
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.BufferNotResolved"));
		return false;
	}
	/* Output folder: empty means OBS's recording path; a typed value must be
	 * absolute and not a drive root — the engine purges "*.rrpart" files and
	 * writes the saves in exactly that folder (same rule as at start). */
	switch (rr_paths_check_output_dir(cfg.output_dir)) {
	case RR_PATH_OK:
		break;
	case RR_PATH_DRIVE_ROOT:
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.OutputDriveRoot"));
		return false;
	default: /* RR_PATH_NOT_ABSOLUTE */
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.OutputRelative"));
		return false;
	}
	if (cfg.audio_tracks == 0) { /* defensive: the checkbox guard prevents this */
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.TrackRequired"));
		return false;
	}
	/* Filename format: a bare-name format only — '/', '\' or ':' would let
	 * the generated save name escape the output folder (the engine also
	 * refuses such a name at save time and falls back to the default). */
	if (cfg.filename_format[0] && !rr_paths_is_safe_filename(cfg.filename_format)) {
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.FilenameFormat"));
		return false;
	}

	struct rr_status st;
	rr_engine_get_status(&st);
	const bool active = (st.state != RR_STATE_OFF);
	const bool changed = !config_equal(cfg, m_saved);

	if (!rr_plugin_set_settings(&cfg)) {
		QMessageBox::warning(this, windowTitle(), rr_qtext("Error.SaveSettings"));
		return false;
	}
	reload(); /* shows the clamped values that were actually persisted */

	if (active && changed) {
		const QMessageBox::StandardButton ans =
			QMessageBox::question(this, windowTitle(), rr_qtext("Restart.Question"),
					      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (ans == QMessageBox::Yes) {
			rr_plugin_stop();
			rr_plugin_wait_off(6000);
			rr_plugin_start();
		}
		/* No: the settings are saved; the running buffer keeps its snapshot. */
	}
	refresh_status();
	return true;
}

void ReplayToDiskDialog::on_start_stop()
{
	struct rr_status st;
	rr_engine_get_status(&st);
	if (st.state == RR_STATE_OFF)
		rr_plugin_start();
	else
		rr_plugin_stop();
	refresh_status();
}

void ReplayToDiskDialog::refresh_status()
{
	struct rr_status st;
	rr_engine_get_status(&st);

	m_statusLine->setText(rr_status_first_line(st));
	m_savedLine->setText(st.last_saved[0] ? rr_qtext("Status.LastSaved").arg(QString::fromUtf8(st.last_saved))
					      : QString());
	m_errorLine->setText(st.last_error[0] ? rr_qtext("Status.LastError").arg(QString::fromUtf8(st.last_error))
					      : QString());

	const bool off = (st.state == RR_STATE_OFF);
	m_startStop->setText(off ? rr_qtext("Button.Start") : rr_qtext("Button.Stop"));
	m_saveBtn->setEnabled(st.state == RR_STATE_BUFFERING);
}

/* ------------------------------------------------------------- registration */

static void show_dialog(void *)
{
	if (!g_dialog)
		g_dialog = new ReplayToDiskDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	g_dialog->reload();
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

extern "C" void rr_settings_dialog_install(void)
{
	obs_frontend_add_tools_menu_item(obs_module_text("Tools.Menu"), show_dialog, nullptr);
}

extern "C" void rr_settings_dialog_uninstall(void)
{
	delete g_dialog; /* the destructor clears the global; null delete is fine */
	g_dialog = nullptr;
}
