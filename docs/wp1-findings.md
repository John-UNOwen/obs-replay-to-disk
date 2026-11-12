# WP1 findings: spike inside real OBS

Date: 2026-11-12. Branch `wp1-spike` (throwaway spike code). Every run used the
portable OBS 33.0.1 test rig (handoff 7.1), `RR_SELFTEST=spike`, test media
`.testrig/media/src.mp4` (testsrc2 1280x720@60 + 1 kHz sine, 20 s, looped) on
output channel 6, `RR_SELFTEST_WINDOW=30`, and a fresh `.testrig/runs/<tag>` dir.
Rig video mode is 1280x720 output at 30 fps, NV12/709/partial, so the spike
encodes at fps=30/1. Segment durations below were measured, never assumed (E6).

Spike flow per run: worker thread → create encoder + AAC track 0 → start
unsplit `out/reference.mkv` on the same encoders → 1.5 s → start the split
output (`buffer/rrseg_*.mkv`, `max_time_sec=10`, D2 settings) → 45 s, forced
`split_file` at t=12 s → stop both outputs from the worker → `rr_remux`
in-process → `out/spike.mp4` → `selftest finished`.

## Verdicts

| Check | Verdict | Evidence (runs A2 / B2 / C / D unless noted) |
|---|---|---|
| U1: start and stop from a non-UI worker thread, clean OBS exit | PASS | All 6 spike runs. Encoder, both outputs, signals, and `rr_remux` all run on the worker (`CreateThread`, e.g. tid 30116 in A2); both `obs_output_stop` calls come from that thread; OBS exited cleanly via WM_CLOSE every run (`OBS_EXITED_CLEANLY`, plugin unloaded, no crash sentinel). |
| U2: 0 dropped/lagged frames attributable to the plugin, OBS recording at the same time | PASS (Intel QSV only) | B2 = `obs_qsv11_v2` + `--startrecording` (recording file 40.8 MB written). Every 5 s UI-timer tick and the final line: `lagged_frames=0 rr_dropped=0 ref_dropped=0`. OBS's adapter thread and both outputs never dropped. NVENC/AMF are untestable here — see "Machine notes". |
| U3: DLL links obs-deps FFmpeg on Windows, loads in OBS 33.0.1, `rr_remux` runs in-process | PASS | Load line `[obs-replay-to-disk] avformat 62.12.102` (major 62) in every run; the remux ran inside obs64.exe (no child process) and every output passed verification. |
| `verify_remux.py` in both modes, exit 0 | PASS | All runs, table below. |
| `ffmpeg -f null` prints nothing | PASS | All runs: `ffmpeg -v warning -i spike.mp4 -map 0 -f null -` printed nothing. |
| U4: same for a hardware encoder, and HEVC | PASS (QSV H.264 / HEVC / AV1) | B/B2 (H.264), C (HEVC, `spike.mp4` video tag is `hevc,hvc1` per ffprobe), D (AV1, `av01`/Main). All verify-clean with `warnings=0`. NVENC/AMF untested (no such hardware). |
| Segment closure time on this machine | PASS, well under 100 ms | `rr_file_closed` (snippet 8.4, 20 ms poll) returned true on its first attempt for every segment of every run — logged as `after 0ms`. The check itself starts 7–60 ms after the boundary event is dequeued. |
| U5: HDR | NOT TESTED | Rig is SDR (709 partial). Untouched, as planned. |

## Measured numbers

### split_file → file_changed latency (t=12 s forced split, E5 budget 3 s)

| Run | Encoder | Latency |
|---|---|---|
| A2 | obs_x264 | 292 ms |
| A  | obs_x264 | 294 ms |
| C  | obs_qsv11_hevc | 383 ms |
| D  | obs_qsv11_av1 | 535 ms |
| B  | obs_qsv11_v2 | 798 ms |
| B2 | obs_qsv11_v2 (+OBS recording) | 877 ms |

All inside the 3 s budget. The QSV values (798/877 ms) exceed the 164–632 ms
headless band from E5 but are the same order of magnitude; the 877 ms case is
the QSV H.264 run while OBS also recorded.

### Segment durations (never assume 10 s — E6)

Forced split at t=12 s always landed before the first timer-based split, so the
first boundary is the forced one. The split output starts ~1.5 s after t0
(x264) or ~3.9 s (QSV — encoder init takes ~2.4 s), which sets the first
segment's length. Later boundaries are timer splits.

| Run | Segment durations (s) |
|---|---|
| A2 | 10.84, 10.00, 10.00, 10.01, 2.70 (final, closed at stop) |
| A  | 10.83, 10.01, 10.00, 10.01, 2.68 |
| B  | 8.95, 10.01, 10.00, 10.02, 2.20 |
| B2 | 8.95, 10.01, 10.00, 10.02, 2.24 |
| C  | 8.96, 10.01, 10.00, 9.99, 2.58 |
| D  | 8.92, 10.01, 10.00, 10.00, 2.48 |

### Thread ids (D6/D7 evidence)

`file_changed` and `stop` arrive on encoder/muxer threads, the stats tick on the
UI thread, output management on the worker — four different tids, e.g. run A2:
worker 30116, file_changed 187500, stop 187500 (a muxer thread), stats/UI
47576. Run B: worker 67348, file_changed 138440 then 38432 (different audio or
video threads across events), stats 146048. Handlers only captured
`os_gettime_ns()` + copied the path into the queue.

### In-process rr_remux results

| Run | Frames | Duration | Time | grid_warnings | skipped_tail | reorder_delay |
|---|---|---|---|---|---|---|
| A2 | 1291 | 43.03 s | 0.15 s | 0 | 0 | 2 |
| A  | 1291 | 43.03 s | 0.17 s | 0 | 0 | 2 |
| B  | 1220 | 40.67 s | 0.10 s | 0 | 0 | 2 |
| B2 | 1217 | 40.57 s | 0.12 s | 0 | 0 | 2 |
| C  | 1232 | 41.07 s | 0.13 s | 0 | 0 | 2 |
| D  | 1227 | 40.90 s | 0.04 s | 0 | 0 | 0 |

AV1 (D) reported reorder_delay 0 — no B-frames from the QSV AV1 encoder.
`skipped_tail` was 0 everywhere; no stop-time stub appeared in any run
(E17 not reproduced with a clean `obs_output_stop`).

### verify_remux.py (both modes)

Every run, both invocations exit 0:

- `--segments`: 5 segments each, both streams `continues` from the second
  segment on (video + audio), no gaps.
- replay vs reference: video and audio `matched_in_order == packets`,
  `spread ≈ 0.667 ms`, `dts_monotonic=True`, `dts<=pts=True`, `alignments=1`,
  constant A/V offset vs reference −5.333 ms (well inside ±45 ms).
- `ffmpeg -v warning -i spike.mp4 -map 0 -f null -`: silent, every run.

One quirk: `verify_remux.py --segments` globs `seg_*.mkv`, so the D2 `rrseg_`
names never match. For the check the segments are copied to
`runs/<tag>/verify/` as `seg_NNN.mkv` (name order = chronological) plus
`reference.mkv`. The copy is what the script is designed to read; nothing about
the segments changes.

### Stop behavior (E18)

`obs_output_stop` raised `stop` in 1100 ms (x264 runs) or 400 ms (QSV runs),
both outputs, every time. `obs_output_force_stop` was never needed.

## Memory leaks

Every plugin session logged `Number of memory leaks: 1`. This is the rig, not
the plugin. Proven by controls with the plugin DLL renamed out:

| Session | Plugin | Leaks |
|---|---|---|
| First session of a fresh config (WP0, `05-53-14`) | loaded | 0 |
| First session of a rebuilt fresh config, no plugin (`13-39-14`) | absent | 0 |
| Second session of that config, no plugin, saved scene collection loaded (`13-41-41`) | absent | 1 |
| Fresh config, plugin loaded, `RR_SELFTEST` unset, spike never runs (`13-40-50`) | loaded, idle | 1 |
| Every spike run A/A2/B/B2/C/D | running | 1 |

Stock OBS 33.0.1 leaks exactly 1 allocation in any session after the first one
(on this rig: from the second launch, with the saved scene collection being
loaded). The plugin adds 0 on top of that baseline. WP0's `leaks: 0` was
literally true because it was the first session of a fresh config.

## Machine notes

- GPUs: Intel Arc A770 (adapter 0, used by OBS), UHD Graphics 770, Arc B580.
  OBS enumerates a fourth adapter that also reports as "Intel(R) Arc(TM) A770
  Graphics" (adapter 3). No Parsec virtual display appears in OBS's adapter
  list on this machine. QSV encoder creation was clean for all three codecs;
  no adapter weirdness, no encoder-creation failures, so the "stop and report"
  path was never taken.
- NVENC is not present (`LoadLibrary failed for 'nvEncodeAPI64.dll'`,
  `NVENC not supported`, `Failed to initialize module 'obs-nvenc.dll'`) and no
  AMD adapter is enumerated, so U2/U4 with NVENC or AMF cannot be tested on
  this Intel-only host. U2 is settled for Intel QSV only.
- The rig records to `.testrig/runs/rec` (profile redirected there); the
  B/B2 `--startrecording` runs wrote `2026-11-12 13-33-31.mp4` / a B2 file
  (~40 MB each).

## Deviations from the WP1 instructions (all forced by OBS 33.0.1 or tooling)

1. `obs_frontend_get_stats()` no longer exists in OBS 33.0.1 — the frontend API
   (`.deps/include/obs-frontend-api.h`) has no stats function. The periodic
   dropped/lagged logging uses a UI-thread `QTimer` (small C++ shim,
   `spike-timer.cpp`) calling `obs_get_total_frames()` /
   `obs_get_lagged_frames()` and, once outputs exist,
   `obs_output_get_frames_dropped()` per output. That is the same data the
   `numDroppedFrames`/`numLaggedFrames` fields carried.
2. The reference spike's `video_output_get_skipped_frames(obs_get_video())`
   also no longer exists in OBS 33; the equivalent is
   `obs_get_lagged_frames()`.
3. Encoder settings come from `obs_encoder_defaults(id)` +
   `rate_control=CBR` + `bitrate` + `keyint_sec=1` last (handoff 5.5), not from
   a fresh `obs_data_t` with a hardcoded `preset` as in the Linux reference.
   `obs_encoder_defaults` keeps each encoder's own valid preset/profile
   defaults, which matters on Windows QSV. Bitrate default 30000 kbps
   (`RR_SELFTEST_BITRATE` overrides).
4. First file name uses `os_generate_formatted_filename` per 5.5 (the reference
   hardcoded `seg_first.mkv`); output/encoder/signal call order is otherwise
   identical to the reference.
5. Runs A and B used a first build whose stats line lacked the per-output
   drop counters; B2 (and later C/D, plus redo A2) carry them. Run A's "0
   dropped" evidence is limited to `lagged_frames=0` plus A2, which repeats the
   baseline with full counters.
6. `obs_module_unload` waits up to 20 s for the worker to finish
   (`rr_spike_wait_worker`). The handoff has no such step; without it OBS exit
   mid-spike could unload the DLL while the worker thread was still executing
   in it. All runs finished the worker long before OBS closed; the wait never
   triggered.
7. Config additions for unattended runs: rig profile records into
   `.testrig/runs/rec`, `ConfirmOnExit=false` in the rig `user.ini`, and the
   close helper targets the (tray-hidden) main window with `WM_CLOSE` even when
   `IsWindowVisible` is false. Rig-local only, not committed.

## What WP2+ should keep in mind

- The OBS 33 frontend API has no stats function (deviation 1) — the status dock
  in WP5 must use libobs getters, and WP3/WP4 self-test acceptance should log
  via the same getters.
- `verify_remux.py --segments` needs `seg_*.mkv`-style names; keep a copy step
  or a glob option in test tooling.
- QSV encoder creation cost ~2.4 s; a real buffer's START should expect the
  output to go live seconds after the call on hardware encoders, and the first
  segment's start time must be taken after `obs_output_start` returns, not at
  request time (the spike logs both).
