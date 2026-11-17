/* outputs-gate.cpp: OBS control-panel gating (owner policy 2026-11-15).
 *
 * While the plugin's buffer runs, OBS's own Start/Stop buttons for streaming,
 * recording and the replay buffer do nothing: each button stays disabled while
 * its output is idle, and a running output keeps its button live so Stop still
 * works (Start and Stop are the same button). With the buffer off the buttons
 * belong to OBS again. A 1 s poll re-applies the states because OBS also
 * writes the enabled flag when its own output state changes, so an edge-only
 * update gets stomped.
 *
 * OBS 32 removed the Output menu, so these buttons are the only start path
 * inside the main window. Hotkeys and the tray menu stay available.
 * License: GPL-2.0-or-later.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "plugin/engine.h"
#include "plugin/plugin-ui.h"

struct gate_slot {
	const char *name;
	bool (*running)(void);
	QPushButton *button = nullptr;
};

static gate_slot g_slots[] = {
	{"streamButton", obs_frontend_streaming_active},
	{"recordButton", obs_frontend_recording_active},
	{"replayBufferButton", obs_frontend_replay_buffer_active},
};

static QTimer *g_timer = nullptr;
static bool g_gating;

static QWidget *main_window(void)
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

static QPushButton *find_button(gate_slot &slot)
{
	if (!slot.button) {
		QWidget *win = main_window();
		if (win)
			slot.button = win->findChild<QPushButton *>(QString::fromLatin1(slot.name));
	}
	return slot.button;
}

static void poll_gate(void)
{
	struct rr_status st;
	rr_engine_get_status(&st);
	const bool gate = st.state != RR_STATE_OFF;

	/* Steady state with gating off: OBS owns the buttons. */
	if (!gate && !g_gating)
		return;

	for (gate_slot &slot : g_slots) {
		QPushButton *button = find_button(slot);
		if (!button)
			continue;
		const bool enabled = !gate || slot.running();
		if (button->isEnabled() != enabled)
			button->setEnabled(enabled);
	}
	g_gating = gate;
}

extern "C" void rr_outputs_gate_install(void)
{
	if (g_timer)
		return;
	g_timer = new QTimer();
	QObject::connect(g_timer, &QTimer::timeout, poll_gate);
	g_timer->start(1000);
}

extern "C" void rr_outputs_gate_uninstall(void)
{
	if (!g_timer)
		return;
	g_timer->stop();
	delete g_timer;
	g_timer = nullptr;

	/* Re-enable anything left disabled and drop the widget pointers: OBS
	 * expects its buttons live after the module is gone, and the cached
	 * pointers must not outlive the module to survive a plugin reload. */
	if (g_gating) {
		for (gate_slot &slot : g_slots) {
			if (slot.button)
				slot.button->setEnabled(true);
		}
	}
	for (gate_slot &slot : g_slots)
		slot.button = nullptr;
	g_gating = false;
}

/* Selftest probe: the live enabled state of the three buttons, UI thread.
 * False when an OBS widget could not be found. */
extern "C" bool rr_outputs_gate_debug_enabled(bool *stream, bool *record, bool *replay)
{
	bool out[3] = {false, false, false};
	for (int i = 0; i < 3; i++) {
		QPushButton *button = find_button(g_slots[i]);
		if (!button)
			return false;
		out[i] = button->isEnabled();
	}
	*stream = out[0];
	*record = out[1];
	*replay = out[2];
	return true;
}
