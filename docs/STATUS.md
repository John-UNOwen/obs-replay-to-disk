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

