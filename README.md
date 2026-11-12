# Replay to Disk

A disk-backed rolling replay buffer for OBS Studio. It keeps the last 30 seconds to 30 minutes of video and audio in small files on disk, then writes them out as one MP4 when you press a hotkey. It works like OBS's built-in Replay Buffer, except the buffer lives on disk instead of RAM, so a long buffer costs disk space rather than memory.

This plugin was built with AI assistance.

This build targets OBS Studio 33.0.1 on Windows x64. It links against the FFmpeg that ships with that OBS version; on an OBS with a different FFmpeg major it will not load.

## Side effects

- OBS locks its settings while the buffer runs. OBS disables the Video page and most of the Output page whenever any video output is active, the same lock a running recording causes. Stop the buffer to change those settings.
- If you record or stream at the same time, the GPU or CPU encodes twice.
- A saved clip can start up to 1 s early, because cuts land on keyframes.
- A saved clip has a constant audio/video offset of a few milliseconds up to about 40 ms. It does not grow over time.

## Status

Work in progress: the plugin builds and loads in OBS, and does nothing else yet. See [docs/STATUS.md](docs/STATUS.md) for what is done and what is left.

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
