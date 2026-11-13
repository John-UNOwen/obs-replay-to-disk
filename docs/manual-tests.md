# Manual test checklist

This checklist is for a human running OBS with a real scene or game. It reflects the build on `main` after the post-checkup fixes: the `v0.1.0` tag (`f8740ec`) plus the 12 fixes recorded in [STATUS.md](STATUS.md), built against OBS Studio 33.0.1 Windows x64 (obs-deps 2026-10-23). What was already machine-verified on the development rig (build gates, self-tests, the WP3/WP4 regression, memory-leak counts) is recorded in [STATUS.md](STATUS.md). Only what needs a person at the OBS window is listed here.

Install the plugin first (see the README). Work down the list and check a box when the line passes. If something fails, note which line failed and what OBS did, and copy the OBS log line (Help > Log Files > View Log Files).

## Long runs and disk

- [ ] 30-minute window, 60 minutes of play: disk use stays flat at about window x bitrate.
  How-to: open Tools > Replay to Disk, set the buffer length to 30 min, click Start Buffer, and play for at least 60 minutes. Watch the "GB on disk" line in the dialog or the size of the buffer folder (default `<OBS recording folder>/.rolling-replay-buffer`). At the default 30000 kbps CBR it should level off near 6.8 GB and stay there.
- [ ] A full 30-minute save while playing: no dropped frames, and note the time it took.
  How-to: with the 30-minute buffer still running and something moving on screen, click Save Replay. Watch OBS's Stats dock (Dropped Frames should stay 0 while the save runs). The OBS log line `save finished: ... took=...s` is the time it took.
- [ ] OBS Recording and Streaming running next to the buffer.
  How-to: Start Buffer, then start a Recording and a Stream as you normally would. All three should run at once. Check OBS's Stats dock: anything the plugin adds shows as a second encode, so expect higher GPU or CPU use, but no dropped or lagged frames you can attribute to the plugin.
- [ ] Buffer folder on a second drive, output folder on another.
  How-to: in the dialog, set the buffer folder to a folder on one drive and the output folder to a folder on a different drive, click Apply, then Start Buffer and Save Replay. The `rrseg_*.mkv` files should roll in the buffer folder and the `Replay_*.mp4` should land in the output folder.

## Encoders and files

- [ ] Each hardware encoder the machine has; the saved H.264, HEVC and AV1 files play in VLC and in Windows Media Player.
  How-to: in the dialog, pick one video encoder at a time (Automatic picks the first available; the list shows every H.264, HEVC and AV1 encoder OBS reports). For each: Start Buffer, let it run a minute, Save Replay, and play the MP4 in both players.
- [ ] NVENC and AMF matrix (untested on the development host).
  How-to: on a machine with an NVIDIA or AMD GPU, repeat the encoder box with `NVIDIA NVENC H.264/HEVC/AV1` and `AMD HW H.264/H.265/AV1`, once each, including a save per encoder. The development host is Intel-only, so only Quick Sync has been verified there; the measurements in [wp1-findings.md](wp1-findings.md) cover QSV. If you get a failure with NVENC or AMF, report it with the OBS log.
- [ ] HDR (best effort, untested).
  How-to: if you have an HDR-capable display and OBS in HDR video mode (Settings > Advanced > Video), start the buffer and save a clip. Check the clip opens and looks right. HDR through the save has never been tested; treat problems here as expected and still report them.

## Audio

- [ ] Audio tracks 1, 2 and 3 carry different sources: each is in the MP4.
  How-to: put different audio on tracks 1, 2 and 3 in OBS's mixer (for example desktop audio on 1, a mic on 2, a browser source or media source on 3; enable a track with the Advanced Audio Properties or per-source track settings). Tick tracks 1, 2 and 3 in the dialog, Apply, Save Replay. Check the saved MP4: it should have three audio streams (ffprobe shows them; a player's audio-track menu should switch between three audible tracks).

## Failure behavior

- [ ] Disk nearly full: the buffer stops with a visible error, and OBS keeps running.
  How-to: point the buffer folder at a small partition or a drive that is nearly full (a spare USB stick or a small test partition works). Start Buffer: if the free space is below the preflight estimate, the start is refused with an error in the dialog's status line (Last error) and a `buffer error:` line in the OBS log. If space runs out while buffering, the buffer stops on its own the same way. OBS itself must stay usable.
- [ ] A/V sync of a 30-minute save looks right at the start and at the end.
  How-to: save a full 30-minute window while something with obvious sync cues (a talking face, a metronome) plays. Watch the first minute and the last minute of the clip. A constant audio offset of a few ms up to about 40 ms is known and acceptable; it must not grow through the clip.
- [ ] With the plugin removed, OBS behaves as stock.
  How-to: close OBS, delete `%ProgramData%\obs-studio\plugins\obs-replay-to-disk`, start OBS. No Tools menu item, no hotkeys, no dock, and recording, streaming and the built-in Replay Buffer all behave normally.
- [ ] OBS cannot change its Video settings while the buffer runs (a known limitation, not a bug).
  How-to: OBS grays out the Video page when you open Settings during buffering, so set the change up first: open Settings, go to the Video page, then start the buffer with the hotkey, change the Base (Canvas) Resolution, and click Apply. OBS stays at the old resolution: the OBS log gets a `Tried to reset when already active` line plus the plugin's `while the buffer runs OBS cannot change its Video settings` warning, and the new value only takes effect at the next OBS start. Close Settings, click Stop Buffer, apply the same change again: it lands immediately this time. The AutoConfig Wizard behaves the same while the buffer runs (it finishes, OBS keeps the old resolution), and Scene Collection > Remigrate Scene Collection refuses with a "Reset not possible" box. A profile switch is the one video reset OBS does run while the buffer is live, and the plugin steps aside for it: check it in the next box.
- [ ] A profile switch stops the buffer so the new profile's video settings land; a save running at that moment is cancelled.
  How-to: give the profile you will switch to a different Base (Canvas) or Output (Scaled) Resolution in Settings > Video, so you can see the change take effect. Start the buffer, then switch profiles (OBS logs it as `Switched to profile '<name>'`). The OBS log gets `buffer stopped` from the plugin before OBS writes its `video settings reset:` block, the block shows the new profile's resolution, and there is no `Tried to reset when already active` line. With Autostart off (the default) the buffer stays stopped after the switch, so start it again. Then repeat with a save in flight: set the buffer length to 30 min, Start Buffer, press Save Replay, and switch profile while the dialog status line still reads "Saving". The log gets `engine: video reset: cancelling the in-flight save` and `save failed: cancelled`, the output folder gets no new `Replay_*.mp4` and no `.rrpart` file, and the switch still lands.

## Dialog, dock and hotkeys (WP5)

- [ ] Both hotkeys appear in Settings > Hotkeys, work, and still work after restarting OBS.
  How-to: Settings > Hotkeys, filter for "Replay to Disk": "Replay to Disk: Save" and "Replay to Disk: Start/Stop Buffer". Bind both, press them (start, save, stop). Close OBS (File > Exit), start it again, press them again; the bindings should still be listed and still work. This also covers the plugin reloading its saved bindings at startup, which OBS does not do for plugins itself.
- [ ] Each setting survives an OBS restart.
  How-to: Tools > Replay to Disk, change every value you can (autostart, buffer length, save scope, folders, filename format, encoder, rate control, bitrate, quality level, extra JSON, tracks, audio bitrate, save sound), click Apply, exit OBS, start it, reopen the dialog. Every value should be as you left it.
- [ ] A buffer folder of `C:\` or the output folder is refused with a message.
  How-to: in the dialog set the buffer folder to a drive root (`C:\`), click Apply: a warning box explains it cannot be a drive root and the settings are not saved. Then set the buffer folder to the same path as the output folder, click Apply: it is refused with a message the same way.
- [ ] Start, Save and Stop work from the dialog and the dock; the status line follows; the restart prompt works while buffering.
  How-to: open Tools > Replay to Disk and also the dock (Docks menu > Replay to Disk). From both: Start Buffer (the status line goes to Buffering and the buffered time climbs), Save Replay (enabled only while buffering; it completes with the saved file listed), Stop Buffer (status back to Off). Change any setting while the buffer runs: you get a prompt to restart the buffer; Yes stops it and starts again with the new settings (OBS briefly shows the settings lock ending and starting again).
