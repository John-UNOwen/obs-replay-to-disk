/* status-dock.cpp — the status dock (handoff 6 WP5 step 3): one small widget
 * with the engine status line and Start/Stop + Save, refreshed from
 * rr_engine_get_status on a 500 ms QTimer. UI thread only; the buttons only
 * enqueue engine commands (D5).
 * License: GPL-2.0-or-later.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "plugin/engine.h"
#include "plugin/plugin-ui.h"
#include "status-dock.hpp"
#include "status-util.hpp"

static QWidget *g_dock_content = nullptr;

ReplayToDiskDock::ReplayToDiskDock()
{
	auto *layout = new QVBoxLayout(this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_status);

	auto *row = new QHBoxLayout();
	m_startStop = new QPushButton(this);
	m_save = new QPushButton(rr_qtext("Button.Save"), this);
	row->addWidget(m_startStop);
	row->addWidget(m_save);
	layout->addLayout(row);

	connect(m_startStop, &QPushButton::clicked, this, [this]() { on_start_stop(); });
	connect(m_save, &QPushButton::clicked, this, [this]() {
		rr_plugin_save();
		refresh();
	});

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &ReplayToDiskDock::refresh);
	m_timer->start(500);
	refresh();
}

ReplayToDiskDock::~ReplayToDiskDock()
{
	/* OBS owns the widget once added to a dock: this also runs when the
	 * main window (and with it the dock) dies at shutdown, before the
	 * module unloads, so the global never dangles. */
	if (g_dock_content == this)
		g_dock_content = nullptr;
}

void ReplayToDiskDock::on_start_stop()
{
	struct rr_status st;
	rr_engine_get_status(&st);
	if (st.state == RR_STATE_OFF)
		rr_plugin_start();
	else
		rr_plugin_stop();
	refresh();
}

void ReplayToDiskDock::refresh()
{
	struct rr_status st;
	rr_engine_get_status(&st);

	m_status->setText(rr_status_lines(st).join('\n'));

	const bool off = (st.state == RR_STATE_OFF);
	m_startStop->setText(off ? rr_qtext("Button.Start") : rr_qtext("Button.Stop"));
	m_save->setEnabled(st.state == RR_STATE_BUFFERING);
}

/* ------------------------------------------------------------- registration */

extern "C" void rr_status_dock_install(void)
{
	if (g_dock_content)
		return;

	auto *content = new ReplayToDiskDock();
	if (!obs_frontend_add_dock_by_id("replay_to_disk_dock", obs_module_text("Dock.Title"), content)) {
		/* id already used (duplicate plugin) */
		delete content;
		return;
	}
	g_dock_content = content;
}

extern "C" void rr_status_dock_uninstall(void)
{
	if (!g_dock_content)
		return;
	/* OBS drops its shared_ptr to the QDockWidget, which deletes the
	 * wrapper and our content widget (whose destructor clears the
	 * global) while this module is still loaded. */
	obs_frontend_remove_dock("replay_to_disk_dock");
	g_dock_content = nullptr;
}
