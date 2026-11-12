# Status

## WP0 repository and build: IN PROGRESS
- Date: 2026-11-12
- Commits: 516cb55 f379783 f379bc1 af8fd45 beace0e 9ea373c e8600dd (+ the evidence commits)
- Acceptance:
  - PASS: The three build commands succeed. `cmake --preset windows-x64`
    (configure, "Configuring done (71.7s) / Generating done"), `cmake --build --preset
    windows-x64 --config RelWithDebInfo` (links obs-replay-to-disk.dll),
    `cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo`
    (installs DLL+PDB to `release/RelWithDebInfo/obs-replay-to-disk/bin/64bit/`), all exit 0.
  - PASS: Test-rig OBS 33.0.1 (portable, `.testrig/obs`) loaded the plugin from
    `obs-plugins/64bit/`. Log `2026-11-12 05-53-14.txt`:
    `[obs-replay-to-disk] plugin loaded successfully (version 0.1.0)` and
    `[obs-replay-to-disk] avformat 62.12.102` → avformat major 62, as expected for the
    obs-deps FFmpeg 8.1.2. OBS closed via window message; log tail: `Number of memory leaks: 0`.
  - PENDING: CI green on Windows (format + windows-build), not observed yet.
- Hashes used in buildspec.json:
  - obs-studio 33.0.1 source zip (computed): `0f228b960c099c6f6bca9f09a0c3516f14fdf1dd96e1d4702145c5d3ba8cba31`
  - prebuilt 2026-10-23 windows-x64: `7508972a867a0ef68c26cd8bdba61fd999edd0a92fa9eb9585774de9bf51bfbe` (handoff B2)
  - qt6 2026-10-23 windows-x64: `78cd7a138464bb6edcdd3b85fcd63a5a7c69e369629963f3bc395bab74fb3f47`, debugSymbols `5c97809dcb663a51e5b5f47983359d03a213a8c711c4b158a1b62429b8dff6be` (handoff B2)
- Deviations from the handoff:
  - Local toolchain: no Visual Studio 2022 on this host. Builds run against "Visual Studio
    Build Tools 2026" v18.9 (MSVC toolset 14.51.36231 / compiler 19.51, Windows SDK 10.0.26100)
    at `%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools`; cmake 4.3.1 and ninja 1.13.2
    exist only inside that tree and are not on PATH. The `windows-x64` preset generator is
    retargeted from "Visual Studio 17 2022" to "Visual Studio 18 2026"; the SDK pin
    `version=10.0.22621` is dropped to plain `x64` because only 10.0.26100 is installed here.
    CI keeps the template's `windows-2022` runner for now.
  - `buildspec.json` keeps `"email": ""`: `cmake/common/bootstrap.cmake` reads the key and a
    missing key aborts configure; the owner decision is no personal email.
  - macOS hashes for obs-deps 2026-10-23 are the `universal` assets from the release notes
    (handoff B2 only gave windows-x64); the obs-studio 33.0.1 macos hash equals the computed
    windows-x64 hash because both keys point at the same source archive URL.
  - This OBS 33.0.1 build ships bundled plugins in `core/<name>/` rather than
    `obs-plugins/64bit/`, but a plugin installed under `obs-plugins/64bit/` (handoff 7.1) loads
    fine, so the rig keeps the handoff layout.
- Open problems: none.
