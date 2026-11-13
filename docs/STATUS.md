# Status

## WP0 repository and build: DONE
- Date: 2026-11-12
- Commits: 516cb55 f379783 f379bc1 af8fd45 beace0e 9ea373c e8600dd af4291b 5ba9539 e9c17dd (+ this status commit)
- Acceptance:
  - PASS: The three build commands succeed. `cmake --preset windows-x64`
    (configure, "Configuring done (71.7s) / Generating done"), `cmake --build --preset
    windows-x64 --config RelWithDebInfo` (links obs-replay-to-disk.dll),
    `cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo`
    (installs DLL+PDB to `release/RelWithDebInfo/obs-replay-to-disk/bin/64bit/`), all exit 0.
    A clean rebuild of the plugin target with the CI preset (`CMAKE_COMPILE_WARNING_AS_ERROR`)
    also passed with no warnings.
  - PASS: Test-rig OBS 33.0.1 (portable, `.testrig/obs`) loaded the plugin from
    `obs-plugins/64bit/`. Log `2026-11-12 05-53-14.txt`:
    `[obs-replay-to-disk] plugin loaded successfully (version 0.1.0)` and
    `[obs-replay-to-disk] avformat 62.12.102` → avformat major 62, as expected for the
    obs-deps FFmpeg 8.1.2. OBS closed via window message; log tail: `Number of memory leaks: 0`.
  - PASS: CI green on Windows. Run 43205165284 (push of e9c17dd): clang-format success,
    gersemi success, "Build for Windows" success; macOS and Ubuntu jobs skipped as configured.
    https://github.com/John-UNOwen/obs-replay-to-disk/actions/runs/43205165284
    (first run 43204558389 failed on the two issues listed under Deviations.)
- Hashes used in buildspec.json:
  - obs-studio 33.0.1 source zip (computed): `0f228b960c099c6f6bca9f09a0c3516f14fdf1dd96e1d4702145c5d3ba8cba31`
  - prebuilt 2026-10-23 windows-x64: `7508972a867a0ef68c26cd8bdba61fd999edd0a92fa9eb9585774de9bf51bfbe` (handoff B2)
  - qt6 2026-10-23 windows-x64: `78cd7a138464bb6edcdd3b85fcd63a5a7c69e369629963f3bc395bab74fb3f47`, debugSymbols `5c97809dcb663a51e5b5f47983359d03a213a8c711c4b158a1b62429b8dff6be` (handoff B2)
- Deviations from the handoff:
  - Local toolchain: no Visual Studio 2022 on this host. Builds run against "Visual Studio
    Build Tools 2026" v18.9 (MSVC toolset 14.51.36231 / compiler 19.51, Windows SDK 10.0.26100)
    at `%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools`; cmake 4.3.1 and ninja 1.13.2
    exist only inside that tree and are not on PATH. The local `windows-x64` preset generator is
    retargeted from "Visual Studio 17 2022" to "Visual Studio 18 2026"; the SDK pin
    `version=10.0.22621` is dropped to plain `x64` there because only 10.0.26100 is installed.
  - CI keeps the VS 2022 generator: the `windows-ci-x64` preset pins
    `generator: "Visual Studio 17 2022", architecture: "x64,version=10.0.22621"`. GitHub
    hosted x64 Windows images (windows-2022 and windows-2025, Nov 2026) ship only Visual
    Studio 2022; VS 2026 images are arm64-only. The first CI run 43204558389 failed with
    "CMake Error: Could not create named generator Visual Studio 18 2026".
  - `cmake/finders/FindFFmpeg.cmake` is fenced with `# gersemi: off` / `# gersemi: on` and
    otherwise byte-identical to obs-studio 33.0.1: on the first push the current brew gersemi
    wants to reformat the upstream file (OBS formatted it with an older gersemi), so the fences
    keep the vendored content unchanged instead of diverging from upstream.
  - `buildspec.json` keeps `"email": ""`: `cmake/common/bootstrap.cmake` reads the key and a
    missing key aborts configure; the owner decision is no personal email.
  - macOS hashes for obs-deps 2026-10-23 are the `universal` assets from the release notes
    (handoff B2 only gave windows-x64); the obs-studio 33.0.1 macos hash equals the computed
    windows-x64 hash because both keys point at the same source archive URL.
  - This OBS 33.0.1 build ships bundled plugins in `core/<name>/` rather than
    `obs-plugins/64bit/`, but a plugin installed under `obs-plugins/64bit/` (handoff 7.1) loads
    fine, so the rig keeps the handoff layout.
- Open problems: none.

## WP1 spike inside real OBS: DONE
- Date: 2026-11-12
- Commits (branch `wp1-spike`, throwaway code, not merged to main): 9048308 3f25253
  ffa3deb (+ this status/docs commit). Deliverables on main: docs only.
- Full measurements and verdicts: `docs/wp1-findings.md`.
- Acceptance:
  - PASS U1: start and stop from the worker thread work, OBS exits cleanly.
    Spike worker (`CreateThread`) creates encoders + both `ffmpeg_muxer` outputs,
    starts them, and stops both from that thread. Every run: `selftest finished:
    ok`, `plugin unloaded`, clean WM_CLOSE exit (`OBS_EXITED_CLEANLY`), no crash.
    Evidence runs A2/B2/C/D logs.
  - PASS U2: 0 dropped/lagged frames attributable to the plugin with OBS
    recording at the same time (Intel QSV; NVENC/AMF untestable on this
    Intel-only host). Run B2 (`obs_qsv11_v2` + `--startrecording`, 40.8 MB
    recording): all 5 s UI-timer ticks and the final line
    `lagged_frames=0 rr_dropped=0 ref_dropped=0`.
  - PASS U3: DLL loads in OBS 33.0.1 and `rr_remux` runs in-process. Load line
    `[obs-replay-to-disk] avformat 62.12.102` in every run; remux runs inside
    obs64.exe with no child process, producing a verified `spike.mp4`.
  - PASS: `verify_remux.py` prints OK for every stream, both modes, exit 0.
    `--segments`: 5 segments, video+audio `continues`, no gaps. Replay vs
    reference: `matched_in_order==packets`, `spread≈0.667 ms`,
    `dts_monotonic=True`, `dts<=pts=True`, `alignments=1`, A/V offset −5.333 ms.
  - PASS: `ffmpeg -f null` prints no warnings. `ffmpeg -v warning -i spike.mp4
    -map 0 -f null -` was silent in every run.
  - PASS U4: same passes on hardware encoders and HEVC. `obs_qsv11_v2` (H.264,
    runs B/B2), `obs_qsv11_hevc` (run C, `spike.mp4` ffprobe tag `hevc,hvc1`),
    `obs_qsv11_av1` (run D, `av01` Main). All verify-clean, `grid_warnings=0`.
    NVENC/AMF untested (no such hardware on this host).
  - PASS: Segment closure time recorded, well under 100 ms. `rr_file_closed`
    (snippet 8.4) returned true on its first 20 ms poll for every segment of
    every run (`after 0ms`); the check starts 7–60 ms after the boundary event.
  - PASS (with note): OBS log tail. `plugin unloaded` in every run. No crash.
    `Number of memory leaks: 1` in every plugin session, and also 1 in a
    plugin-free second-launch control run with the DLL removed — stock OBS
    33.0.1 leaks that 1 allocation from the second session of a config onward
    (first-session controls show 0; WP0's `0` was a first session). Plugin
    adds 0 on top of the rig baseline. Control matrix in `docs/wp1-findings.md`.
- Deviations from the handoff:
  - `obs_frontend_get_stats()` does not exist in OBS 33.0.1's frontend API, and
    `video_output_get_skipped_frames()` is gone from libobs. Periodic
    dropped/lagged logging uses a UI-thread QTimer shim calling
    `obs_get_total_frames()` / `obs_get_lagged_frames()` /
    `obs_output_get_frames_dropped()` (same data as the old
    `numDroppedFrames`/`numLaggedFrames` fields).
  - Encoder settings built from `obs_encoder_defaults(id)` per 5.5 rather than
    the Linux reference's fresh `obs_data_t`; needed for valid Windows QSV
    presets. Call order otherwise identical to the reference.
  - `verify_remux.py --segments` globs `seg_*.mkv`, so D2 `rrseg_*` segments are
    copied to a `verify/` dir as `seg_NNN.mkv` (chronological) for that check.
  - QSV runs show `split_file→file_changed` latency of 798–877 ms, above the
    handoff's 164–632 ms headless band but well inside the 3 s E5 budget.
  - Added `rr_spike_wait_worker()` (bounded 20 s) in `obs_module_unload` so a
    mid-spike OBS exit cannot unload the DLL while the worker thread runs.
- Open problems: none. U5 (HDR) intentionally untouched. NVENC/AMF variants of
  U2/U4 need an NVIDIA/AMD host; owner decides if/when.

## WP2 pure-C core and tests: DONE
- Date: 2026-11-12
- Commits: 5e55bb7 (cherry-pick remux from wp1-spike) bf5d62c a66cf6e 151f0c4
  bc04e75 659ee78 e8a1c69 707e8a3 7c18cfc (+ this status commit)
- Acceptance:
  - PASS: `cmake --preset windows-x64 -DENABLE_TESTS=ON`,
    `cmake --build --preset windows-x64 --config RelWithDebInfo`, and
    `ctest --test-dir build_x64 -C RelWithDebInfo` all succeed:
    `1/1 Test #1: core ..... Passed 0.01 sec` /
    `100% tests passed, 0 tests failed out of 1` (exit 0). The `core` test is
    `tests/test_core.c`: one plain-`assert` executable with a tiny `RUN(test)`
    macro, no framework, running all 18 required WP2 test functions; it prints
    each test name and ends `all tests passed` (exit 0). Expected values are
    hand-written (E6 10.83 s/59.94 fps, 11.7 s first segment, 300 KiB 1 s
    segment, D10 boundary at ±1 ns, 6.75e9 B for 30 Mbps × 1800 s).
    Assertions stay live under RelWithDebInfo (`#undef NDEBUG` before
    `#include <assert.h>`); verified live: a temporary `assert(1+1==3)` aborted
    the run (exit -1073740791 = 0xC0000409) instead of being optimized out.
  - PASS: `rr_core` compiles with no warnings under the CI warning posture. The
    CI preset `windows-ci-x64` pins the VS 2022 generator, which this host lacks
    (WP0 deviation), so its posture (`/W3` from `compilerconfig` + `/WX` via
    `CMAKE_COMPILE_WARNING_AS_ERROR`) was reproduced in a throwaway configure
    dir kept out of the repo by the `build_*/` gitignore:
    `cmake --preset windows-x64 -B build_check -DENABLE_TESTS=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=ON`
    then `cmake --build build_check --config RelWithDebInfo --target rr_core`
    → exit 0, zero warnings. A stricter second pass
    (`-DCMAKE_C_FLAGS="/W4 /WX"`, MSVC's `-Wall` equivalent at level 4, warnings
    fatal) also compiled all six `rr_core` TUs (five `core/` modules +
    `save/remux.c` with its FFmpeg headers) with exit 0 and zero warnings.
    The regular `ENABLE_TESTS=ON` plugin build likewise logged 0 compiler
    warnings.
  - PASS (WP2 step 4): the `-DRR_REMUX_MAIN` CLI builds as `rr-remux-cli` from
    `src/save/remux.c` + FFmpeg. Smoke: joining run A's five WP1
    `rrseg_*.mkv` gave `video_frames=1291 duration=21.516667 delay=2
    grid_warnings=0 skipped_tail=0`, exit 0, 162 MB mp4. (Runtime needs the
    obs-deps bin on PATH: `.deps/obs-deps-2026-10-23-x64/bin`, same DLLs OBS
    ships.)
- Deviations from the handoff:
  - Settings clamp helper placement: `rr_settings_clamp_window_sec` (30–1800,
    5.8) lives in `src/core/settings-clamp.c/.h` inside `rr_core`, so
    `window_seconds_clamped_30_1800` needs no libobs; `plugin/settings.c` (WP3)
    uses the same header. The four 5.10 interfaces are implemented verbatim.
  - `src/save/remux.c` got one clang-format 19 whitespace commit (bf5d62c)
    after the cherry-pick: the reference file has four continuation-line spots
    that violate the repo's own `.clang-format`, and the CI format job checks
    every file under `src/`. Format only, no text change.
  - `.gitignore` whitelist gained `!/tests` — the template excludes everything
    and did not yet allow the handoff 5.2 `tests/` directory.
  - `rr_window_select` does not pin (5.10 passes the ledger as `const`):
    selection returns `{first, count}` and WP3's save flow calls
    `rr_ledger_pin`, matching 5.6 step 5's "Pin them" on the worker side.
- Open problems: none.

## WP3 buffer engine: DONE
- Date: 2026-11-12
- Commits: bee74d7 423f3a1 ed88508 4554237 03183bc cc92737 (+ this status commit)
- Acceptance (rig, `RR_SELFTEST=buffer`, window 30, runs in `.testrig/runs/wp3-r1|-r2|-r3`;
  encoder auto-resolved to `obs_qsv11_v2`, rig fps 30/1):
  - PASS: Log shows `buffer started`, `segment closed` about every 10 s, `pruned` starting
    after ~55 s. wp3-r1 log `2026-11-12 14-04-16`: `buffer started: encoder=obs_qsv11_v2
    fps=30/1 tracks=1 window=30s dir=...`; 11 `segment closed` lines with intervals
    `10, 10.01, 9.98, 10.01, 10.01, 10.01, 9.99, 10, 10.01, 10` (first segment 11.45 s,
    per E6); first `pruned:` line at +61.5 s after `buffer started` (D10: the first
    segment ends +11.5 s, becomes prunable at +56.5 s, is pruned at the next boundary
    +61.5 s — 30 s window + 15 s margin, as designed). Run 2 (`14-07-06`) identical:
    first prune +61.5 s, 6 prunes, cadence ≤10.01 s.
  - PASS: buffer folder never holds more than 6 `rrseg_*.mkv`. Run 2 polled every 2 s
    from pwsh while OBS ran: `MAX_CONCURRENT_SEGMENTS=6 samples=62`
    (`.testrig/runs/wp3-r1/poll-run2.txt`).
  - PASS: `keep-me.txt` placed in the buffer folder before the runs (including the
    purge-at-start and purge-at-stop runs) still exists afterwards; post-stop listing of
    `wp3-r1\buffer` shows only `keep-me.txt` (15 B).
  - PASS: after `buffer stopped`, no `rrseg_*.mkv` remain. Stop purge summary:
    `engine: purged 3 segment file(s) on stop` then `buffer stopped` (wp3-r2 run 2, exit
    while buffering); directory listing after exit: empty.
  - PASS: kill OBS mid-run, restart: stale segments purged at next start. Killed the
    saved-PID obs64 (93728) at ~40 s; `obs-ffmpeg-mux` PID 167276 was gone in the same
    listing (E21 reconfirmed, mux never killed by us); 4 stale segments on disk. After
    config rebuild and restart into the same `RR_SELFTEST_DIR`, the log shows four
    `engine: purged: ...\rrseg_2026-11-12_14-09-*.mkv` lines at start and the dir holds
    only new `rrseg_..._14-10-*.mkv` files.
  - PASS: OBS closes within 5 s while the buffer runs, no crash, leaks line at 0 for
    this plugin's objects. WM_CLOSE (EnumWindows on the owned PID) → `OBS_EXITED_CLEANLY`:
    1.0 s (r1 run 1), 0.9 s (r1 run 2), 0.9 s (r2 run 2, closed while buffering: engine
    logged `buffer stopped` via the EXIT handler before `plugin unloaded`), 1 s (r3).
    Leaks control: r1 run 1 was the first session of a rebuilt fresh config and logged
    `Number of memory leaks: 0`; every later session of a config logged 1, matching the
    WP1 stock-OBS baseline (second-session leak, plugin adds 0). No crash sentinels or
    exceptions; `plugin unloaded` every session.
  - PASS: switching profile while buffering does not log "Tried to reset when already
    active". Method: OBS 33 built-in obs-websocket 5.7.6 (`plugin_config/obs-websocket/
    config.json` → `server_enabled=true`, `auth_required=false`), python `websockets`
    client, request `SetCurrentProfile` (this build's name; `SwitchToProfile` and
    `LoadProfile` return code 204) twice: Untitled → RigB → Untitled (rig profile
    `basic/profiles/RigB`). Log grep for `Tried to reset when already active`: 0 hits.
    Engine logged for each switch: `buffer stopped` at PROFILE_CHANGING, then
    `engine: profile changed, autostarting the buffer` + `buffer started` (autostart was
    set in `settings.json`, exercising the 5.8 load path end to end).
  - PASS: zero-warning builds. `cmake --preset windows-x64 -DENABLE_TESTS=ON` +
    `cmake --build --preset windows-x64 --config RelWithDebInfo`: 0 warnings, ctest
    `core` passes. CI warning posture (`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` in a
    throwaway dir, tests ON): all targets, 0 warnings, exit 0. Stricter `/W4 /WX` one-off
    (`-DCMAKE_C_FLAGS="/W4 /WX"`): plugin TUs `plugin-main.c engine.c platform-win.c
    settings.c` + `rr_core` all compile with 0 warnings; the libobs headers did NOT force
    a /W3 downgrade — /W4 is clean as-is.
  - PASS: clang-format 19.1.1 (`--dry-run --Werror` clean on all new/changed C files) and
    gersemi 0.29.2 (`--check` clean on CMakeLists.txt).
- Deviations from the handoff:
  - OBS 33 frontend API has no `obs_frontend_get_recording_directory()` (reconfirmed: the
    symbol is absent from obs.dll). The recording folder is resolved on the UI thread from
    the profile config: `Output/RecFilePath`, else `SimpleOutput/FilePath`, else
    `%USERPROFILE%\Videos`.
  - `obs_data_merge` does not exist in OBS 33.0.1; `extra_encoder_settings` is merged
    with `obs_data_apply(settings, extra)` (copies the extra JSON's items over the
    defaults), before `keyint_sec=1` is set last (5.5).
  - The `split_file` proc helper (8.2) lands with the WP4 save flow: WP3's `SAVE` is a
    log-and-ignore stub by spec, so the proc would have no caller.
  - Any `OUTPUT_STOPPED` that arrives while buffering without a stop request runs the 5.7
    sequence (5.3 covers non-zero codes; a zero-code unexpected stop would otherwise hang
    a dead output in BUFFERING). Error text comes from `obs_output_get_last_error` or
    "the output stopped unexpectedly (code N)".
  - The self-test watchdog is a plain worker thread (like WP1's worker) instead of a Qt
    timer; it logs a `selftest: t=...s state=... total_frames=... lagged=N` line every
    10 s using the OBS 33 getters (wp1-findings), then stops the buffer and logs
    `selftest finished: <ok|failed> <reason>`.
  - Extra (non-5.9) engine log lines: `engine: purged: %s` (stale purge at start, used
    as R2 evidence), `engine: purged %d segment file(s) on stop`, start/stop ignore
    notices. The exact 5.9 lines are unchanged.
  - Rig-local: `ConfirmOnExit=false` in the test `user.ini` (WP1 deviation 7, needed for
    unattended WM_CLOSE); the task's suggested `[WebSockets]` global.ini section is the
    pre-OBS-28 layout — OBS 33 stores websocket settings in the obs-websocket
    `config.json` shown above.
- Open problems:
  - `rr_settings_save()` has no in-app caller until WP5, so its write path itself has
    not run in OBS; the file format/keys were exercised in R3 through the load path with
    a hand-written `settings.json`.
  - The self-test media source gets persisted as a global source in the rig's scene
    collection at OBS exit, so a second run on the same config logs one cosmetic
    `duplicate name "rr_selftest_media"` warning at load. Harmless for WP3 (R3 run was
    such a session); WP5's hotkeys/dialog need to tolerate a reloaded global source.
  - A pre-existing rig OBS instance (PID 112092, started 05:05 before this WP) was
    present during the runs; it was left untouched (all launches/kills/closes targeted
    only PIDs started by this WP).

## WP4 save flow: DONE — acceptance G PASS 2026-11-13 (host wedge cleared on its own)
- Date: 2026-11-12
- Commits: 103c14b 5324a7f 661577f 4a12229 (+ this status commit)
- Acceptance (rig, `RR_SELFTEST=save`, encoder `obs_qsv11_v2` default CBR, media
  `.testrig/media/src.mp4` 30 fps, runs in `.testrig/runs/wp4-*`; OBS logs print
  UTC timestamps ≈7 h behind host wall clock):
  - PASS: `RR_SELFTEST_SAVES=20,60` with `RR_SELFTEST_WINDOW=30` gives a ~20 s clip
    and a full-window clip. Run wp4-A2: `Replay_2026-11-12_14-31-53.mp4` ffprobe
    duration 20.032 s (600 frames, band 19–21 s) and
    `Replay_2026-11-12_14-32-33.mp4` 31.000 s (930 frames, band 30.0–31.1 s);
    `verify_remux.py` exit 0 with `matched_in_order==packets` on both vs the
    `RR_SELFTEST_REFERENCE=1` `reference.mkv`; `ffmpeg -v warning -map 0 -f null -`
    silent for both; both `save finished:` lines show `warnings=0`. The first attempt
    (wp4-A) FAILED at 18.00 s: the selftest run clock started at thread creation, so
    save@20 s fired after ~18 s of buffered video (encoder init ≈2.4 s + scene load);
    4a12229 gates the run clock on `BUFFERING` and A2 passed.
  - PASS: `since_last_save` vs `window` scope. wp4-B1 (`SCOPE=since_last_save
    SAVES=60,72`): second clip 12.00 s (360 frames) — exactly the time since the
    previous save. wp4-B2 (`SCOPE=window`, same SAVES): second clip 30.000 s — the
    window clamp (`max(window_start, last_save_end)` does not apply; the window wins).
  - PASS: two saves ≤1 s apart → exactly one remux, the other logged and ignored,
    file intact. wp4-C (`SAVES=45,45.5`): log shows one `save started:`/`save
    finished:` pair and one `engine: SAVE ignored (saving)`;
    `Replay_2026-11-12_14-38-00.mp4` verifies 900/900 video and 1407/1407 audio
    packets (`exit 0`), ffmpeg silent, duration 30.021 s.
  - PASS: `RR_SELFTEST_TRACKS=2` saves two audio tracks. ffprobe on the saved mp4
    lists exactly `0 h264 video`, `1 aac audio`, `2 aac audio`.
  - PASS: no `*.rrpart` after completed saves; kill mid-remux leaves the part; next
    start deletes it and the save folder works again. Listing after A2/B1/B2/C/D: 0
    `*.rrpart`. wp4-E1 (window 120 s + `Bitrate=150000` to widen the remux): killed
    the saved-PID obs64 (96172) ~22 ms after `save started` 14:42:14.436;
    `Replay_2026-11-12_14-42-14.mp4.rrpart` (41,418,800 B) remained (and
    `obs-ffmpeg-mux` died with OBS — E21, never killed by us; user OBS 112092
    untouched). A restart into the same buffer directory logged `engine: removed
    stale part: ...\Replay_2026-11-12_14-42-14.mp4.rrpart`, and its own save then
    completed normally (0 parts left after; `Replay_2026-11-12_14-42-50.mp4`).
  - PASS: pruning pauses during SAVING and resumes after. wp4-F (window 420 s):
    `save started` 14:50:46.269 → `save finished: ... took=1.47s warnings=0`
    14:50:47.741; zero `pruned:` lines in that window (the `prune()` early-return);
    pruning continued right after unpin, the first two backlogged candidates pruned
    within one boundary (`pruned: ...rrseg_2026-11-12_14-43-23.mkv`, `..._14-43-37.mkv`).
    Run total: 4 prunes, 0 during SAVING. A dedicated 1800 s/150 kbps run for a longer
    pinned-backlog burst could not start (see Open problems); the pause + resume-after
    behavior is fully evidenced above.
  - PASS (run 2026-11-13 06:49-06:53, rig healthy again — see the RESOLVED note
    under Open problems): close OBS via WM_CLOSE with the save flow live, the
    scripted recipe (`start-save.ps1` + a scratch close script mirroring
    `close-at-save.ps1`; window 30, `SAVES=55`, `REFERENCE=0`; runs
    `wp4-G1/G2/G3`, PIDs 29152/6724/13712). Acceptance path (a) all three
    times — the remux finished before the close could land (`took=0.05s`/`0.07s`
    on hot cache is faster than any log-polling close: G2's `WM_CLOSE` went out
    25 ms after `save finished`): G1 `save started: segments=4 inpoint=4.01s`
    06:50:14.733 → `save finished: ...Replay_2026-11-13_06-50-14.mp4
    frames=900 duration=30.00s took=0.07s warnings=0` 06:50:14.805 → WM_CLOSE
    2.1 s later; G2 close at DelayMs=0, exit 0.22 s after WM_CLOSE; G3
    (`BITRATE=150000` to widen the remux — qsv delivered ~19 Mbps, remux still
    0.05 s) exit 0.24 s. No crash: each log ends clean —
    `engine: quitting with the output active, forcing stop` / `engine: purged
    7 segment file(s) on stop` / `buffer stopped` / `selftest finished: failed
    interrupted (OBS exit)` (the watchdog's interruption reason) / `plugin
    unloaded`; exit times 0.22 / 0.22 / 0.24 s (≤5 s). Leaks `1` in all three
    = the stock-OBS control on the same config that morning (also 1), so the
    plugin adds 0. Each run dir: exactly one final `Replay_*.mp4`,
    `*.rrpart` count 0 (recursive); no retry beyond the three attempts was
    needed. The (b) quit-flag cancel path stays unexercised at runtime (the
    70 ms remux window is unreachable by log polling); E1 keeps the
    kill-mid-remux evidence.
  - PASS (after the final state): zero-warning builds. `cmake --build --preset
    windows-x64 --config RelWithDebInfo`: exit 0, 0 warnings; CI posture
    (`build_check`, `CMAKE_COMPILE_WARNING_AS_ERROR=ON`): exit 0; stricter
    (`build_w4`, `/W4 /WX`): exit 0; `ctest --test-dir build_x64 -R core
    -C RelWithDebInfo`: `100% tests passed`; clang-format 19.1.1
    `--dry-run -Werror` clean over all `src/**/*.{c,h}`; gersemi 0.29.2 `--check`
    clean on `CMakeLists.txt`.
- Deviations from the handoff:
  - 5.6 step 8's "play the save sound": `rr_play_save_sound()` in `platform-win.c`
    uses `MessageBeep(MB_OK)` / `MessageBeep(MB_ICONERROR)` — system sound, no media
    file, no-op on non-Windows.
  - `RR_SELFTEST_TRACKS` and `RR_SELFTEST_BITRATE` are selftest-only overrides (used
    for D and the widened remux); production settings keep the 5.8 values.
  - The selftest run clock is gated on `BUFFERING` (4a12229) so `RR_SELFTEST_SAVES`
    counts seconds of buffered video; without it hardware-encoder init delays shorten
    the first clip (wp4-A, see A box).
  - The global proc `void rolling_replay_save()` and the
    `void rolling_replay_saved(string path)` signal are registered in
    `obs_module_load` on the process-wide handlers. OBS 33.0.1 offers no
    `proc_handler_remove` and no per-handler signal removal (only
    `signal_handler_remove_current` from inside a callback), so they live for the
    process lifetime; they read engine state under the status mutex and no-op when
    the engine is stopped. Duplicate-registration warnings on reload are checked.
  - `save_paths` capacity `RR_MAX_SAVE_SEGS=512` (> 1800 s / ~10 s + headroom): a
    selection over capacity fails the save through the normal `save failed:` path.
  - Rig-local: `wait-run.ps1` pins the OBS log by the `runs\<tag>` string because
    OBS keeps an open log flush-ambiguous while another `--multi` instance runs
    (newest-by-mtime picked the wrong instance's log).
- Open problems:
  - RESOLVED 2026-11-13, no reboot: the host wedge below cleared on its own.
    The 06:45 probe (`.testrig/obs/config/obs-studio/logs/2026-11-13
    06-45-23.txt`) showed the window after ~5 s and a clean WM_CLOSE exit on a
    freshly rebuilt config (`Number of memory leaks: 0`); G and the WP5 load
    smoke all ran normally at 06:47-06:53. Rig rule kept: one instance per
    portable config at a time, closes only by WM_CLOSE to the saved PID.
  - History — G was blocked by a host fault that started ≈21:54 local, after F finished:
    every new obs64 (with or without this plugin, fresh portable config, with or
    without `--multi`/`--minimize-to-tray`) hangs at the same point — the log ends at
    `Created scene collection 'Untitled' (clean, ...)` / `Loaded scenes` and the main
    thread never returns (frida stack shows Qt window-show/DWM frames, 0 context
    switches; a `MessageBoxTimeoutW` probe shows windows, the user's long-running OBS
    112092 stays responsive, `DwmGetWindowAttribute` answers instantly). Hung
    instances were killed by saved PID only; a reboot or logoff is needed before G (or
    any further OBS run) can execute. F's pinned-backlog-burst variant was blocked by
    the same fault.
  - `rolling_replay_saved` fires from the save path but no in-app subscriber exists
    yet; WP5's UI will consume it.
  - `rr_selftest_media` persists as a global source in the rig collection (WP3
    carry-over).


## WP5 user interface and hotkeys: DONE — load smoke PASS 2026-11-13; manual acceptance PENDING (owner)
- Date: 2026-11-13
- Commits: 1b604e0 (+ this status commit)
- Acceptance (handoff WP5 is "manual, in the test rig"; the host wedge cleared
  2026-11-13 (see the WP4 Open-problems RESOLVED note) and the load smoke below
  ran on this host — the four click-through lines need a human at the OBS
  window and stay owner-pending):
  - PENDING: Both hotkeys appear in Settings → Hotkeys, work, and still work
    after restarting OBS.
  - PENDING: Each setting survives an OBS restart.
  - PENDING: A buffer folder of `C:\` or the output folder is refused with a
    message.
  - PENDING: Start, Save and Stop work from the dialog; the status line follows.
  - PASS (agent, 2026-11-13): WP5 load smoke — main (code 1b604e0, HEAD
    878dc14) built and installed per 7.1, loaded in the rig OBS with no
    `RR_SELFTEST` in a fresh session (PID 23856, log `2026-11-13 06-47-05.txt`):
    `[obs-replay-to-disk] plugin loaded successfully (version 0.1.0)` and
    `[obs-replay-to-disk] avformat 62.12.102`, no plugin error lines (the log's
    only failures are the stock host ones: aja/decklink/nvenc); WM_CLOSE →
    `plugin unloaded`, `Number of memory leaks: 1` — a stock-OBS control run
    (plugin DLL moved aside, same config, `2026-11-13 06-47-44.txt`) logs the
    same `1`, so the plugin adds 0 leaks; the WP4-G runs show the same pair.
    Rig fix on the way: the rig data folder held a stale root-level `en-US.ini`
    (the 06:45 probe logged `Failed to load 'en-US' text for module:
    'obs-replay-to-disk.dll'`); redeployed `data/` as
    `data/obs-plugins/obs-replay-to-disk/locale/en-US.ini` and the line is gone
    — locale strings (dialog labels, Tools item) will now render.
  - Re-verify recipe (for the owner's manual run): (1) build, install per handoff
    7.1: DLL+PDB -> `.testrig/obs/obs-plugins/64bit/`, `data/` ->
    `.testrig/obs/data/obs-plugins/obs-replay-to-disk/`. (2) Launch with no
    `RR_SELFTEST` in the environment (fresh pwsh; the `.testrig/scripts`
    launchers set it - use them only with the env removed, or launch directly:
    `.testrig/obs/bin/64bit/obs64.exe --portable --disable-updater
    --disable-missing-files-check --multi --minimize-to-tray`; strictly one
    instance per portable config). (3) Settings -> Hotkeys shows "Replay to
    Disk: Save" and "Replay to Disk: Start/Stop Buffer" - bind both, exercise,
    close via `close-obs.ps1` (saved PID), relaunch, exercise again (binding
    persistence = the B6 reload at FINISHED_LOADING). (4) Tools -> "Replay to
    Disk...": change values incl. autostart, Apply, restart OBS, reopen -
    values persisted (settings.json through the now-wired `rr_settings_save`,
    first in-app caller). (5) Buffer folder `C:\` and = output folder ->
    warning box on Apply, settings not saved. (6) Start/Save/Stop from the
    dialog and from the "replay_to_disk_dock" dock (Docks menu; starts
    hidden); status line follows the engine within ~0.5 s; a settings change
    while buffering offers a restart (Yes -> stop, wait_off 6 s, start with
    the new snapshot).
- Verified locally 2026-11-13 (repo root; VS Build Tools 2026 cmake/ninja on
  PATH, formatters from `%APPDATA%\Python\Python314\Scripts`):
  - `cmake --preset windows-x64` -> exit 0.
  - `cmake --build --preset windows-x64 --config RelWithDebInfo` -> exit 0,
    0 warning lines; AUTOMOC processed the two Q_OBJECT headers and the Qt C++
    TUs (settings-dialog.cpp, status-dock.cpp, mocs_compilation) linked into
    `obs-replay-to-disk.dll`.
  - CI posture: `cmake --preset windows-x64 -B build_check -DENABLE_TESTS=ON
    -DCMAKE_COMPILE_WARNING_AS_ERROR=ON` then `cmake --build build_check
    --config RelWithDebInfo` -> exit 0, 0 warnings (C and Qt C++ at /W3 /WX).
  - Stricter one-off `/W4 /WX` for the C TUs (`-DCMAKE_C_FLAGS='/W4 /WX'`,
    build_w4: plugin-main, engine, settings, platform-win, core, tests) ->
    exit 0, 0 warnings. Qt C++ TUs in a separate `/W4` (no /WX) run
    (build_w4cpp): 0 warning lines total - our C++ code is /W4-clean and no
    Qt6 header noise appeared either.
  - `ctest --test-dir build_x64 -C RelWithDebInfo` -> "100% tests passed, 0
    tests failed out of 1" (exit 0).
  - clang-format 19.1.1 `--dry-run -Werror` over all new/changed C/C++
    sources -> clean (violations fixed with `-i`, then re-gated).
  - gersemi 0.29.2 `--check CMakeLists.txt` -> exit 0.
- Deviations from the handoff:
  - Names per the owner decision (KB plugin-name): hotkeys
    `ReplayToDisk.Save`/`ReplayToDisk.Toggle` (locale `Hotkey.Save`/
    `Hotkey.Toggle`), Tools item "Replay to Disk...", dialog/dock title
    "Replay to Disk", dock id `replay_to_disk_dock` (handoff
    `RollingReplay.*`/"Rolling Replay..." superseded).
  - New UI bridge `src/plugin/plugin-ui.h` (implemented in plugin-main.c, so
    `g_settings`, the start cache and the OBS-33 recording-path resolver live
    in one place): `rr_plugin_get/set_settings` (set = clamp + `rr_settings_save`
    + invalidate the resolved-folder cache), `rr_plugin_effective_dirs` (the
    one resolver behind the autostart/selftest paths and the dialog's greyed
    defaults + validation), `rr_plugin_start/stop/save/wait_off`. The dialog
    and dock never touch engine internals. Toggle hotkey: state OFF (an error
    ends in OFF with `last_error` set) -> `rr_plugin_start()` (fresh settings
    snapshot, folders resolved on the UI thread); otherwise stop.
  - Bug fixed while wiring the toggle: WP3's `start_from_settings` cached only
    the resolved folders into `g_start_cfg`, so a PROFILE_CHANGED autostart
    restarted from a zeroed config (clamp: 30 s window, 1000 kbps). It now
    caches the full resolved snapshot.
  - `extern "C"` guards added to `core/paths.h`, `core/space.h`,
    `core/settings-clamp.h` and `plugin/settings.h` so the C++ UI TUs link the
    C definitions (core stays pure C).
  - Dialog details beyond the handoff words: the minutes+seconds spins are one
    value (clamped through `rr_settings_clamp_window_sec`); the encoder combo
    = "Automatic" + h264/hevc/av1 video encoders passing the 5.8 availability
    rule, plus a kept "(not available)" entry for a configured encoder that
    currently fails it (Apply never silently rewrites an explicit choice);
    the inactive rate-control spin is disabled but still saved; unchecking the
    last audio track is refused (mask stays >= 1 bit); the restart prompt fires
    only when the state is not OFF and the settings actually changed.
  - Dock: one multi-line status label + Start/Stop + Save, same 500 ms QTimer;
    Save enabled only while BUFFERING (the engine ignores it otherwise anyway).
  - All strings in `data/locale/en-US.ini` via `obs_module_text` (~45 keys);
    `PluginName` already read "Replay to Disk" (unchanged).
- Open problems:
  - The four acceptance lines are PENDING for a manual run at the OBS window
    (the host wedge is gone — see the WP4 RESOLVED note and the load smoke);
    the recipe above is the full re-verify.
  - `rr_settings_save` now has an in-app caller, but its write path runs for
    the first time inside the pending manual test (survive-restart box).
  - `rolling_replay_saved` still has no in-app subscriber (the UI refreshes
    via the QTimer); the signal stays for scripts.


## WP6 hardening, docs, tag: DONE — except the owner manual-test checklist
- Date: 2026-11-13
- Commits: 91f35d3 (docs/manual-tests.md) cedfdd3 (README) (+ this status
  commit; the local annotated tag `v0.1.0` lands on this commit, not pushed).
- Acceptance (WP6 step 2: re-run of the WP3 and WP4 acceptance on the final
  HEAD 6aa7199; rig, encoder auto-resolved `obs_qsv11_v2`, rig fps 30/1; the
  used rig config means `Number of memory leaks: 1` is the stock-OBS baseline,
  plugin delta 0):
  - PASS: build gates on HEAD. `cmake --preset windows-x64` exit 0;
    `cmake --build --preset windows-x64 --config RelWithDebInfo` exit 0 with
    0 warning lines; `ctest --test-dir build_x64 -C RelWithDebInfo` →
    `100% tests passed, 0 tests failed out of 1` exit 0;
    `cmake --install build_x64 --prefix release/RelWithDebInfo` exit 0; DLL +
    PDB + `data/locale/en-US.ini` deployed to the rig per 7.1 (last code
    commit 1b604e0, so the binary was current for HEAD).
  - PASS: Run A, WP3 buffer suite (`RR_SELFTEST=buffer WINDOW=30 RUN=120`,
    run `wp6-A`, log `2026-11-13 06-55-57.txt`). 06:56:01.010 `buffer
    started: encoder=obs_qsv11_v2 fps=30/1 tracks=1 window=30s`; 11 `segment
    closed` lines, first `dur=11.46s` then 10.00 s intervals within 3 ms
    (06:56:12.494 → 06:57:52.498); first `pruned:` 06:57:02.496 = +61.5 s
    after start (D10: window 30 s + 15 s margin + next boundary; 6 prunes);
    `keep-me.txt` pre-placed in the buffer dir survived the run and the stop
    purge (the only file left there); `engine: purged 6 segment file(s) on
    stop` then `buffer stopped`, 0 `rrseg_*.mkv` after; every watchdog tick
    `lagged=0`; `selftest finished: ok buffer ran 120s`; WM_CLOSE →
    `OBS_EXITED_CLEANLY` in <1 s; `plugin unloaded`; `Number of memory
    leaks: 1` = stock baseline. One cosmetic
    `duplicate name "rr_selftest_media 2"` warning, the known WP3/WP4
    global-source carry-over.
  - PASS: Run B, WP4 save suite (`RR_SELFTEST=save WINDOW=30 SAVES=20,60
    SCOPE=window REFERENCE=1 RUN=100`, run `wp6-B`, log
    `2026-11-13 06-58-19.txt`). `save started: segments=2 inpoint=0.00s`
    06:58:43.030 → `save finished: ...Replay_2026-11-13_06-58-43.mp4
    frames=600 duration=20.00s took=0.04s warnings=0`; `save started:
    segments=4 inpoint=10.00s` 06:59:23.025 → `...Replay_2026-11-13_06-59-23
    .mp4 frames=930 duration=31.00s took=0.19s warnings=0`; ffprobe durations
    20.032 s (band 19–21 s) and 31.000 s (band 30.0–31.1 s); `verify_remux.py
    <clip> reference.mkv` exit 0 for both (600/600 + 939/939 and 930/930 +
    1453/1453 `matched_in_order==packets`, spread ≤0.667 ms,
    `dts_monotonic=True`, `dts<=pts=True`, `alignments=1`, constant A/V
    offsets +21.333 ms and −11.333 ms); `ffmpeg -v warning -i <clip> -map 0
    -f null -` silent for both; 0 `*.rrpart` recursively after the run; 0
    `rrseg_*` after stop; clean WM_CLOSE exit, `plugin unloaded`, leaks 1.
    (`verify_remux.py` ran from the `.handoff` reference folder, deleted
    afterwards — see Deviations.)
- Handoff WP6 steps: step 1 (manual tests on a real machine with a real game)
  → the checklist is written as `docs/manual-tests.md` (91f35d3), owner runs
  it; step 2 done above, no failures to fix; step 3 README final (cedfdd3):
  install to `%ProgramData%\obs-studio\plugins\`, uninstall, all 14 settings
  of 5.8, side effects of 1.3 verbatim, disk sizing of section 10, the exact
  OBS-33.0.1 pin (B5/D17), known limits, the AI-assistance statement; step 4
  tag: local annotated `v0.1.0` only, no push, CI build or GitHub release —
  the owner publishes; step 5: nothing posted anywhere.
- Deviations from the handoff: no code changes in WP6 (the tree at the tag is
  the code of 1b604e0). The handoff's tag-CI-publish chain (step 4) is
  replaced by the owner decision: local commits and the local tag only. The
  gitignored `.handoff/` package was removed from the working tree after the
  last run that needed it (`verify_remux.py`); no tracked file references a
  `.handoff/` path (checked with `git grep` before deleting).
- Open problems:
  - The `docs/manual-tests.md` checklist is owner-pending, incl. the four WP5
    manual boxes and the NVENC/AMF matrix and HDR box, which need a machine
    with that hardware (this host is Intel-only; the QSV findings in
    `docs/wp1-findings.md` carry over).

## v0.1.0 READY (summary)
- WP0 repository and build: DONE — three build commands pass, plugin loads in
  rig OBS 33.0.1 with `avformat 62.12.102`, Windows CI green (WP0 box above).
- WP1 spike inside real OBS: DONE — U1–U4 measured on Intel QSV, remux
  verified clean, leak control matrix in `docs/wp1-findings.md` (WP1 box).
- WP2 pure-C core and tests: DONE — `rr_core` + `tests/test_core.c` (18
  tests), ctest passes, core clean at /W3 /WX and /W4 /WX (WP2 box).
- WP3 buffer engine: DONE — start/roll/prune/stop on the rig, purge rules,
  profile-switch autostart, ≤5 s closes, runs wp3-r1…r3 (WP3 box).
- WP4 save flow: DONE — duration bands, packet-exact saves vs the reference,
  2-track saves, part-file lifecycle, prune pause, close-during-save G runs
  (WP4 box, G PASS 2026-11-13).
- WP5 user interface and hotkeys: DONE — dialog, dock, hotkeys; agent load
  smoke PASS; the four manual acceptance boxes stay owner-PENDING (WP5 box;
  the how-to is in `docs/manual-tests.md`).
- WP6 hardening, docs, tag: DONE except owner manual tests — WP3+WP4
  acceptance re-run PASS on the final HEAD (WP6 box above); README and
  `docs/manual-tests.md` written; local tag `v0.1.0`, not pushed.
- Owner publish checklist: (1) push `main` and the tag `v0.1.0` to GitHub;
  (2) confirm the CI Windows job builds the release package; (3) publish the
  GitHub release with that package, stating it requires OBS 33.0.1; (4) run
  `docs/manual-tests.md` on a real machine with a real game and report the
  NVENC/AMF/HDR results there; fix what fails and re-tag if needed.
  Done for v0.1.0 (handoff WP6) = every acceptance box PASS.
