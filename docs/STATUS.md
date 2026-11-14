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
- Owner publish checklist: (1) push `main` and the tag `0.1.0` to GitHub — the
  tag must be bare semver: the CI release job in `.github/workflows/push.yaml`
  only creates a release for tags matching `X.Y.Z` (plus `-rc`/`-beta` forms)
  and silently builds nothing for a `v`-prefixed tag (that is why the tag was
  moved from `v0.1.0` to `0.1.0`); (2) on the tag push, CI builds the Windows
  package with config Release and opens a draft release "Replay to Disk 0.1.0"
  attaching `obs-replay-to-disk-0.1.0-windows-x64.zip` with SHA-256 checksums
  in the body — the draft stays invisible until published; (3) review the draft
  and publish it, stating it requires OBS 33.0.1; (4) run `docs/manual-tests.md`
  on a real machine with a real game and report the NVENC/AMF/HDR results there;
  fix what fails and re-tag if needed.
  Done for v0.1.0 (handoff WP6) = every acceptance box PASS.

## Post-checkup fixes on top of v0.1.0: 12 problems, all fixed — rig-verified 2026-11-13

- Base and commits: the fixes stack on `f8740ec` (the pre-checkup release
  commit); the annotated tag `v0.1.0` was moved to the final commit of this
  round, a docs-only follow-up, so the tag and `main` HEAD are the fixed build.
  One commit per problem, oldest first: `7f255b3` `8a3b395`
  `f4096e1` `fbb4f32` `94b5319` `195da3c` `2fc5973` `6312155` `cc63dc2`
  `b6332eb` `9fc11f9` `2fd806c`, then docs commits: `1388e3d` (this box, the
  README profile-switch and folder/filename notes, the new
  `docs/manual-tests.md` profile box) and a follow-up that replaces item 9's
  verification entry with the rig run `fix9r2` recorded there. Rig = the
  portable OBS 33.0.1 under `.testrig`, encoder
  `obs_qsv11_v2`, 30/1 fps. OBS logs are the files under
  `.testrig/obs/config/obs-studio/logs/`, run dirs under `.testrig/runs/`.
  The rig config was reset mid-loop, so only runs from 12:19 on have a
  surviving OBS log; an entry whose rig log is gone names what still proves it
  (a unit test, the standalone repro, a run dir) and says the log is gone.
- Gates re-run on the fixed tree (HEAD `2fd806c`, before this markdown-only
  commit): `cmake --preset windows-x64` exit 0;
  `cmake --build --preset windows-x64 --config RelWithDebInfo` exit 0 with 0
  `: warning ` lines; `ctest --test-dir build_x64 -C RelWithDebInfo` →
  `100% tests passed, 0 tests failed out of 2` (`core`, `rename`);
  `cmake --build build_check --config RelWithDebInfo` (cache
  `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, C flags `/W4 /WX`) exit 0, 0 warnings;
  `cmake --build build_w4 --config RelWithDebInfo` (C flags `/W4 /WX`) exit 0;
  clang-format 19.1.1 `--dry-run -Werror` over the 25 files under `src/` exit 0;
  gersemi 0.29.2 `--check CMakeLists.txt` exit 0.
- Rig regression on the fixed build, WP3 buffer suite (run `chk-wp3`, log
  `2026-11-13 12-41-23.txt`, `RR_SELFTEST=buffer WINDOW=30 RUN=120`):
  `buffer started` 12:41:28.167, then 11 `segment closed` lines at 10.00 s
  intervals within 3 ms after the first (`dur=11.47s`); first `pruned:`
  12:42:29.676 = +61.5 s after start (6 prunes); `purged 6 segment file(s) on
  stop` and `buffer stopped` 12:43:28.640; every watchdog tick `lagged=0`;
  `selftest finished: ok buffer ran 120s`; `plugin unloaded`; the pre-placed
  `keep-me.txt` is still in the buffer dir and 0 `rrseg_*.mkv` are left.
- Rig regression, WP4 save suite (run `chk-wp4`, log `2026-11-13 12-44-09.txt`,
  `RR_SELFTEST=save WINDOW=30 SAVES=20,60`): `save started: segments=2
  inpoint=0.00s` 12:44:35.345 → `save finished: ...Replay_2026-11-13_12-44-35
  .mp4 frames=600 duration=20.00s took=0.08s warnings=0`; `save started:
  segments=3 inpoint=0.02s` 12:45:15.366 → `...12-45-15.mp4 frames=900
  duration=30.00s took=0.11s warnings=0`. Both clips re-measured after the last
  fix with the repo's own ffmpeg build: ffprobe format duration 20.032000 s
  (band 19–21 s) and 30.000000 s (band 30.0–31.1 s); `ffmpeg -v warning -i
  <clip> -map 0 -f null -` exit 0 with no output lines for both. Across all
  `chk-*` run dirs: 0 `rrseg_*.mkv`, 0 `*.rrpart`. The packet-exact
  `verify_remux.py` check could not be repeated: that script and its reference
  were deleted after WP6 (see WP6 Deviations), so this box records the duration
  and stream-clean measurement only.
- 1. `save_drain` livelock (major). A `CMD_STOP` dequeued while waiting for the
  split boundary went back into the same queue, so `q_count` never hit 0, the
  3 s "no boundary" timeout (on the empty-queue side) never fired, and the
  worker spun at 100 % CPU in SAVING; with a stalled muxer the user's Stop never
  ran. Fix `7f255b3`: STOP/FORCE_STOP are parked in a local array outside the
  queue (`RR_SAVE_DEFER_CAP`) and handed back after the drain ends
  (`src/plugin/engine.c:1012-1018`, `1046-1049`), so the queue drains empty, the
  loop sleeps on the queue event and the timeout branch is reachable again.
  Verified by re-running the structural repro (the queue plus a standalone copy
  of `save_drain`): the pre-fix build reports `[watchdog] t=5610ms
  spin_iters=53704154` then `LIVELOCK: save_drain did not return within 6000 ms`
  (exit 2); the fixed build logs `engine: save: no boundary within 3 s,
  continuing with closed segments`, returns at 3000 ms with `spin_iters=0` and
  the STOP re-queued (`RESULT: ok (timed out at the 3 s deadline, STOP re-queued
  for when the save ends)`, exit 0); with a boundary at +1.2 s it returns at
  1218 ms, `spin_iters=0`. The rig save-then-stop scenario, run `chk-stop2`
  (`2026-11-13 12-51-37.txt`, WINDOW=600): `save started: segments=30`
  12:56:41.300 → `save finished ... frames=8940 duration=298.00s took=1.02s
  warnings=0` 12:56:42.319 → `purged 31 segment file(s) on stop` and
  `buffer stopped` 12:56:43.354 → `final state=0`: the run's Stop was honored as
  soon as the save was over and the state left SAVING; the clip is 298.026667 s.
- 2. Unexpected OUTPUT_STOPPED cost 5–7 s (major). `handle_unexpected_stopped`
  ran the graceful-stop sequence on an output libobs had already marked inactive,
  and libobs emits no second `stop` signal, so both waits always expired —
  STOPPING held for 5–7 s with two misleading warnings. Fix `8a3b395`:
  `stop_session(force, stop_seen)` takes the already-arrived signal, and the
  unexpected-death path passes `stop_seen=true` (engine.c:916-918).
  Verified on the rig, run `chk-f2` (`2026-11-13 12-33-18.txt`, muxer child
  killed while buffering): `buffer error: the output stopped unexpectedly
  (code -8)` 12:33:36.042 → `purged 2 segment file(s) on stop` and
  `buffer stopped` 12:33:36.051 (9 ms later), and 0 occurrences of
  `graceful stop took over 5 s` or `the output never reported its stop signal`
  anywhere in that log.
- 3. Profile switch against a live save (major, B8/D16). PROFILE_CHANGING waited
  a fixed 6 s and let the switch through anyway; with the shipped 1800 s default
  a save outran it, OBS then failed the video reset (log
  `Tried to reset when already active`) and silently kept the old video mode.
  Fix `f4096e1`: `rr_engine_halt_for_video_reset` escalates within a bound —
  stop (6 s), cancel the running save (12 s more), force the stop (15 s more),
  33 s in total (engine.c:48-56, 1610-1657; plugin-main.c:402-414 calls it).
  Verified on the rig, run `chk-f3` (`2026-11-13 12-34-14.txt`, save-mode with
  WINDOW=600, save at t=400 s): `save started: segments=40 inpoint=0.00s`
  12:41:00.195 → `engine: video reset: cancelling the in-flight save`
  12:41:00.303 → `engine: save cancelled (a video reset asked for it)` and
  `save failed: cancelled` 12:41:00.318 → `purged 41 segment file(s) on stop`
  and `buffer stopped` 12:41:00.889; OBS then logged `video settings reset: ...
  output resolution: 852x480` (the new profile's value, the session had started
  at 1280x720) and `Switched to profile 'RigB'` at 12:41:00.919 / .928, and the
  log has 0 `Tried to reset when already active` lines. `.testrig/runs/chk-f3/out`
  holds 0 files: the cancelled save left no clip and no part file.
- 4. A segment was marked closed although the 2 s exclusive-open wait proved the
  muxer still held it (major): it entered save selection and prune, and a save
  that read only the valid prefix logged as clean (`warnings=0`) with the tail
  silently missing. Fix `fbb4f32`: on a failed probe the segment stays NOT closed
  (engine.c:877-891), window selection refuses to run past a segment that ended
  but is not closed (`src/core/window.c:21-32`, `window.h:5`), and the idle tick
  re-probes and closes it once the release really happens
  (`retry_segment_releases`, engine.c:1327). Verified by the unit test
  `window_skips_segment_still_being_written` (`tests/test_core.c:620`, passing
  in the ctest run above); the hold experiment run dirs `.testrig/runs/f4held`,
  `f4held2` survive, their OBS log does not.
- 5. `filename_format` could write the save outside the output folder (minor
  after red team): `os_generate_formatted_filename` keeps `/` and `\`, and the
  dialog's Filename field was never validated. Fix `94b5319`:
  `rr_paths_is_safe_filename` (core/paths.c:242) rejects `/`, `\`, `:`, `.` and
  `..`; Apply refuses such a format (`Error.FilenameFormat`), and the engine
  re-checks the generated name and falls back to the default format with a
  warning log (engine.c:1073-1083). Verified by the unit test
  `save_filename_must_be_bare` and on the rig, run `fx5-escape`: with the
  escaping format configured, the only finished file is inside the output folder
  under the default-format name (`.testrig/runs/fx5-escape/out/
  Replay_2026-11-13_11-42-20.mp4`), nothing above it; that run's OBS log is gone.
- 6. Worker not joined before the DLL was unmapped (minor). After the 10 s wait
  timed out, `rr_engine_shutdown` returned and OBS unmapped the DLL; the
  once-only guard also made the unload-time call return instantly, and a null
  `g_done_ev` skipped the wait entirely. Fix `195da3c`: the QUIT request stays
  once-only, but every call joins — `WaitForSingleObject(g_thread, INFINITE)`
  then `CloseHandle` (engine.c:1503-1530) — which is bounded by the QUIT path
  itself (force stop, per-segment cancel checks, bounded purge retries).
  Verified: every surviving rig run ends `buffer stopped` → `plugin unloaded`
  with no hang (12:19–12:56 logs); the timeout branch itself needs >10 s of
  stacked stalls at exit and was not reproduced.
- 7. The buffer==output rule was a raw string compare (minor): `C:\rec\..\rec`
  passed against `C:\rec`. Fix `2fc5973`: `paths_canonicalize`
  (`src/core/paths.c:80`) folds `.`, `..`, repeated separators and slash
  style before the equality, absolute and drive-root checks. The fold is
  purely textual — the code comment calls it "like GetFullPathNameW, no
  short-name or case resolution", but the Win32 API is never called. This
  entry originally read "GetFullPathNameW with a manual fallback", which
  never matched the code; corrected after the round-2 checkup. Verified by
  the unit test `buffer_dir_dot_segments_are_folded`
  (`tests/test_core.c:628`).
- 8. `output_dir` was never validated (minor): a relative value resolved against
  the OBS working directory (including the stale `*.rrpart` purge there), a drive
  root swept a whole volume. Fix `6312155`: `rr_paths_check_output_dir`
  (core/paths.c:195 — empty means the OBS recording path, otherwise absolute and
  not a drive root) applied at settings load (an unusable value falls back to the
  recording path with a warning, `src/plugin/settings.c:142-151`), at dialog
  Apply (`Error.OutputDriveRoot`, `Error.OutputRelative`) and at buffer start
  (engine.c:575-587). Verified by the unit test
  `output_dir_absolute_not_drive_root` (`tests/test_core.c:629`).
- 9. The save rename replaced an existing file (minor): `os_stat` then
  `os_rename`, and `os_rename` is `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`,
  so a same-named file created in the gap was lost instead of the save taking a
  `_2` suffix. Fix `cc63dc2`: new `src/plugin/rename.c` `rr_rename_exclusive`
  (no `REPLACE_EXISTING`, 1 = target exists, libobs-free so tests link it) and
  the save walks the `_N` suffixes on refusal, logging which name it used
  (engine.c:1106-1141). Two separate proofs, because the two halves live in
  different modules.
  - The exclusive primitive, ctest `rename` (`tests/test_rename.c`:
    `moves_a_file_when_the_target_is_free`,
    `refuses_an_existing_target_without_clobber`, `fails_on_a_missing_source`),
    passing in the ctest run above. This covers `rr_rename_exclusive` only —
    the suffix walk sits in the libobs-linked engine, so no unit test reaches
    it.
  - The engine walk, rig run `fix9r2` (`2026-11-13 13-22-11.txt`, save mode,
    WINDOW=300, one save at t=240 s). To make the collision deterministic the
    run harness polls the run's `out\` for the remux part file: the part name
    is `<chosen-name>.rrpart`, so the moment it exists the advisory
    `build_final_path` pick is fixed and the file the harness creates lands on
    the rename, not on that earlier name check. `buffer started` 13:22:16.795,
    `save started: segments=24` 13:26:17.315, part
    `Replay_2026-11-13_13-26-17.mp4.rrpart` created 13:26:17.347, harness
    wrote the pre-existing file under that exact name 20 ms later
    (13:26:17.367, 31 B, marker text `PREEXISTING-DO-NOT-TOUCH-fix9r2`), and
    the exclusive rename refused it — the collision line:
    `engine: save: ...out\Replay_2026-11-13_13-26-17.mp4 appeared while
    saving, wrote to ...out\Replay_2026-11-13_13-26-17_2.mp4 instead`
    13:26:18.032 (engine.c:1128-1140), then `save finished:
    ...Replay_2026-11-13_13-26-17_2.mp4 frames=7200 duration=240.00s
    took=0.72s warnings=0` at the same stamp. After the run `Get-ChildItem`
    on `out\` shows both files: `Replay_2026-11-13_13-26-17.mp4` 31 B and
    `Replay_2026-11-13_13-26-17_2.mp4` 901,294,625 B. The 31 B file is still
    the marker, never touched: SHA256
    `DCF1DC3A924CE77B053A70F6AC0F76F30E590A2FA34FF8BBA61874B7EF9E9FA0` when
    it was written and the same value after the run (`Get-FileHash`, equal to
    a reference hash of the marker text). The save is a valid clip: ffprobe
    format duration 240.021333 s (band 232–246 s) and `ffmpeg -v warning -i
    <save> -map 0 -f null -` exit 0 with no output lines; 0 `*.rrpart` under
    the run dir. `selftest finished: ok buffer ran 250s`, `plugin unloaded`,
    `Number of memory leaks: 1` = the stock baseline on the used config.
- 10. A configured output folder that did not exist refused the start with a
  misleading `not enough free disk ... output drive 0 MB` (minor): the preflight
  measures free space there and a missing folder measures 0. Fix `b6332eb`:
  `os_mkdirs(cfg.output_dir)` before the preflight, with its own failure message
  (engine.c:592-599). Verified: the root cause re-measured with the same Win32
  call (`GetDiskFreeSpaceExW` on a missing path → ok 0, 0 bytes; on an existing
  path → ok 1, 59470663680 bytes), so the preflight can no longer see a missing
  folder. The rig run that typed a nonexistent Output folder (12:14) left no
  surviving OBS log.
- 11. Engine entry points callable during shutdown (minor): OBS 33 has no
  `proc_handler_remove`, so the global `rolling_replay_save()` proc stays
  callable until the instant before the DLL is unmapped, and the worker and its
  queues are already gone by then. Fix `9fc11f9`: `g_shutting_down` is set
  before anything else in `rr_engine_shutdown` (engine.c:1485-1498) and
  start/stop/save/wait_off/get_status return at once, logging
  `START ignored (shutting down)` / `SAVE ignored (shutting down)` where a caller
  asked for something (engine.c:1533-1540, 1562-1569, 1601-1608, 1659-1666);
  `enqueue_cmd` itself stays open for the shutdown's own QUIT and the critical
  sections are never deleted. Verified: smoke run `fix11-smoke`
  (`2026-11-13 12-19-58.txt`) unaffected (buffer 30 s, `selftest finished: ok`,
  `plugin unloaded`), and the QUIT path still runs through the guard — run
  `chk-wp4`'s first attempt (`2026-11-13 12-43-52.txt`) logs
  `engine: quitting with the output active, forcing stop` 12:44:04.571 →
  `purged 1 segment file(s) on stop` → `buffer stopped` → `plugin unloaded`.
  No run had a late call through the proc, so that last instant stays
  open-in-principle, as the checkup filed it.
- 12. Video settings while the buffer runs (minor): OBS resets video from
  Settings > Video Apply, the Advanced tab, the AutoConfig wizard and a
  scene-collection remigrate without emitting a frontend event, so the plugin
  cannot step aside and OBS gives up quietly. Fix `2fd806c` is mitigation and
  documentation, not a repair: the engine logs
  `engine: while the buffer runs OBS cannot change its Video settings
  (Settings > Video, AutoConfig, scene-collection remigrate): stop the buffer
  first` at buffer start and at save start (engine.c:667-668, 1225-1228), the
  README states the limit under Known limits, and `docs/manual-tests.md` has a
  box for it. Verified on the rig, runs `fix12-buffer`
  (`2026-11-13 12-25-01.txt`, the line right after `buffer started`
  12:25:07.384) and `fix12-save` (`2026-11-13 12-25-47.txt`, after
  `buffer started` 12:25:53.079 and again after `save started` 12:26:13.577).
- Docs in this last commit, for the behaviour that changed:
  - README Known limits: the profile-switch sentence was wrong. It claimed the
    plugin "stops the buffer before the switch and restarts it after", but the
    restart only happens with Autostart on, which is off by default
    (`src/plugin-main.c:416-421`, `src/plugin/settings.c:28`; README settings
    table already said so). It now says the switch is bounded at 33 s and states
    the new user-visible behaviour: a profile switch cancels a save that is
    running, so no MP4 and no part file appear.
  - README settings table: the Output folder row now records that the folder is
    created at buffer start and may not be a drive root (fixes 8, 10); the
    Filename format row that `/`, `\` and `:` are not accepted, and that a
    hand-edited format producing them falls back to the default (fix 5).
  - `docs/manual-tests.md`: the "cannot change Video settings" box no longer
    claims a profile switch "stops and restarts the buffer"; a new box walks a
    profile switch while buffering and while saving, with the log lines to look
    for. The header now says the checklist is for the post-fix build.
- Left open by design: the OBS-side video reset with no frontend event (item 12)
  is not fixable from the plugin; `docs/manual-tests.md` and the README carry it
  as a limitation. `verify_remux.py` is not in the repo, so the packet-exact save
  check is not repeatable here (see WP6 Deviations).

## Post-checkup fixes round 2: 15 problems, all fixed — rig-verified 2026-11-13

- Base and commits: the round-2 checkup ran on `main` at `3a25f52`, and the
  annotated tag `v0.1.0` stays on that commit — this round does not move it (the
  parent retags after convergence). One commit per fix, oldest first:
  `ffb4700` (1) `2d6111b` (2) `1e66a4c` (3) `dd1b9eb` (6) `507de8e` (7)
  `58b766d` (10) `cca62e1` (12) `c1a5b8c` (14) `69b7a7e` (15), then
  `433a7e0` (4+5, the one folder-rule change), `58ebf7c` (8), `d4a4b03` (9),
  `7cef324` (11) and `deed83c` (13). Issues 4 and 5 share `433a7e0`: they are
  one change to the folder rules (trailing-dot/space aliases, device-namespace
  paths and UNC share roots). These last five first landed as a single lumped
  commit `62f0d93`, which the follow-up review flagged against the
  one-commit-per-fix rule; the history was rebuilt to split it per fix (the
  final tree is identical, and each split commit builds green with ctest 2/2
  and clang-format clean on its own). Rig = portable OBS 33.0.1 under
  `.testrig`, encoder `obs_qsv11_v2`, 30/1 fps; OBS logs under
  `.testrig/obs/config/obs-studio/logs/`, run dirs under `.testrig/runs/`.
- Gates re-run on the fixed tree (working tree = `deed83c` plus these docs):
  `cmake --preset windows-x64 -DENABLE_TESTS=ON` exit 0;
  `cmake --build --preset windows-x64 --config RelWithDebInfo` exit 0 with 0
  `: warning ` lines; `ctest --test-dir build_x64 -C RelWithDebInfo` →
  `100% tests passed, 0 tests failed out of 2` (`core`, `rename`); the two strict
  caches rebuild exit 0 with 0 warning lines (`build_check`,
  `CMAKE_COMPILE_WARNING_AS_ERROR=ON`; `build_w4`, C flags `/W4 /WX`);
  clang-format 19.1.1 `--dry-run -Werror` over the changed `src/**` and
  `tests/test_core.c` exit 0; gersemi 0.29.2 `--check CMakeLists.txt` exit 0.
- Rig regression, WP3 buffer suite (run `r2chk-wp3`, log `2026-11-13
  17-59-09.txt`, `RR_SELFTEST=buffer WINDOW=30 RUN=120`): `buffer started`
  17:59:15.231; `segment closed` at 10.00 s intervals (`dur=11.46s` first, then
  `10.02, 10.00, 9.98, 10.01, 10.00`); first `pruned:` 18:00:16.726 = +61.5 s
  after start; `engine: purged 6 segment file(s) on stop` then `selftest
  finished: ok buffer ran 120s` 18:01:15.734; 0 `rrseg_*.mkv` left and
  `keep-me.txt` survives in the buffer dir; `Number of memory leaks: 1` = the
  stock baseline on the used config.
- Rig regression, WP4 save suite (run `r2chk-wp4`, log `2026-11-13
  18-01-28.txt`, `RR_SELFTEST=save WINDOW=30 SAVES=20,60 SCOPE=window`): `save
  started: segments=2 inpoint=0.00s` 18:01:54.182 → `...18-01-54.mp4 frames=600
  duration=20.00s took=0.11s warnings=0`; `save started: segments=4
  inpoint=10.00s` 18:02:34.182 → `...18-02-34.mp4 frames=930 duration=31.00s
  took=0.12s warnings=0`; ffprobe format durations 20.032000 s (band 19–21 s)
  and 31.000000 s (band 30.0–31.1 s); `purged 6 segment file(s) on stop`,
  `selftest finished: ok buffer ran 100s`, `Number of memory leaks: 1`.

- 1. `since_last_save` advanced past footage it never wrote (major). Step 8 of
  `handle_save` set `s.last_save_end_ns = window_end` on success even when the
  selection had been cut short, so footage the clip never contained was recorded
  as saved: the next save started at `window_end` and prune deleted the rest, all
  under a clean `save finished: ... warnings=0`. Fix `ffb4700`: new
  `rr_window_saved_end` (`src/core/window.c:62`) returns the end of the last
  segment actually written — its predecessor when `rr_remux` reported
  `skipped_tail` — or 0 (do not move) when a hole cut the run off with unsaved
  in-window footage below it; `handle_save` advances the boundary only to that end
  and only forward (`src/plugin/engine.c:1354-1357`). Unit-tested by the new
  `rr_window_saved_end` asserts in `tests/test_core.c`. Rig, run `r2chk-hold3`
  (`2026-11-13 17-56-29.txt`, `SCOPE=since_last_save SAVES=2`, a tool holding
  each just-rotated `rrseg_*.mkv` with share 0 across the boundary probe): save 1
  `save started: segments=2 inpoint=0.00s` → `save finished:
  ...Replay_...17-56-56.mp4 frames=330 duration=11.00s warnings=0`; after the
  hold released, save 2 `save started: segments=3 inpoint=0.00s` → `...
  Replay_...17-57-16.mp4 frames=570 duration=19.00s warnings=0`. The two clips
  fill the window back to back — ffprobe 11.029333 s + 19.008000 s = 30.04 s, so
  nothing was skipped — and `engine: segment not released within 2 s:
  ...rrseg_...17-57-15.mkv` shows a real hold landed inside the run.
- 2. Stop purge was not aggregate-bounded (minor). The stop/QUIT purge retried
  each locked segment for ~2 s and so cost ~2 s per locked file; a buffer folder
  of externally held files (backup/sync tools, media preview panes) froze OBS's
  exit for minutes at the 1800 s default, and shutdown then joins on the UI
  thread behind it. Fix `2d6111b`: `purge_buffer_dir` shares one
  `RR_PURGE_TOTAL_MS` (5 s) deadline across the whole pass
  (`src/plugin/engine.c:261`) — past it every remaining file gets one unlink and
  is left behind with a warning, re-purged on the next start — and `stop_session`
  reports the skipped count. No unit test (worker-thread path); proven on the
  rig. Before/after with every just-closed segment held open: run `r2f2-pre3`
  (`hold-trace.txt` 16 files `HELD`) closed via WM_CLOSE in `OBS_EXITED after
  32.4 s` (`close-measure.txt`); run `r2f2-post` (fixed build, `hold-trace.txt`
  15 files held) closed in `OBS_EXITED after 5.5 s` — the 5 s budget bounds the
  stop. Those two OBS logs are gone; the per-run `close-measure.txt` and
  `hold-trace.txt` are what survive.
  - Corrects the round-1 item 6 record (`195da3c`): it called the QUIT path
    "bounded by the QUIT path itself ... purge gives up on locked files after ~2 s
    each", which was true only per file. With `2d6111b` the purge is bounded in
    aggregate (5 s total), so the join behind it is bounded too.
- 3. Dead `stop_requested` field (minor). `s.stop_requested` was written in
  `stop_session` and cleared, but read nowhere; the stop/wait logic keys off
  `s.stop_seen` alone. Fix `1e66a4c` deletes the field and both writes. Verified:
  `git grep stop_requested` over `src/` is empty after the commit, and the full
  build plus both strict caches rebuild clean (nothing else depended on it).
- 4. Folder rules missed trailing-dot/space aliases (minor).
  `paths_canonicalize` kept a name's trailing dots and spaces, so `D:\out.`,
  `D:\out ` and `D:\out ..` passed `rr_paths_check_buffer_dir` as different from
  the output folder `D:\out` even though Windows resolves all three to that
  folder — the buffer then rolled `rrseg_*.mkv` into the output folder where
  every stop purge deletes them. Fix `433a7e0`: the canonicalizer now strips each
  name's trailing dots and spaces, keeping `.` and `..` meaning (stripping to
  empty keeps the directory's level). Unit-tested in
  `buffer_dir_windows_alias_is_refused` (`D:\out.`, `D:\out `, `D:\out ..`,
  `D:\OUT. `, a mid-path `sub ..`, and stripping that exposes the drive root
  behind it), and on the rig: run `r2gap-alias-same` (`2026-11-13
  18-37-26.txt`) starts the buffer from a `settings.json` whose `buffer_dir`
  is the output folder spelled with a trailing dot (`<out>.` against `<out>`),
  and `start_session` refuses it — `buffer error: buffer folder is the same
  as the output folder` (Gap-close section below).
- 5. Folder rules missed device paths and UNC share roots (minor). Device
  spellings `\\?\C:\`, `\\.\C:\` and `\??\C:\` alias ordinary folders yet bypass
  the root checks, and `\\server\share` — a whole network volume — was accepted
  where a drive root is refused, so the `*.rrpart` purge acted at a share root.
  Fix `433a7e0`: `path_is_device_path` (`src/core/paths.c:84`) with a new
  `RR_PATH_DEVICE_PATH` returned by both checkers (`paths.c:230,244,262`), and
  `rr_paths_is_drive_root` (`paths.c:39`) now answers true for the UNC share root
  (`\\server\share` yes, `\\server` and `\\server\share\sub` no); the dialog and
  `start_session` map the new error to their own messages
  (`Error.BufferDevicePath`, `Error.OutputDevicePath`). Unit-tested in
  `buffer_dir_device_path_is_refused` and `unc_share_root_is_refused`, and on
  the rig with both spellings in `settings.json` (autostart, Gap-close section
  below): `\\?\C:\` as the output folder logs the load-time
  `settings: output folder '\\?\C:\' is not usable, falling back to the OBS
  recording path`, and `\\?\...` as the buffer folder is refused at start by
  `buffer error: buffer folder is a Windows device path (\\?\, \\.\ or \\??\)`
  (run `r2gap-device`, `2026-11-13 18-37-34.txt`); `\\server\share` in both
  keys gives `settings: output folder '\\server\share' is not usable, ...` and
  `buffer error: buffer folder is a drive root` (run `r2gap-unc`, `2026-11-13
  18-37-41.txt`) — the share root refused as a root, and no network is touched
  because the checks are textual and come before any disk work.
- 6. The round-1 fix-7 entry misdescribed the code (docs, minor). It read
  "GetFullPathNameW with a manual fallback", but `paths_canonicalize` is a purely
  textual fold and never calls the Win32 API. Corrected in place by `dd1b9eb`:
  the entry now names the textual fold and says what it does not do (trailing
  dots and spaces — which issue 4 then fixed).
- 7. Start purge deleted another instance's live save part (minor).
  `start_session` deleted every `*.rrpart` in the output folder, so a second OBS
  `--multi` starting while a first instance sat between remux-end and the
  exclusive rename deleted that save's finished part and lost its clip. Fix
  `507de8e`: a part is stale by last write, not by existence — one written within
  `RR_PART_STALE_SECS` (120 s) is kept and logged, only older leftovers deleted
  (`src/plugin/engine.c:337`). The honest cost is in the code comment: a crashed
  run's leftover is purged no earlier than one window after its last write, and
  only by an instance that restarts. Rig, run `r2f7` (`2026-11-13
  17-12-48.txt`, three `*.rrpart` stubs planted at 6 s / 106 s / older): the
  start logs `engine: part kept, written 6 s ago: ...Replay_fresh_...rrpart`,
  `engine: part kept, written 106 s ago: ...Replay_mid_...rrpart`, and
  `engine: removed stale part: ...Replay_old_...rrpart`.
- 8. `filename_format` accepted Windows-illegal names (minor).
  `rr_paths_is_safe_filename` rejected only `/`, `\` and `:`, so `Best Moment?`
  passed the dialog and settings load and every save then failed at the file open
  (`ERROR_INVALID_NAME`) with no config-time warning. Fix `58ebf7c`: the checker
  now rejects the whole illegal set `< > : " | ? *` plus `/` `\`, a trailing dot
  or space, and the reserved device bases `CON PRN AUX NUL COM1-9 LPT1-9` (but
  not `COM10`, `LPT0`, or those words as a substring) (`src/core/paths.c:320,376`);
  settings load falls back to the default with a warning when a hand-edited
  `settings.json` bypasses the dialog (`src/plugin/settings.c`, log `settings:
  filename_format '<name>' is not a usable Windows file name, falling back to the
  default`). Unit-tested by the new asserts in `save_filename_must_be_bare` (the
  report's `Best Moment?` spelling among them). The dialog Apply message and a
  `docs/manual-tests.md` box carry the user-facing path, and the settings-load
  fallback ran on the rig (Gap-close section below): run `r2gap-filename`
  (`2026-11-13 18-34-48.txt`) loads a hand-written `settings.json` with
  `filename_format: Best Moment?`, logs the documented
  `settings: filename_format 'Best Moment?' is not a usable Windows file name,
  falling back to the default` at plugin load, and the save lands
  `out\Replay_2026-11-13_18-35-13.mp4` (`warnings=0`) with no `Best Moment*`
  file in the run. A crafted name that skips settings load is clamped again by
  `start_session` (engine.c:576) before `build_final_path`'s backstop — run
  `r2gap-enginename` shows that clamp — so the `ERROR_INVALID_NAME`-at-open
  scenario stays a unit-test claim (`save_filename_must_be_bare`), not a
  behavior OBS can reach.
- 9. Two suffix systems chained `_2_2` (minor). `build_final_path`'s advisory
  name pick and `rename_part_exclusive`'s rename walk were independent, so when
  the advisory picked `<stem>_2` and that file also appeared mid-remux, the rename
  stripped `.mp4` from the suffixed name and wrote `<stem>_2_2.mp4`. Fix
  `d4a4b03`: one walk, `rr_paths_take_suffix` (`src/core/paths.c:403`), which both
  passes run over the same bare stem — `build_final_path` returns the stem and the
  first index it did not use (`src/plugin/engine.c:1169`), and
  `rename_part_exclusive` continues from there (`engine.c:1204`), so a taken `_2`
  lands on `_3`, never `_2_2`. Unit-tested by
  `save_suffix_walk_continues_the_sequence` (fresh walk starts at `_2` and skips
  taken names; the composition where the advisory used `_2` and the rename pass
  starts at 3 lands `_3`). The functions stay `static` in the libobs-linked
  engine, so the engine glue itself is not unit-reachable — the round-1 `fix9r2`
  run exercised the plain→`_2` rename leg; the `_2`→`_3` continuation through
  the engine glue ran on the rig in run `r2gap-suffix` (`2026-11-13
  18-33-12.txt`), where a pre-created `Replay_T.mp4` drove the advisory pick to
  `_2` and a mid-remux file forced the rename walk from `next_n = 3` (lines and
  hashes in the Gap-close section below).
- 10. A held newest segment blocked a whole closed run (minor). When the newest
  ENDED segment was the unclosed one (the usual just-rotated file a scanner
  holds), `rr_window_select` broke before collecting anything and the save logged
  `nothing to save` with a full window of closed segments below it. Fix
  `58b766d`: such a segment above the run is skipped, not a refusal — the closed
  run under it is hole-free footage on its own — and `handle_save` logs the
  skipped file by name so the save is not mistaken for covering it
  (`src/core/window.c:26-36`, `src/plugin/engine.c:1304`). Unit-tested (newest
  hole) in `tests/test_core.c`. Rig, run `r2chk-hold2` (`2026-11-13
  17-53-54.txt`): the save starts on the closed run and names both held files —
  `engine: save: ...rrseg_...17-54-20.mkv is not released yet, saving the older
  closed run` and the same for `...17-54-30.mkv`. In that run the save then fails
  `save failed: open segment: Permission denied` because the holder had the
  selected segment open too, a legitimately unreadable file; the clean two-save
  recovery is run `r2chk-hold3` under issue 1.
- 11. `path_is_drive_root("C:")` contradicted its comment (minor). The helper
  answered `C:` a root while its comment said drive-relative `C:` is not one.
  Unreachable through the public API (both checkers refuse `C:` earlier on the
  absolute-path gate) but a trap for a future caller. Fix `7cef324`:
  `rr_paths_is_drive_root` returns false for `C:` and `C:file` and is exposed for
  testing (`src/core/paths.c:39`); the contract is asserted directly in
  `drive_root_helper_matches_its_contract`, and the public refusal of `C:` is
  still asserted in `output_dir_absolute_not_drive_root`.
- 12. ctest never ran in CI (minor, improvement-class). Both ctest gates were
  wired in CMake but no workflow configured `ENABLE_TESTS=ON` or ran `ctest`, so
  a core or rename regression could merge on a green CI. Fix `cca62e1`: the
  Windows build job now reconfigures `windows-ci-x64 -DENABLE_TESTS=ON`, builds
  `test_core` and `test_rename`, puts the FFmpeg DLL dirs the tests link (read
  from the CMake cache) on `PATH`, and runs `ctest --output-on-failure`, failing
  the job on a non-zero exit; the macOS and Ubuntu jobs are unchanged. CI is not
  run in this environment — the job runs the same `ctest` that passes locally
  (100%, 2/2) and the diff was reviewed as valid YAML.
- 13. Dialog restart was silently lost when the stop was slow (minor). The
  Apply-restart path ran `rr_plugin_stop(); rr_plugin_wait_off(6000);
  rr_plugin_start();` and ignored `wait_off`'s result, so a buffer not OFF within
  6 s (a save in flight parks the STOP; a long stop outlasts the wait) had its
  START dropped by the engine and the user was left with a stopped buffer and no
  message. Fix `deed83c`: the START is enqueued only when `wait_off` reports OFF;
  otherwise the dialog logs `dialog: buffer restart skipped: the buffer did not
  stop within 6 s` and shows a box saying the buffer was not restarted (settings
  are still saved, press Start Buffer once the status reads Off)
  (`src/ui/settings-dialog.cpp`). There is no automated rig proof (the drop needs
  a stop slower than the wait, which a headless harness cannot time against the
  dialog); the code path and a `docs/manual-tests.md` box with the log line and
  the message box carry it.
- 14. Per-profile hotkeys leaked across profiles (minor). `load_hotkey` returned
  early when a profile's `basic.ini` had no binding for a hotkey, so
  `obs_hotkey_load` — the only libobs path that clears the previous profile's
  live bindings — never ran, and a hotkey bound on profile A kept firing after a
  switch to profile B that had none. Fix `c1a5b8c`: `obs_hotkey_load` is always
  called, with an empty array when the profile stores no bindings
  (`src/plugin-main.c`). Proven against vendored `libobs/obs-hotkey.c`
  (`remove_bindings` runs only inside `obs_hotkey_load`) plus a
  `docs/manual-tests.md` box (bind under A, switch to B, the key does nothing and
  the Hotkeys dialog for B shows no binding); run `hotkey-empty-load`
  (`2026-11-13 17-43-29.txt`) exercises the changed load path through a clean
  start/stop with no crash. The cross-profile behavior itself needs a person at
  the Hotkeys dialog to confirm.
- 15. Stale `reference/` pointers in the shipped sources (docs, minor).
  `remux.c`/`remux.h` pointed readers at `reference/README.md` and called remux.c
  a port of `reference/rr_remux.c`, but `reference/` was deleted with the WP1
  handoff. Fix `69b7a7e`: both headers now point at `docs/wp1-findings.md` and
  `docs/STATUS.md`; `git grep reference/` over `src/` returns nothing after.
- Honest about what is unit-only vs rig-proven. Rig-proven this round: 1, 2, 7,
  10, and the WP3/WP4 regression. When the round first closed, 4, 5, 8 and 9
  carried only unit tests plus code-path reading — three of the task's five
  targeted rig proofs ((b) suffix, (c) filename, (d) aliases) had not been run,
  so closing then was wrong; the Gap-close section below ran them all on the
  rig and those entries now cite those runs. Still unit-only or code-path-only:
  11, 12 (CI not run here), 13, and the code half of 14. The three user-facing
  paths that need a human — issue 8's dialog Apply, issue 13's restart
  message, and issue 14's cross-profile binding — are carried by
  `docs/manual-tests.md` boxes (filename validation, restart-skip message,
  per-profile hotkey) rather than automation. `verify_remux.py` is still not
  in the repo, so the packet-exact save check stays unrepeatable here (WP6
  Deviations).

## Gap close: the three missed round-2 rig proofs

The round-2 wave closed claiming nothing remained while three of the five
mandated targeted rig proofs were never run: (b) suffix chain, (c)
settings.json filename, (d) path-alias start refusals — issues 4, 5, 8 and 9
were labeled unit-test + code path only. The acceptance asked for all five,
quoted logs and listings included. The blocker for (b) was real: the selftest
always names saves with the default timestamp format, so a harness cannot
pre-create the name the save collides with — `fix9r2` could only force the
plain→`_2` leg once the part file was already visible. (c) and (d) had no
blocker; they had simply never been run.

- The hook (commit `24e6ca3`): a selftest-only env `RR_SELFTEST_FILENAME`
  overrides `cfg.filename_format` in `selftest_start` (`src/plugin-main.c`),
  logging `selftest: filename_format override: <fmt>`. No production settings
  path is touched. All gates re-ran clean with it in: RelWithDebInfo build 0
  warnings, `build_check` (CMAKE_COMPILE_WARNING_AS_ERROR=ON) exit 0 with 0
  warnings, `/W4 /WX` C and C++ builds exit 0, ctest 2/2, clang-format 19.1.1
  and gersemi 0.29.2 clean. The built DLL went to the rig and every run below
  uses it.
- (b) `r2gap-suffix` (`2026-11-13 18-33-12.txt`, save mode, WINDOW=45, one
  save at t=45 s, override `Replay_T`). The harness pre-creates
  `out\Replay_T.mp4` (37 B marker `PREEXISTING-DO-NOT-TOUCH-r2gap-suffix`,
  SHA256 C9A4BD25ECFAD0116B675F7E4166D36B4CD3D5CAA40A7E50390BC7FA5DF8E939,
  written before OBS started), so `build_final_path`'s advisory pass takes `_2`
  — the part `Replay_T_2.mp4.rrpart` is created 18:34:03.601 — and hands
  `next_n = 3` to the rename. The harness creates `Replay_T_2.mp4` (47 B,
  SHA256 00740E17E056DF9DA79EF3C4692003E2065EC42430564004FA721B132A64C2A6)
  mid-remux at 18:34:03.625, and the rename refuses it and continues the
  sequence: `engine: save: ...\out\Replay_T_2.mp4 appeared while saving,
  wrote to ...\out\Replay_T_3.mp4 instead`, then `save finished:
  ...\Replay_T_3.mp4 frames=1350 duration=45.00s took=0.18s warnings=0`
  (18:34:03.757). After the run `out\` holds `Replay_T.mp4` 37 B (hash
  unchanged), `Replay_T_2.mp4` 47 B (hash unchanged) and `Replay_T_3.mp4`
  165,947,192 B; ffprobe duration 45.034667 s; `ffmpeg -v warning` silent; 0
  `*.rrpart` and 0 `rrseg_*.mkv` in the run dir; `selftest finished: ok
  buffer ran 75s`; `Number of memory leaks: 1` (baseline). That is the
  stem/`next_n` wiring through the two static engine functions, end to end.
- (c) `r2gap-filename` (`2026-11-13 18-34-48.txt`, save mode, no override):
  the module config dir gets a `settings.json` holding `filename_format:
  Best Moment?` and nothing else. Plugin load logs the documented
  `settings: filename_format 'Best Moment?' is not a usable Windows file name,
  falling back to the default` (18:34:50.649), and the t=20 s save finishes
  `out\Replay_2026-11-13_18-35-13.mp4` (71,671,876 B, `warnings=0`); no
  `Best Moment*` file exists in the run. settings.json was restored (absent)
  afterwards. Honest scope: in save mode the selftest's own default cfg names
  the clips, so what the run proves is the load-time checker and documented
  fallback on the exact spelling, and that the default it falls back to names
  saves as `Replay_*.mp4` — the dialog→`g_settings` hop stays the manual box.
- The engine-side backstop of issue 8 never got its turn, and the run says
  why: `r2gap-enginename` (`2026-11-13 18-35-41.txt`) feeds `Best Moment?`
  straight to the engine through the new override, bypassing settings load,
  and the `settings:` clamp line appears again — `start_session` clamps the
  incoming config (engine.c:576) before `handle_save`'s
  `build_final_path` check can see the name — and the save lands
  `Replay_2026-11-13_18-36-06.mp4`. With two clamps upstream of it, the
  `filename_format produced the unsafe name` line is unreachable by any
  start path (defensive depth), and no `ERROR_INVALID_NAME` file-open can
  happen from a configured format.
- (d) three autostart runs (each installs a `settings.json`, waits for the
  refusal, closes OBS by WM_CLOSE, restores settings.json to absent after;
  each ends at the leak baseline 1):
  - `r2gap-alias-same` (`2026-11-13 18-37-26.txt`): `buffer_dir`
    `...\runs\r2gap-alias-same\out.` against `output_dir` the same path
    without the dot → `buffer error: buffer folder is the same as the output
    folder` (18:37:29.647).
  - `r2gap-device` (`2026-11-13 18-37-34.txt`): `output_dir` `\\?\C:\` →
    load refuses `settings: output folder '\\?\C:\' is not usable, falling
    back to the OBS recording path` (18:37:36.637); `buffer_dir` spelled
    `\\?\C:\...` → `buffer error: buffer folder is a Windows device path
    (\\?\, \\.\ or \??\)` (18:37:36.852).
  - `r2gap-unc` (`2026-11-13 18-37-41.txt`): `\\server\share` as output →
    `settings: output folder '\\server\share' is not usable, falling back to
    the OBS recording path` (18:37:43.784); the same as buffer →
    `buffer error: buffer folder is a drive root` (18:37:44.015), the share
    root refused where a drive root is refused. No network was touched: the
    checks are textual and precede any disk access.
- WP3/WP4 regression on this final build. `r2gap-wp3` (`2026-11-13
  18-37-48.txt`): buffer started, 11 segments closed at ~10 s cadence, first
  prune at +61.5 s, `keep-me.txt` survived in `buffer\`, 0 `rrseg_*` after
  close, leaks 1; the analyzer's one red line, `No crash sentinel location
  set`, is present in every rig log including the round-2 originals.
  `r2gap-wp4` (`2026-11-13 18-40-00.txt`): the t=20 and t=60 saves land
  ffprobe durations 20.032000 s and 30.000000 s (both bands), `warnings=0`
  on both, `ffmpeg -v warning` silent on both, 0 `*.rrpart`.

That closes the proof list: (a) and (e) were run in the round, (b), (c) and
(d) above. What remains unit-only or manual is exactly the honesty bullet.

## Widening OBS compatibility: FFmpeg bundled with the plugin (2026-11-14)

Owner ask: support a wider OBS range than exactly 33.0.1. Research split the
pin into two independent gates:

- The libobs API gate. `OBS_DECLARE_MODULE()` inside libobs's own
  `obs-module.h` defines `obs_module_ver()` returning `LIBOBS_API_VER` (33.0
  for our build), and libobs refuses any plugin reporting a version newer
  than itself. Older-built plugins are accepted, so this gate is
  forward-open: a 33-built plugin can load on future OBS; OBS ≤ 32 can never
  load it. Nothing to change here — this is also what the OBS plugin
  ecosystem documents.
- The FFmpeg soname gate. Our import table names `avformat-62.dll`,
  `avcodec-62.dll`, `avutil-60.dll` (dumpbin on the built DLL, matching the
  FFmpeg 8 pair OBS 33 ships). Per [ffmpeg.org's version tables](https://ffmpeg.org/download.html),
  OBS 30/31 ship 6.0/6.1 (all 60/60/58), OBS 32 ships 7.1 (61/61/59), OBS 33
  ships 8.x (62/62/60). A future OBS with a different major is what breaks
  the import, and that is the gate worth removing.

Owner chose option B: bundle FFmpeg inside the plugin package (one zip works
on 33 and any later OBS, forward-only; a static-FFmpeg build — option C —
was left open as future slimming). Two mechanics decided the design, both
measured, not assumed:

- OBS maps plugins with `os_dlopen`'s `LoadLibraryExW(path, NULL,
  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, so
  the plugin's own folder is searched for its direct imports; and a module
  OBS already loaded by the same basename wins over ours.
- Those flags do NOT extend to the bundled DLLs' own child imports. A
  scratch harness (console exe, same flags, plugin-like DLL importing
  avformat-62, no FFmpeg anywhere else on the machine) reproduced it: first
  level resolves from the plugin folder, `avcodec-62` then fails with err
  126. Preloading the closure dependency-first fixes it. The plugin now
  delay-loads its three FFmpeg imports (`<DelayLoadDLLs>avformat-62.dll;
  avcodec-62.dll;avutil-60.dll</DelayLoadDLLs>` in the vcxproj, derived from
  the `FFmpeg_*_LIBRARY` cache vars) and `rr_ffmpeg_preload()`
  (`src/plugin/ffmpeg-load.c`, first statement of `obs_module_load`) loads
  the full closure from the plugin's own directory, topological order,
  reusing any member OBS already loaded (`GetModuleHandleW` first). A
  preload failure logs `FFmpeg preload failed: <dll> (err N)` and refuses
  the load, so a broken install is loud in the OBS log.
- The closure is 8 files (dumpbin `/DEPENDENTS` transitive walk over
  obs-deps 2026-10-23): avutil-60, zlib, swresample-6, libx264-164, srt,
  librist, avcodec-62, avformat-62 — the same binaries OBS 33 ships,
  ≈ 42.7 MB uncompressed, installed next to the plugin DLL by the bundle
  list in `CMakeLists.txt`; the list and `ffmpeg-load.c` must stay in sync
  and the configure fails loudly if a member is missing.

Evidence:

- ctest `ffmpeg_load` (new, 3/3 green): a process that never links FFmpeg
  preloads all 8 members from the dependency folder, every closure member
  is a loaded module afterwards in order, and `avformat_version` resolves
  from the loaded module (62.12.102).
- Rig run `ff-dl-t3` (`2026-11-14 03-21-49.txt`, save mode, the delay-load
  build + bundle deployed to the rig's plugin dir): `plugin loaded
  successfully (version 0.1.0)`, `avformat 62.12.102` straight after
  (first delay-thunk call through the preload), both saves `warnings=0`
  at ffprobe durations 20.032000 s and 31.000000 s (bands), `ffmpeg -v
  warning` silent on both, 0 `rrseg_*`/`*.rrpart` left, `selftest finished:
  ok`, leaks 1 (baseline). OBS skipped the bundled copies in its module
  scan (`Skipping module ... not an OBS plugin` — three such lines in
  earlier runs) and coexisted with its own FFmpeg.
- Failed experiment, kept for the record: trying to hide OBS's own FFmpeg
  (rename `bin\64bit\{avformat-62,avcodec-62,avutil-60}.dll`) hung OBS at
  startup behind a module-failure modal (process up, no window; own PID
  killed, names restored). The "future OBS" state cannot be simulated by
  breaking 33 — OBS's own plugins import the same sonames — so the future
  case rests on the loader mechanics above, proven outside OBS, plus OBS's
  documented load rule. An OBS ≤ 32 rig would not help either: its libobs
  version gate refuses this build by design.

Gates on the final tree: windows-x64 RelWithDebInfo 0 warnings, `/W4 /WX`
exit 0, Release build 0 warnings with ctest 3/3, clang-format 19.1.1 and
gersemi 0.29.2 clean, CI test-target line includes `test_ffmpeg_load`.
Tag `0.1.0` was re-pointed to this commit (no CI run had consumed it).

Honest edges: the zip grew ≈ 43 MB (slim custom-configure FFmpeg build stays
an option); the bundled DLLs sit in a user-writable folder, a DLL-hijack
surface shared with every ProgramData-installed plugin (a checksum-at-load
pass is the recorded mitigation if wanted); README carries the FFmpeg
redistribution note (obs-deps / ffmpeg.org).

## Rolling dev draft release on main pushes (2026-11-14)

Owner asked whether the missing-zip problem (a `main` push skips
`Create Release 🛫` — it is tag-gated) can be automated; the original plan
had deliberately kept push/publish owner-only ("the repo is left fully
push-ready" — autonomous-run-plan decision). Owner chose: every push to
`main` updates one rolling draft release, version tags stay the real
release path.

- `build-project.yaml` check-event: `main`/`master` pushes now build config
  `Release` (same config as tag builds; semver-tag rule unchanged).
- `push.yaml` gains `dev-release` (`if: github.ref_name == 'main'`, after
  `build-project`): downloads the windows artifact, renames its zip to the
  fixed `obs-replay-to-disk-dev-windows-x64.zip`, and posts through the
  same pinned `softprops/action-gh-release` action with `tag_name: dev`,
  `draft: true`, `update: true`, `overwrite_files: true` — one draft,
  always current, SHA + date + checksum in the body. Drafts are
  owner-visible only, so nothing publishes itself; publishing stays the
  owner's click. The `dev` tag is created by `GITHUB_TOKEN`, which cannot
  re-trigger workflows, so there is no run loop.

Verified: both workflow files parse as YAML; artifact name/glob match the
existing `windows-build` upload step. Not exercised on a run — first proof
is the next `main` push (owned by the owner): a draft "Replay to Disk — dev
build" should appear under Releases → Drafts.

## OBS 33 flat plugin layout (2026-11-14)

Owner's screenshot from their streaming PC: OBS's Plugin Manager listed
obs-replay-to-disk with the Legacy badge and "installed to a location that
will stop working in the future" — and the zip itself caused it. The
[OBS Legacy Plugin Locations KB](https://obsproject.com/kb/legacy-plugin-locations)
(2026-09-21) and its [developer page](https://obsproject.com/kb/legacy-plugin-developer-info)
redefine the supported Windows layout: OBS 33 loads
`C:\ProgramData\obs-studio\plugins\<name>\<name>.dll` + `data\*` FLAT; the
previous recommendation `…\<name>\bin\64bit\<name>.dll` — exactly what our
zip and README shipped — is now itself a Legacy structure, flagged in the
Plugin Manager and not loaded from OBS 34 (≈ Aug 2027) on. Portable builds
have the same rule: `<install>\plugins\<name>\` is current, `obs-plugins\`
is Legacy. (OBS loads the new location first and skips a same-named legacy
copy, so mixed installs self-heal.)

Changes: `cmake/windows/helpers.cmake` installs the plugin DLL and PDB to
`${target}/` instead of `${target}/bin/64bit`; the FFmpeg bundle install in
CMakeLists went flat the same way. `data/` was already correct. CI needs no
change — the zip's root folder and name are unchanged, only its nesting.

Verified: fresh install tree is flat (`obs-replay-to-disk\{obs-replay-to-disk.dll,
obs-replay-to-disk.pdb, the 8 FFmpeg DLLs}, data\locale\en-US.ini`), ctest
3/3, gersemi clean; rig run `flat-t4` (`2026-11-14 08-54-15.txt`) on the
portable OBS 33.0.1 with the plugin deployed ONLY at
`.testrig\obs\plugins\obs-replay-to-disk\` (the new portable location):
loaded from there, FFmpeg preload and both saves green (20.032 s / 31.000 s
bands, `warnings=0`, 0 leftovers, leaks 1). README install/build text and
the KB (new reference `obs-plugin-install-layout.md`, fixed
`release-tag-convention.md` layout sentence) updated with it.

Immediate remedy on the affected machine (their OBS 33 install, our
README-era zip): with OBS closed, move everything from
`…plugins\obs-replay-to-disk\bin\64bit\` up into `…plugins\obs-replay-to-disk\`
next to `data\` — the flat set is what the new layout wants, and our bundled
FFmpeg dependencies move with the DLL, so the "complex dependencies"
exception on the OBS page does not apply.
