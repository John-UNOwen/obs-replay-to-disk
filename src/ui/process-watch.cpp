/* process-watch.cpp — the process watcher (UI thread, 2 s QTimer).
 *
 * Two settings-driven behaviors, inert until enabled:
 * - watch_follow: when the foreground window belongs to a listed exe, point
 *   the user-picked window/game capture source at that window. Only the
 *   foreground moves the source: an app from the list that merely starts in
 *   the background never steals the capture, and switching away from a listed
 *   app (Discord, desktop, ...) keeps the last followed window. A follow
 *   source that goes away (deleted, or turned into a non-capture) does not
 *   end the follow: the watcher retries on every tick, warns with a throttled
 *   sequence (first tries, then about every 30 s), and follows again on its
 *   own as soon as a source answers the uuid again.
 * - watch_autostart: when a listed exe appears in the process table
 *   (rising edge) and the buffer is off, start it. Start only: the watcher
 *   never stops the buffer, and a stop while the process runs is respected
 *   until the process starts again. The baseline is re-seeded whenever the
 *   list or the toggle changes, so an already-running app is never treated
 *   as a start.
 *
 * The source is retargeted exactly the way OBS's own picker encodes its combo
 * value (title:class:exe with # -> #22 and : -> #3A, see
 * libobs/util/windows/window-helpers.c add_window), so the source behaves as
 * if the user had picked the window in the source dialog. priority is pinned
 * to "exe" so a changing window title does not break the match.
 *
 * Lifetime rule: the watcher holds no strong source reference between ticks —
 * only a weak ref plus the uuid — because OBS 33 refuses to switch scene
 * collections while a plugin still holds one. See follow_target().
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
static obs_weak_source_t *g_cached_weak = nullptr; /* never keeps a source alive */
static QString g_cached_uuid;
static QString g_last_followed;   /* lower exe currently followed, "" = none */
static QString g_follow_warn_exe; /* exe of the current retarget failure streak */
static int g_follow_warns;        /* consecutive retarget failures for that exe */
static QStringList g_prev_procs;
static bool g_have_prev = false;
static QStringList g_prev_wanted; /* the parsed watch_names the baseline was taken against */
static bool g_watch_event_hooked = false;

static void drop_cached_src(void)
{
	if (g_cached_weak)
		obs_weak_source_release(g_cached_weak);
	g_cached_weak = nullptr;
	g_cached_uuid.clear();
}

/* Follow bookkeeping. g_last_followed may only ever claim "the source follows
 * this exe right now": watch_tick records it only after a successful
 * retarget(), and a failed one leaves it alone so the next tick retries.
 * Every site that drops the follow drops the warning streak with it — the
 * next failure is a fresh context (new source, new settings, new run) and
 * must warn from the top again. */
static void reset_follow_state(void)
{
	g_last_followed.clear();
	g_follow_warns = 0;
	g_follow_warn_exe.clear();
}

/* A failed retarget repeats on every tick while the listed app stays in the
 * foreground. Warn for the first few tries and then every 15th (~30 s at the
 * 2 s tick), so a vanished follow source can never go silently dead while the
 * app is up, and the log never spams every two seconds. A different exe
 * starts a new streak, so foregrounding another listed app always warns
 * immediately again. */
static void warn_follow_failure(const QString &exe, const QString &msg)
{
	if (exe != g_follow_warn_exe) {
		g_follow_warn_exe = exe;
		g_follow_warns = 0;
	}
	if (++g_follow_warns <= 3 || g_follow_warns % 15 == 0)
		obs_log(LOG_WARNING, "process watcher: %s", msg.toUtf8().constData());
}

/* The watcher keeps a *weak* ref to the follow source, so it never holds one
 * across an OBS scene teardown. OBS 33 ClearSceneData (scene-collection switch,
 * coordinate-mode migration, collection removal) removes every scene item,
 * pumps the destroy queue and then enumerates what is left: anything still
 * refcounted — even by a plugin — is logged as "Not all sources were cleared",
 * sets clearingFailed, and LoadData answers with the critical "Source Cleanup
 * Error" box and closes OBS. A weak ref does not show up there.
 *
 * It also gives the honest answer about a deleted source: obs_weak_source_get_source
 * returns NULL once the source is gone (libobs removes the uuid hash entry when
 * the source is destroyed), so the cache cannot keep "following" an orphan. Any
 * uuid mismatch or dead weak ref drops the cache and re-walks the scenes, which
 * is what makes a source that was deleted, or deleted and re-added under the
 * same uuid, recover.
 *
 * The strong ref returned here belongs to one retarget() call only. */
static obs_source_t *follow_target(const char *uuid)
{
	const QString id = QString::fromUtf8(uuid);
	if (g_cached_weak && g_cached_uuid == id) {
		obs_source_t *src = obs_weak_source_get_source(g_cached_weak);
		if (src)
			return src;
	}

	drop_cached_src(); /* a different source, or the cached one has died */

	obs_source_t *src = find_capture_source_by_uuid(uuid);
	if (src) {
		g_cached_weak = obs_source_get_weak_source(src);
		g_cached_uuid = id;
	}
	return src;
}

/* Points the follow source at hwnd. Returns true only when the source follows
 * exe afterwards; the caller records that in g_last_followed. A false leaves
 * the bookkeeping alone, so the next tick tries again — a deleted or
 * replaced follow source is retried until follow_target resolves the uuid
 * again, and warn_follow_failure keeps that visible in the log. */
static bool retarget(const char *source_id, HWND hwnd, const QString &exe)
{
	obs_source_t *src = follow_target(source_id);
	if (!src) {
		warn_follow_failure(exe, QStringLiteral("the follow source is not in any scene anymore; "
							"pick it again in the dialog"));
		return false;
	}

	if (!is_capture_id(obs_source_get_id(src))) {
		obs_source_release(src);
		warn_follow_failure(exe, QStringLiteral("the follow source is no longer a window/game capture"));
		return false;
	}

	const QString selector = build_selector(hwnd);
	if (selector.isEmpty()) {
		obs_source_release(src);
		warn_follow_failure(exe,
				    QStringLiteral("could not read the foreground window target for %1.exe").arg(exe));
		return false;
	}

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "window", selector.toUtf8().constData());
	/* "priority" is an OBS_COMBO_FORMAT_INT setting on both captures
	 * (window-capture.c and game-capture.c read it with obs_data_get_int);
	 * 2 = WINDOW_PRIORITY_EXE from libobs/util/windows/window-helpers.h.
	 * A string item reads back as 0 = WINDOW_PRIORITY_CLASS. */
	obs_data_set_int(s, "priority", 2); /* WINDOW_PRIORITY_EXE */
	obs_source_update(src, s);
	obs_data_release(s);

	obs_log(LOG_INFO, "process watcher: '%s' now follows %s.exe", obs_source_get_name(src),
		exe.toUtf8().constData());
	obs_source_release(src);
	g_follow_warns = 0;
	g_follow_warn_exe.clear();
	return true;
}

static void watch_tick(void)
{
	struct rr_config cfg;
	rr_plugin_get_settings(&cfg);

	const QStringList wanted = parse_watch_list(cfg.watch_names);

	/* Rising-edge process starts (watch_autostart). The snapshot only ever
	 * counts names on the list, so the baseline is meaningful only against
	 * the list it was taken against: when the parsed list changes, re-seed
	 * the baseline from the current snapshot before comparing, so an app
	 * that was already running when its name was added is not treated as a
	 * start (README). No snapshot is taken while the setting is off;
	 * toggling watch_autostart goes through the dialog, and every Apply
	 * calls rr_process_watch_refresh, which drops the baseline — so the
	 * first tick after enabling seeds it without firing. */
	if (cfg.watch_autostart) {
		const QStringList procs = process_names_snapshot(wanted);
		if (g_have_prev && wanted == g_prev_wanted) {
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
		g_prev_wanted = wanted;
		g_have_prev = true;
	}

	/* Foreground follow */
	if (!cfg.watch_follow || !cfg.watch_source_id[0] || wanted.isEmpty()) {
		reset_follow_state();
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

	if (exe == g_last_followed) {
		/* The same listed app is still foreground. Keep the last target,
		 * but confirm the followed source is still alive first: while the
		 * cached weak ref resolves this is one atomic get and no scene
		 * walk. If the source died since (deleted while followed), the
		 * bookkeeping cannot keep claiming it follows — clear only
		 * g_last_followed here (a reset_follow_state() would restart the
		 * warning streak every tick), and the retry path below reports
		 * the loss and re-follows as soon as a source answers the uuid. */
		obs_source_t *src = follow_target(cfg.watch_source_id);
		if (src) {
			obs_source_release(src);
			return;
		}
		g_last_followed.clear();
	}

	if (retarget(cfg.watch_source_id, fg, exe))
		g_last_followed = exe;
}

/* OBS is about to tear down (or has torn down) every scene source. OBS 33
 * ClearSceneData fires CLEANUP from inside itself, after it removed all scene
 * items and just before it enumerates the sources that are left — that
 * enumeration is what turns a lingering refcount into the "Source Cleanup
 * Error" box, and OBSStudioAPI::on_event lets only CLEANUP and EXIT through
 * while disableSaving is set (OBSStudioAPI.cpp), so CLEANUP is the hook that
 * matters; CHANGING fires earlier, at the request to switch or drop the
 * collection. The watcher is UI-thread only, and so is this callback. */
static void watch_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		drop_cached_src();
		reset_follow_state();
		break;
	default:
		break;
	}
}

extern "C" void rr_process_watch_install(void)
{
	if (g_watch_timer)
		return;
	if (!g_watch_event_hooked) {
		obs_frontend_add_event_callback(watch_frontend_event, NULL);
		g_watch_event_hooked = true;
	}
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
	if (g_watch_event_hooked) {
		obs_frontend_remove_event_callback(watch_frontend_event, NULL);
		g_watch_event_hooked = false;
	}
	drop_cached_src();
	reset_follow_state();
	g_prev_procs.clear();
	g_prev_wanted.clear();
	g_have_prev = false;
}

extern "C" void rr_process_watch_refresh(void)
{
	/* settings changed: re-follow the current foreground, re-find the source */
	reset_follow_state();
	drop_cached_src();
	/* Drop the autostart baseline too: it is list-relative, and an Apply is
	 * the only way watch_names or the watch_autostart toggle changes outside
	 * the selftest. The next tick re-seeds it from the current process set,
	 * so enabling the toggle or adding an already-running app's name to the
	 * list never counts as a rising edge (README). */
	g_prev_procs.clear();
	g_prev_wanted.clear();
	g_have_prev = false;
}

/* Selftest hook: cache this source directly (retarget skips the scene walk
 * while the uuid matches), so the test does not have to touch the scene
 * graph from the watchdog thread. Only a weak ref is taken: the test owns the
 * source, and the watcher must not be the reason it outlives a scene teardown.
 * UI thread only, like every other watcher entry point: the cache drop here
 * is not atomic against a timer tick. The selftest watchdog reaches it
 * through an OBS_TASK_UI task, never directly. */
extern "C" void rr_process_watch_pin_source(obs_source_t *src, const char *uuid)
{
	drop_cached_src();
	reset_follow_state();
	if (!src || !uuid || !*uuid)
		return;
	g_cached_weak = obs_source_get_weak_source(src);
	g_cached_uuid = QString::fromUtf8(uuid);
}
