# Windows Installer, Delete Track and Delete Menus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a per-user Windows installer with `.aligna` double-click-to-open, and make deleting tracks and clips available from the interface.

**Architecture:** Pure helpers (`removeTrackById`, `projectFileFromArgs`) live in `aligna_core` with Catch2 tests; the UI calls them through `EditHistory::perform`. The installer is an Inno Setup script compiled and smoke-tested by the Windows CI job.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2, Inno Setup 6, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-14-installer-and-delete-design.md`

## Global Constraints

- Every edit goes through `app.history().perform(name, fn)`; nothing edits while `app.isRecording()` or `app.isLoading()`.
- Undo step names: "Delete Track", "Delete", "Split".
- Installer output name: `Aligna-windows-setup.exe`. Install dir `{localappdata}\Programs\Aligna`, `PrivilegesRequired=lowest`.
- Installer version is read from `CMAKE_PROJECT_VERSION` in `build/CMakeCache.txt`.
- No model identifiers or session details in code or docs.

## Review Focus

- Deleting the track whose header was clicked destroys that header: the delete must run after the callback returns.
- Deleting a track while a clip on it is the align reference, is selected, or has the tempo dialog open: no stale ids, no crash.
- A launch argument that is a relative path, quoted, missing, or not a project: the app starts and reports, never crashes.
- Right-click on an unselected clip while others are selected: only the clicked clip is acted on.
- Running a newer installer over an older install: same folder, one entry in "Installed apps".

---

### Task 1: Core helpers

**Files:** Modify `src/edit/ClipOps.{h,cpp}`, `src/io/FileNaming.{h,cpp}`; Test `tests/ClipOpsTest.cpp`, `tests/FileNamingTest.cpp`

**Interfaces — Produces:**
- `bool removeTrackById(Project&, const juce::String& trackId);` — false if no such track.
- `juce::File projectFileFromArgs(const juce::StringArray& args, const juce::File& workingDir);` — the first argument that does not start with `-`, unquoted, resolved against `workingDir` when relative; `juce::File()` when there is none.

- [ ] Tests: removing by id removes that track and its clips only; unknown id returns false. Args: `{}` → none; `{"-psn_0_1"}` → none; `{"C:/x/a.aligna"}`/absolute → that file; `{"sub/a.aligna"}` → `workingDir/sub/a.aligna`; `{"\"/x/my song.aligna\""}` → unquoted.
- [ ] Implement, run `ctest --test-dir build --output-on-failure`, commit.

### Task 2: Open a project at launch

**Files:** Modify `src/app/Main.cpp`, `src/ui/MainComponent.{h,cpp}`

- [ ] Make `MainComponent::openFile(const juce::File&)` public.
- [ ] In `initialise`, after the window exists: `projectFileFromArgs(getCommandLineParameterArray(), File::getCurrentWorkingDirectory())`; if not `File()`, `callAsync` → `openFile` (SafePointer to the main component).
- [ ] Verify under Xvfb: launch with a saved project path, screenshot shows its tracks; launch with a missing path shows "Could not open project". Commit.

### Task 3: Delete track in the header

**Files:** Modify `src/ui/TrackHeader.{h,cpp}`

- [ ] Add `juce::TextButton deleteButton` ("×", tooltip "Delete track") at the right end of the M/S row; `mouseDown` (also as listener on the name label) shows a popup with "Delete track" on a popup-menu click.
- [ ] Both call `requestDelete()`: `MessageManager::callAsync` with a `juce::WeakReference<AppController>` and the track id → if not recording/loading, `history().perform("Delete Track", removeTrackById)`.
- [ ] Verify under Xvfb: click ×, track disappears, Edit menu shows "Undo Delete Track", undo restores. Commit.

### Task 4: Clip menu and Edit menu

**Files:** Modify `src/ui/TimelineView.{h,cpp}`, `src/ui/MainComponent.{h,cpp}`

- [ ] `splitSelected()` / `deleteSelected()` become public; add `bool hasSelection() const`.
- [ ] Clip popup: select the clicked clip if it is not selected; add separator, "Split at playhead" (id 3), "Delete" (id 4) with shortcut text.
- [ ] Edit menu: `menuSplit`, `menuDelete` after Align, enabled when `!busy()` and a clip is selected; menu refreshes when the selection changes (`menuItemsChanged` is already called on project change; the menu is built on open, so enabled state is current).
- [ ] Verify under Xvfb. Commit.

### Task 5: Installer and CI

**Files:** Create `installer/windows/Aligna.iss`; Modify `.github/workflows/build.yml`

- [ ] `.iss`: fixed `AppId`, `#define`s `AppVersion`, `SourceExe`, `LicenseDir`, `RepoDir` passed with `/D`; `[Files]` exe + 4 license files; `[Icons]` Start menu + optional desktop; `[Registry]` HKCU `Software\Classes\.aligna` → `Aligna.Project` with `shell\open\command` = `"{app}\Aligna.exe" "%1"`; `ChangesAssociations=yes`; `[Run]` optional launch.
- [ ] Workflow (Windows only): ensure Inno Setup (install with choco if `ISCC.exe` is absent), compile, then smoke test: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`, assert `Aligna.exe` and the registry command exist, run the uninstaller silently, assert the folder is gone. Upload `Aligna-windows-setup.exe` as its own artifact; the release job already publishes `dist/*`.
- [ ] Push and confirm the Windows job is green.

### Task 6: Install updates

**Files:** Create `src/core/UpdateCheck.{h,cpp}`, `tests/UpdateCheckTest.cpp`, `src/ui/Updater.{h,cpp}`; Modify `src/ui/MainComponent.{h,cpp}`, `CMakeLists.txt`

**Interfaces — Produces:** `int compareVersions(a, b)`, `std::optional<ReleaseInfo> parseLatestRelease(json)` (core, tested); `class Updater { Updater(juce::Component& parent, std::function<void(std::function<void()>)> confirmQuit); void check(); void close(); bool busy() const; }`.

- [ ] Core + tests (version ordering, installer asset, refused links, non-release JSON).
- [ ] `Updater`: check and download on `ThreadWithProgressWindow`s; verify size and SHA-256 (`juce_cryptography`); `confirmQuit` then `startAsProcess("/SILENT")` and quit. Install only on Windows; elsewhere open the release page.
- [ ] Help menu "Check for Updates...", disabled while recording/loading. Commit.

### Task 7: README

- [ ] Add an "Install" section (Windows installer, portable zips, unknown-publisher note), the track delete, the new menu items; update the shortcut table. Commit.
