# Replay to Disk

A disk-backed rolling replay buffer for OBS Studio. It keeps the last 30 seconds to 30 minutes of video and audio in small files on disk, then writes them out as one MP4 when you press a hotkey. It works like OBS's built-in Replay Buffer, except the buffer lives on disk instead of RAM, so a long buffer costs disk space rather than memory.

The buffer runs on its own encoder and its own output. OBS's Recording, Streaming and built-in Replay Buffer keep working unchanged and can run at the same time. When the buffer stops, or OBS exits, the buffered files are deleted.

This plugin was built with AI assistance.

## Requirements

- Windows x64 with an OBS Studio 33.0.1 install.
- The build is pinned to exactly OBS 33.0.1. The plugin links against the FFmpeg that ships with that OBS version (avformat major 62). If OBS later ships a different FFmpeg major, the plugin fails to load in it (OBS logs a failed-to-load-plugin line) until a build for that OBS version exists. This is normal for OBS plugins and needs a rebuild, not a setting.

## Install

1. Download the release package for OBS 33.0.1.
2. Copy the `obs-replay-to-disk` folder from the package into `%ProgramData%\obs-studio\plugins\`. You should end up with `%ProgramData%\obs-studio\plugins\obs-replay-to-disk\bin\64bit\obs-replay-to-disk.dll`.
3. Start OBS. To verify it loaded: Help > Log Files > View Log Files, and look for `[obs-replay-to-disk] plugin loaded successfully (version 0.1.0)` and `[obs-replay-to-disk] avformat 62.12.102`.

To uninstall, remove the `%ProgramData%\obs-studio\plugins\obs-replay-to-disk` folder and restart OBS.

## Controls

- Two hotkeys in Settings > Hotkeys: "Replay to Disk: Save" and "Replay to Disk: Start/Stop Buffer". Bindings are saved per OBS profile.
- The full settings dialog at Tools > Replay to Disk. It has Start Buffer / Save Replay / Stop Buffer buttons and a status line showing state, buffered time, disk use, last saved file and last error.
- An optional dock with the same status line and buttons: Docks > Replay to Disk.

## Settings

Everything is set in the dialog. The values are stored in `settings.json` in OBS's config folder, under `plugin_config/obs-replay-to-disk/` (normally `%APPDATA%\obs-studio\plugin_config\obs-replay-to-disk\settings.json`). These are the settings:

| Setting | Default | Notes |
|---|---|---|
| Autostart | off | start the buffer when OBS finishes loading |
| Buffer length | 30 min | 30 s to 30 min |
| Save scope | Full window | or: only footage since the previous save |
| Buffer folder | OBS recording folder + `/.rolling-replay-buffer` | the plugin only ever deletes its own `rrseg_*.mkv` files here |
| Output folder | OBS recording folder | where `Replay_*.mp4` files are written |
| Filename format | `Replay_%CCYY-%MM-%DD_%hh-%mm-%ss` | OBS's filename format codes |
| Video encoder | Automatic | first available of NVENC H.264, AMD HW H.264, Quick Sync H.264, x264; or pick any available H.264, HEVC or AV1 encoder |
| Rate control | CBR | CBR or constant quality (VBR is deliberately not offered, it is ignored by OBS's x264) |
| Bitrate | 30000 kbps | 1000 to 150000 |
| Quality level | 20 | CQ level, 1 to 51, used with constant quality |
| Extra encoder settings | `{}` | JSON merged into the encoder settings |
| Audio tracks | track 1 | any of tracks 1 to 6, at least one |
| Audio bitrate | 160 kbps | 64 to 320, per track (AAC) |
| Save sound | on | system beep when a save finishes |

With constant quality, the disk-space estimate in the dialog assumes about 50 Mbps.

## Side effects

- OBS locks its settings while the buffer runs. OBS disables the Video page and most of the Output page whenever any video output is active, the same lock a running recording causes. Stop the buffer to change those settings.
- If you record or stream at the same time, the GPU or CPU encodes twice.
- A saved clip can start up to 1 s early, because cuts land on keyframes.
- A saved clip has a constant audio/video offset of a few milliseconds up to about 40 ms. It does not grow over time.

## Disk use

| CBR video bitrate | 30-minute buffer | Peak during a full save |
|---|---|---|
| 20 Mbps | ≈ 4.5 GB | ≈ 9 GB |
| 30 Mbps (default) | ≈ 6.8 GB | ≈ 13.5 GB |
| 50 Mbps | ≈ 11.3 GB | ≈ 22.5 GB |

A full save reads and writes the whole buffer once. In rig tests a full-window save on the development machine's SSD took 0.05 s for a 30 s window and 1.5 s for a 7-minute window; a 30-minute save on slower media has not been measured, expect tens of seconds on an SSD and a few minutes on a hard disk.

## Known limits

- Only Quick Sync (Intel) has been machine-tested for the encoder path. NVENC and AMD AMF are supported in the settings and follow the same code path, but are untested; see [docs/manual-tests.md](docs/manual-tests.md).
- HDR is best-effort and untested.
- A/V sync, long saves, low-disk behavior and player compatibility still need a human run through [docs/manual-tests.md](docs/manual-tests.md). Everything that was checked on the development rig is recorded in [docs/STATUS.md](docs/STATUS.md).

## Building

Windows x64 only. Requires Visual Studio Build Tools (2026) with the Desktop C++ workload, CMake 3.28+, and network access on first configure (the build downloads OBS sources and prebuilt dependencies).

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

The installed plugin lands in `release/RelWithDebInfo/obs-replay-to-disk/`: the DLL and PDB in `bin/64bit/`, locale files in `data/locale/`.

## License

GPL-2.0-or-later.
