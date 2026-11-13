# Aligna

Aligna is an open-source digital audio workstation (JUCE / C++20) focused on one job: **aligning audio tracks**.
Import several recordings of the same performance, line them up on a timeline (by hand, with snapping and nudging,
or automatically by cross-correlation), listen, and export a mixed WAV. Source audio files are never modified.

## Features

- Import WAV, AIFF, FLAC and MP3 (menu, Ctrl/Cmd+I, or drag and drop); each file becomes a track. Sources are resampled to the project rate once at import and are never modified.
- Timeline with waveforms, zoom, beat grid and snapping; move, trim, split, delete and nudge clips (the later clip covers the earlier one where they overlap).
- Per-track volume, pan, mute and solo; transport with play/pause/stop, BPM, time signature and grid settings.
- Auto-align: pick a reference clip and shift other clips to match it by cross-correlation (shifts only, never stretches; reports inverted polarity). You confirm before anything moves.
- Undo/redo for every edit.
- Projects are saved as `.aligna` JSON files (audio paths relative to the project when possible). If audio files have moved, the project still opens, the missing clips are shown in red and the rest plays.
- Export the mix as a stereo WAV, 16- or 24-bit, at the project sample rate.

## Keyboard shortcuts

Use Cmd instead of Ctrl on macOS.

| Shortcut | Action |
| --- | --- |
| Ctrl+N | New project (44.1 or 48 kHz) |
| Ctrl+O | Open project |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+I | Import audio |
| Ctrl+E | Export mix |
| Ctrl+Q | Quit (not on macOS, where the app menu provides it) |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z (Ctrl+Y on Windows/Linux) | Redo |
| Ctrl+L | Align selected clips to the reference |
| Space | Play / pause |
| S | Split selected clips at the playhead |
| G | Toggle snap |
| Left / Right | Nudge selected clips by 1 ms (Shift: 10 ms, Alt: 1 sample, Ctrl/Cmd: one grid step) |
| Delete / Backspace | Delete selected clips |
| Esc | Cancel a drag in progress |
| Ctrl + mouse wheel | Zoom the timeline |
| Shift + mouse wheel | Scroll the timeline horizontally |

## Working with the timeline

- **Select clips:** click a clip; Shift-click adds or removes clips from the selection.
- **Align reference:** right-click a clip and choose "Set as align reference". Then select the clips to move and use Edit > Align to Reference (Ctrl/Cmd+L).
- **Time selection:** Shift-drag on the ruler. Auto-align then only looks at that region (at most its first 60 s); without a selection it uses up to the first 60 s of overlap.
- **Clip gain:** double-click a clip.
- **Bypass snap:** hold Alt while dragging (the Snap button or G toggles snapping on and off).
- **Time signature:** the two boxes next to BPM in the transport bar set beats per bar (1 to 32) and the beat unit (1, 2, 4, 8 or 16); each change is one undo step.

### Aligning two recordings

1. Import both files (Ctrl/Cmd+I); each becomes a track.
2. Right-click the clip you want to keep in place (for example the cleanest recording) and choose "Set as align reference".
3. Click the other clip to select it (Shift-click to select several).
4. Optional: Shift-drag on the ruler over a section that both recordings contain.
5. Press Ctrl/Cmd+L, set the search range and press Align.
6. Check the offset and confidence of each row, tick the clips to move and press Apply. Nothing moves until you press Apply; if you change the project meanwhile, the results are marked stale and you must align again.
7. Press Space to listen; Ctrl/Cmd+Z undoes the alignment.

## Building

Requirements: CMake 3.22+, a C++20 compiler, and (Linux only) the JUCE system dependencies:

```sh
sudo apt-get install libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
  libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
```

The list matches the CI workflow (`.github/workflows/build.yml`). JUCE and Catch2 are fetched automatically by CMake.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The app is produced at `build/Aligna_artefacts/Release/` (`Aligna`, `Aligna.app` or `Aligna.exe`).

## Running unsigned builds

Release builds from CI are not code-signed.

- **macOS:** on macOS 14 and earlier, right-click `Aligna.app` and choose Open. On macOS 15 and later that no longer bypasses Gatekeeper: try to open the app once, then go to System Settings > Privacy & Security, scroll to the Security section and click "Open Anyway" next to Aligna. Alternatively run `xattr -dr com.apple.quarantine Aligna.app` in a terminal.
- **Windows:** in the SmartScreen dialog choose "More info", then "Run anyway".
- **Linux:** extract the tarball and run `./Aligna`.

## License

AGPL-3.0, see [LICENSE](LICENSE) (JUCE 8 open-source terms).

Time-stretching uses [Rubber Band Library](https://breakfastquay.com/rubberband/) (v4.0.0, GPL-2.0-or-later, Particular Programs Ltd).
