# Aligna

Aligna is an open-source digital audio workstation (JUCE / C++20) focused on one job: **aligning audio tracks**.
Import several recordings of the same performance, line them up on a timeline (by hand, with snapping and nudging,
or automatically by cross-correlation), listen, and export a mixed WAV. Source audio files are never modified.

Status: early development (v0.1.0 skeleton).

## Building

Requirements: CMake 3.22+, a C++20 compiler, and (Linux only) the JUCE system dependencies:

```sh
sudo apt-get install libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
  libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
```

JUCE and Catch2 are fetched automatically by CMake.

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
