# Aligna

Aligna is an open-source digital audio workstation (JUCE / C++20) focused on one job: **aligning audio tracks**.
Import several recordings of the same performance, line them up on a timeline (by hand, with snapping and nudging,
or automatically by cross-correlation), listen, and export a mixed WAV. Source audio files are never modified.

## Features

- Import WAV, AIFF, FLAC and MP3 (menu, Ctrl/Cmd+I, or drag and drop); each file becomes a track. Sources are resampled to the project rate once at import and are never modified.
- Timeline with waveforms, zoom, beat grid and snapping; move, trim, split, delete and nudge clips (the later clip covers the earlier one where they overlap).
- Per-track volume, pan, mute and solo; transport with play/stop, BPM and grid settings.
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

- **macOS:** right-click `Aligna.app` and choose Open, or run `xattr -dr com.apple.quarantine Aligna.app`.
- **Windows:** in the SmartScreen dialog choose "More info", then "Run anyway".
- **Linux:** extract the tarball and run `./Aligna`.

## License

AGPL-3.0, see [LICENSE](LICENSE) (JUCE 8 open-source terms).
