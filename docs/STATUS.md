# Status

## WP0 repository and build: IN PROGRESS
- Date: 2026-11-12
- Commits: (filled at the end of WP0)
- Acceptance:
  - [ ] The three build commands succeed — pending
  - [ ] Test-rig OBS log shows the plugin load line and avformat major 62 — pending
  - [ ] CI green on Windows — pending (not observed yet)
- Deviations from the handoff:
  - Local toolchain: no Visual Studio 2022 on this host. Builds run against "Visual Studio
    Build Tools 2026" v18.9 (MSVC 14.51.36231, Windows SDK 10.0.26100) at
    `%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools`; cmake 4.3.1 and ninja 1.13.2
    exist only inside that tree and are not on PATH. The `windows-x64` preset generator is
    retargeted from "Visual Studio 17 2022" to "Visual Studio 18 2026"; the SDK pin
    `version=10.0.22621` is dropped to plain `x64` because only 10.0.26100 is installed here.
    CI keeps the template's `windows-2022` runner; if that lacks the 2026 generator this entry
    gets an update.
  - `buildspec.json` keeps `"email": ""`: `cmake/common/bootstrap.cmake` reads the key and a
    missing key aborts configure; the owner decision is no personal email.
  - macOS hashes for obs-deps 2026-10-23 are the `universal` assets from the release notes
    (handoff B2 only gave windows-x64); the obs-studio 33.0.1 macos hash equals the computed
    windows-x64 hash because both keys point at the same source archive URL.
- Open problems: none yet.
