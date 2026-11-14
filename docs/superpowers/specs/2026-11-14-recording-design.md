# Aligna — Recording design

## Intent

The owner wants **a quick way to record a melody over an existing track** without
leaving Aligna: press record, hear the project, play or sing along, and get a new
track that is already in time with the music. Recording was a v1 non-goal; this
design adds it in the smallest form that serves that use. Discussed and approved in
outline by the owner on 2026-11-14 (the owner left the choices to Claude: "What would
you recommend" → "yes go ahead").

## Goals

- **Record button** in the transport bar (red, labelled "Rec") and shortcut **R**.
  Pressing it starts recording at the playhead; pressing it again, Stop, or Space
  stops. *(Shortcut R is Claude's call; the builder checks it is free.)*
  **Pressing Rec while playing** behaves the same as from stopped: the playhead's
  current position becomes `recordStart`, and playback jumps back by the pre-roll and
  restarts. One code path, one behaviour. *(Claude's call.)*
- **The project plays while recording.** All existing tracks are heard, with their
  usual volume, pan, mute and solo.
- **Each recording becomes a new track** named "Take N" (N = one more than the highest
  existing "Take" number), holding one clip. Recording never writes into existing
  tracks or clips. Adding the take is one undo step named "Record".
- **Pre-roll:** playback starts **one bar** (from the project BPM and time signature)
  before the playhead, and the take's clip starts at the playhead. The pre-roll can be
  set to **1 bar** (default) or **Off**. With the playhead less than one bar from zero,
  pre-roll starts at zero.
- **Timing is corrected automatically:** the clip is shifted earlier by the latency
  the audio driver reports (input + output), plus a **manual offset in ms** set in
  Audio Settings (default 0, may be negative, range −500 to +500 ms). The offset exists
  for drivers that report the wrong latency.
- **The take is a normal source.** It is saved as a WAV file and loaded exactly like an
  imported file (resampled to the project rate once), so move, trim, split, gain,
  stretch, align, undo and export all work on it unchanged.
- **Input choice:** Audio Settings gains input selection: the input device (where the
  OS separates them) and the input channel(s). A take is **mono** from one input
  channel by default, or **stereo** if two channels are selected.
- **Level meter:** a small input level meter in the transport bar, live while recording
  and while Audio Settings is open, so the user can see the input works before
  recording. *(Claude's call: without it, a silent take from a wrong input is only
  noticed afterwards.)*
- **Per-machine settings are remembered:** audio device setup (including the input),
  manual latency offset and pre-roll are saved in the app's settings file and restored
  at launch. They are not stored in the project, because they describe the computer,
  not the song.

## Non-goals

Software input monitoring (the user hears themselves through the interface's direct
monitoring or not at all), metronome / click, count-in beyond the pre-roll, punch-in /
punch-out, loop recording, take lanes or comping, recording into an existing track,
recording several tracks at once, MIDI, input effects, loopback latency calibration
(possible later addition).

## Anti-goals

- **Takes that are audibly late or early** when the driver reports latency correctly.
  For an alignment tool this would make the feature worse than importing.
- **Losing a take.** A crash, a full disk or a device error during recording must keep
  whatever was captured up to that point.
- **Glitches or allocation on the audio thread**, and any change to playback or export
  behaviour when not recording.
- **A microphone permission prompt for users who never record.** macOS must not ask
  for the microphone at launch unless the user has recorded / enabled an input before.

## Constraints

- **Audio thread:** no allocation, locking or file I/O in the device callback. Input
  samples go through a lock-free FIFO to a background writer
  (`juce::AudioFormatWriter::ThreadedWriter` or equivalent).
- **Rates:** the device may run at a different rate from the project (playback already
  resamples in `PlaybackChain`). The WAV is written at the **device rate** and resampled
  to the project rate when it is loaded through `SourcePool`, like any import.
- **WAV format:** 24-bit PCM. *(Claude's call.)*
- **Where takes are saved:**
  - Saved project: `<project folder>/<project name> Takes/Take N.wav` (a numeric suffix
    if the name exists). Inside the project folder, so `ProjectFile` stores it as a
    relative path already.
  - Unsaved project: a per-session folder under the app's data directory (not the OS
    temp directory, which the OS may wipe), e.g.
    `<user app data>/Aligna/Unsaved Takes/<date-time>/`. On the first save
    (Save / Save As), **every** take in that folder is **moved** into the Takes folder
    next to the new project file, and its source path is updated in **every state of
    the undo history** (so a take that is undone, then redone after saving, still points
    at the moved file). If a move fails, the file is copied instead; if that fails,
    saving reports the error and does not write the project.
  - **Nothing deletes takes automatically**: not undo, not "Don't Save", not New /
    Open / Quit. Unsaved-session folders stay until the user deletes them; the README
    says where they are. *(Claude's call: the anti-goal is losing a take; disk use is
    the lesser cost.)*
  - **Save As of an already-saved project** does not move takes: like imported files,
    they keep pointing at the old project's Takes folder (stored as an absolute path).
    *(Claude's call: same as imports today.)*
- **Crash safety:** the background writer rewrites the WAV header with the current
  sizes about once a second (`WavAudioFormatWriter::flush()`), so after a crash the
  file opens in Aligna or any editor with at most the last second missing. There is no
  in-app recovery prompt; the user re-imports the file. *(Claude's call.)*
- **macOS:** the app bundle needs a microphone usage description
  (`MICROPHONE_PERMISSION_ENABLED TRUE` and a `MICROPHONE_PERMISSION_TEXT` in
  `juce_add_gui_app`). The device opens with inputs at launch only if the saved device
  setup has an input enabled; otherwise inputs are opened when the user first presses
  Rec or picks an input in Audio Settings. If permission is denied or there is no
  input, Rec shows a message ("No audio input available. Choose one in Audio
  Settings.") and nothing starts.
- **CI** stays green on Windows, macOS and Linux; new logic gets Catch2 tests like the
  rest of `aligna_core`.

## How timing works (decided)

The take keeps the pre-roll audio and is trimmed non-destructively, so the user can drag
the clip's left edge out to recover it.

1. On Rec, the engine sets the position to `prerollStart` and starts playing. Writing
   of input samples starts in the **same device callback** in which playback of
   `prerollStart` starts.
2. Total latency `L` (in project samples) = driver input latency + driver output
   latency (converted from device to project rate) + any delay inside the playback
   chain (e.g. the resampler) + the manual offset.
3. On stop, the WAV is closed and loaded. The new clip gets `start = recordStart`
   (the playhead when Rec was pressed), `sourceOffset = (recordStart − prerollStart) + L`,
   `length = recordedLength − sourceOffset`. With pre-roll Off, `prerollStart =
   recordStart`.
4. `L` can be negative only through a negative manual offset. If that makes
   `sourceOffset` negative, it is clamped to 0 and the clip `start` moves later by the
   difference, so the audio is still placed where it was played.

The builder must prove the alignment with an **automated loopback test**: drive the
chain with a fake device whose input is its own output delayed by a known number of
samples and reporting that latency; record a click over a project containing the same
click; the take must line up with the project click within ±1 sample, at equal and
at different device/project rates.

## While recording (decided)

- The transport shows "Recording" and the elapsed take time; the timeline shows a red
  bar on a placeholder lane below the last track, growing from `recordStart` to the
  playhead. The waveform appears when recording stops. *(Claude's call: a live
  waveform is more work than the use needs.)*
- **Edits are blocked while recording:** timeline editing, undo/redo, import, open,
  new, BPM / time-signature changes and Audio Settings changes are disabled or ignored.
  Seeking (clicking the ruler) is ignored. *(Claude's call: keeps the take's timing
  and the project unambiguous.)*
- On stop the playhead behaves like Stop today (returns to where playback started,
  i.e. `prerollStart`). *(Claude's call; the builder may return it to `recordStart`
  if that feels better in testing.)*
- **Failure during recording** (device stops or changes, disk full, write error):
  recording stops, everything written so far becomes the take as normal, and a message
  says what happened. A take shorter than 0.1 s, or a WAV that cannot be loaded, is
  discarded with a message.
- Quitting or closing the project while recording first stops the recording and adds
  the take, then the usual "save changes?" question applies.

## Approaches

| Approach | Trade-off | Decision |
|---|---|---|
| **Stream to disk while recording** (audio thread → lock-free FIFO → background WAV writer) | Flat memory use, take survives a crash, standard JUCE pattern. Slightly more plumbing. | **Chosen** |
| Record into memory, write the WAV on stop | Simpler, but RAM grows with take length and a crash loses the whole take (anti-goal). | Rejected |

Latency handling options considered:

| Option | Trade-off | Decision |
|---|---|---|
| Driver-reported latency only | No setup, but some drivers (USB mics, Bluetooth) report it wrongly, with no fix. | Rejected |
| **Driver latency + manual offset (ms)** | No setup for most users, a simple fix for bad drivers. | **Chosen** |
| Plus loopback calibration | Most accurate, but more to build and needs a cable. | Later, if needed |
| Rely on auto-align | Does not work: cross-correlation matches recordings of the same audio, and a melody does not resemble its backing track. | Rejected |

## Left to the builder

- Class shapes and where the recorder lives (e.g. a `Recorder` in `src/engine/` hooked
  into `PlaybackChain`'s device callback, and a `recordTake(...)` in `AppController`).
- How the playback-chain delay is measured (the loopback test must pass).
- Layout of the Rec button, meter and the new Recording section in Audio Settings,
  and how the app settings file is set up (`juce::ApplicationProperties` is fine).
- How "edits blocked" is implemented (disabled controls vs. ignored commands).
- README updates: feature list, shortcut table and a short "Recording a take" how-to.
