/* process-watch.cpp — the process watcher (UI thread, 2 s QTimer).
 *
 * Two settings-driven behaviors, inert until enabled:
 * - watch_follow: when the foreground window belongs to a listed exe, point
 *   the user-picked window/game capture source at that window. Only the
 *   foreground moves the source: an app from the list that merely starts in
 *   the background never steals the capture, and switching away from a listed
 *   app (Discord, desktop, ...) keeps the last followed window.
 * - watch_autostart: when a listed exe appears in the process table
 *   (rising edge) and the buffer is off, start it. Start only: the watcher
 *   never stops the buffer, and a stop while the process runs is respected
 *   until the process starts again.
 *
 * The source is retargeted exactly the way OBS's own picker encodes its combo
 * value (title:class:exe with # -> #22 and : -> #3A, see
 * libobs/util/windows/window-helpers.c add_window), so the source behaves as
 * if the user had picked the window in the source dialog. priority is pinned
 * to "exe" so a changing window title does not break the match.
 * License: GPL-2.0-or-later.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h> /* obs_log */

#include <QApplication>
#include <QTimer>
#include <QString>
#include <QStringList>

#include <windows.h>
#include <tlhelp32.h>

#include <string.h>

#include "plugin/engine.h"
#include "plugin/plugin-ui.h"

#define RR_WATCH_INTERVAL_MS 2000

/* ------------------------------------------------------------ name matching */

static QString lower_base_name(const wchar_t *path)
{
	const wchar_t *slash = wcsrchr(path, L'\\');
	QString s = QString::fromWCharArray(slash ? slash + 1 : path).toLower();
	if (s.endsWith(QStringLiteral(".exe")))
		s.chop(4);
	return s;
}

static QStringList parse_watch_list(const char *raw)
{
	QStringList out;
	const QString in = QString::fromUtf8(raw ? raw : "");
	const QStringList parts = in.split(',', Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		const std::wstring w = part.trimmed().toStdWString();
		const QString name = lower_base_name(w.c_str());
		if (!name.isEmpty() && !out.contains(name))
			out.append(name);
	}
	return out;
}

/* ------------------------------------------------------------- foreground */

/* Same fix OBS's window list applies: a UWP frame window hosts the real
 * window as a child owned by a different process (window-helpers.c
 * ms_get_uwp_actual_window). */
static HWND actual_window(HWND frame)
{
	wchar_t cls[256];
	if (!GetClassNameW(frame, cls, 256))
		return frame;
	if (wcscmp(cls, L"ApplicationFrameWindow") != 0 && wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0)
		return frame;

	DWORD parent_id = 0;
	GetWindowThreadProcessId(frame, &parent_id);
	for (HWND child = FindWindowExW(frame, NULL, NULL, NULL); child;
	     child = FindWindowExW(frame, child, NULL, NULL)) {
		DWORD child_id = 0;
		GetWindowThreadProcessId(child, &child_id);
		if (child_id != parent_id)
			return child;
	}
	return frame;
}

static QString window_exe_name(HWND hwnd)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid)
		return QString();

	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return QString(); /* e.g. an elevated process from a normal OBS */
	wchar_t path[MAX_PATH];
	DWORD cch = MAX_PATH;
	QString name;
	if (QueryFullProcessImageNameW(h, 0, path, &cch))
		name = lower_base_name(path);
	CloseHandle(h);
	return name;
}

/* title:class:exe, encoded like the OBS picker (window-helpers.c) */
static QString encode_field(const wchar_t *s)
{
	QString out;
	for (const wchar_t *p = s; *p; p++) {
		if (*p == L'#')
			out += QStringLiteral("#22");
		else if (*p == L':')
			out += QStringLiteral("#3A");
		else
			out += QChar(*p);
	}
	return out;
}

static QString build_selector(HWND hwnd)
{
	wchar_t cls[256] = {0};
	GetClassNameW(hwnd, cls, 256);
	wchar_t title[1024] = {0};
	GetWindowTextW(hwnd, title, 1024);

	wchar_t path[MAX_PATH] = {0};
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h) {
			DWORD cch = MAX_PATH;
			QueryFullProcessImageNameW(h, 0, path, &cch);
			CloseHandle(h);
		}
	}
	if (!path[0])
		return QString();

	const wchar_t *base = wcsrchr(path, L'\\');
	return encode_field(title) + QChar(':') + encode_field(cls) + QChar(':') + encode_field(base ? base + 1 : path);
}

/* ------------------------------------------------ source find + enumeration */

static bool is_capture_id(const char *id)
{
	return strcmp(id, "window_capture") == 0 || strcmp(id, "game_capture") == 0;
}

struct find_ctx {
	const char *uuid;
	obs_source_t *found; /* ref owned by the caller */
};

static bool enum_items(obs_scene_t *scene, obs_sceneitem_t *item, void *data);

static bool scan_scene(obs_source_t *scene_source, struct find_ctx *ctx)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene)
		obs_scene_enum_items(scene, enum_items, ctx);
	return true;
}

static bool enum_items(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	struct find_ctx *ctx = (struct find_ctx *)data;
	obs_source_t *src = obs_sceneitem_get_source(item);

	if (is_capture_id(obs_source_get_id(src)) && strcmp(obs_source_get_uuid(src), ctx->uuid) == 0) {
		ctx->found = obs_source_get_ref(src);
		return false; /* stop */
	}
	if (obs_scene_from_source(src))
		obs_scene_enum_items(obs_scene_from_source(src), enum_items, ctx);
	return true;
}

static obs_source_t *find_capture_source_by_uuid(const char *uuid)
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);

	struct find_ctx ctx = {uuid, nullptr};
	for (size_t i = 0; i < scenes.sources.num && !ctx.found; i++)
		scan_scene(scenes.sources.array[i], &ctx);

	for (size_t i = 0; i < scenes.sources.num; i++)
		obs_source_release(scenes.sources.array[i]);
	bfree(scenes.sources.array);

	return ctx.found;
}

struct enum_all_ctx {
	void (*cb)(void *, const char *uuid, const char *display);
	void *data;
	QString prefix; /* path to the scene currently being walked, "" = none yet */
};

static void walk_scene(obs_scene_t *scene, struct enum_all_ctx *ctx);

static bool enum_all_items(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	struct enum_all_ctx *ctx = (struct enum_all_ctx *)data;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const QString name = QString::fromUtf8(obs_source_get_name(src));
	const QString full = ctx->prefix.isEmpty() ? name : ctx->prefix + QStringLiteral("/") + name;

	if (is_capture_id(obs_source_get_id(src)))
		ctx->cb(ctx->data, obs_source_get_uuid(src), full.toUtf8().constData());

	obs_scene_t *child = obs_scene_from_source(src);
	if (child) {
		const QString saved = ctx->prefix;
		ctx->prefix = full;
		walk_scene(child, ctx);
		ctx->prefix = saved;
	}
	return true;
}

static void walk_scene(obs_scene_t *scene, struct enum_all_ctx *ctx)
{
	obs_scene_enum_items(scene, enum_all_items, ctx);
}

extern "C" void rr_process_watch_enum_capture_sources(void (*cb)(void *data, const char *uuid, const char *display),
						      void *data)
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);

	struct enum_all_ctx ctx = {cb, data, QString()};
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
		if (!scene)
			continue;
		/* display paths are anchored at the top-level scene name */
		ctx.prefix = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
		walk_scene(scene, &ctx);
	}
	ctx.prefix.clear();
	for (size_t i = 0; i < scenes.sources.num; i++)
		obs_source_release(scenes.sources.array[i]);
	bfree(scenes.sources.array);
}

/* ------------------------------------------------------- process snapshot */

static QStringList process_names_snapshot(const QStringList &wanted)
{
	QStringList hits;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return hits;

	PROCESSENTRY32W pe;
	memset(&pe, 0, sizeof(pe));
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			const QString name = lower_base_name(pe.szExeFile);
			if (wanted.contains(name) && !hits.contains(name))
				hits.append(name);
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return hits;
}

/* ------------------------------------------------------------- the watcher */

static QTimer *g_watch_timer = nullptr;
static obs_source_t *g_cached_src = nullptr; /* our own ref */
static QString g_cached_uuid;
static QString g_last_followed; /* lower exe currently followed, "" = none */
static QStringList g_prev_procs;
static bool g_have_prev = false;

static void drop_cached_src(void)
{
	if (g_cached_src)
		obs_source_release(g_cached_src);
	g_cached_src = nullptr;
	g_cached_uuid.clear();
}

static void retarget(const char *source_id, HWND hwnd, const QString &exe)
{
	const QString uuid = QString::fromUtf8(source_id);
	if (!g_cached_src || g_cached_uuid != uuid) {
		drop_cached_src();
		g_cached_src = find_capture_source_by_uuid(source_id);
		if (!g_cached_src) {
			obs_log(LOG_WARNING,
				"process watcher: the follow source is not in any scene anymore; pick it again in the "
				"dialog");
			g_last_followed.clear();
			return;
		}
		g_cached_uuid = uuid;
	}

	if (!is_capture_id(obs_source_get_id(g_cached_src))) {
		obs_log(LOG_WARNING, "process watcher: the follow source is no longer a window/game capture");
		g_last_followed.clear();
		return;
	}

	const QString selector = build_selector(hwnd);
	if (selector.isEmpty()) {
		obs_log(LOG_WARNING, "process watcher: could not read the foreground window target for %s",
			exe.toUtf8().constData());
		return;
	}

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "window", selector.toUtf8().constData());
	obs_data_set_string(s, "priority", "exe");
	obs_source_update(g_cached_src, s);
	obs_data_release(s);

	obs_log(LOG_INFO, "process watcher: '%s' now follows %s.exe", obs_source_get_name(g_cached_src),
		exe.toUtf8().constData());
}

static void watch_tick(void)
{
	struct rr_config cfg;
	rr_plugin_get_settings(&cfg);

	const QStringList wanted = parse_watch_list(cfg.watch_names);

	/* Rising-edge process starts: tracked even while disabled, so enabling
	 * mid-session never fires on processes that were already running. */
	const QStringList procs = process_names_snapshot(wanted);
	if (cfg.watch_autostart && g_have_prev) {
		for (const QString &name : procs) {
			if (g_prev_procs.contains(name))
				continue;
			struct rr_status st;
			rr_engine_get_status(&st);
			if (st.state == RR_STATE_OFF) {
				obs_log(LOG_INFO, "process watcher: %s.exe started, starting the buffer",
					name.toUtf8().constData());
				rr_plugin_start();
			} else {
				obs_log(LOG_INFO, "process watcher: %s.exe started, buffer already running",
					name.toUtf8().constData());
			}
		}
	}
	g_prev_procs = procs;
	g_have_prev = true;

	/* Foreground follow */
	if (!cfg.watch_follow || !cfg.watch_source_id[0] || wanted.isEmpty()) {
		g_last_followed.clear();
		drop_cached_src();
		return;
	}

	HWND fg = GetForegroundWindow();
	if (!fg)
		return;
	fg = actual_window(fg);
	const QString exe = window_exe_name(fg);
	if (exe.isEmpty())
		return; /* OBS itself, no window, or elevated: keep the last target */
	if (!wanted.contains(exe))
		return; /* not a followed app: keep the last target */
	if (exe == g_last_followed)
		return; /* already following this app */

	retarget(cfg.watch_source_id, fg, exe);
	g_last_followed = exe;
}

extern "C" void rr_process_watch_install(void)
{
	if (g_watch_timer)
		return;
	g_watch_timer = new QTimer(qApp);
	QObject::connect(g_watch_timer, &QTimer::timeout, []() { watch_tick(); });
	g_watch_timer->start(RR_WATCH_INTERVAL_MS);
}

extern "C" void rr_process_watch_uninstall(void)
{
	if (g_watch_timer) {
		g_watch_timer->stop();
		delete g_watch_timer;
		g_watch_timer = nullptr;
	}
	drop_cached_src();
	g_last_followed.clear();
	g_prev_procs.clear();
	g_have_prev = false;
}

extern "C" void rr_process_watch_refresh(void)
{
	/* settings changed: re-follow the current foreground, re-find the source */
	g_last_followed.clear();
	drop_cached_src();
}

/* Selftest hook: cache this source directly (retarget skips the scene walk
 * while the uuid matches), so the test does not have to touch the scene
 * graph from the watchdog thread. The watcher holds its own ref. */
extern "C" void rr_process_watch_pin_source(obs_source_t *src, const char *uuid)
{
	drop_cached_src();
	g_last_followed.clear();
	if (!src || !uuid || !*uuid)
		return;
	g_cached_src = obs_source_get_ref(src);
	g_cached_uuid = QString::fromUtf8(uuid);
}
