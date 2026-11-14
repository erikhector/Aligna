# Aligna — Windows installer, delete track, delete from menus (short)

## Intent

The owner uses Aligna on Windows and wants to install it like any other program instead
of unzipping a bare `.exe`, to delete tracks from the workspace, and to delete the parts
left after splitting a clip. Approved by the owner on 2026-11-14 ("Yes that would be
nice. I use Windows").

## Goals

- **Windows installer:** every build produces `Aligna-windows-setup.exe`; a tagged
  release publishes it next to the existing zip (the zip stays as the portable option).
  The installer:
  - installs for the current user only, with no administrator prompt
    (`%LOCALAPPDATA%\Programs\Aligna`);
  - adds a Start menu entry and, if ticked, a desktop shortcut;
  - registers an uninstaller under Windows "Installed apps";
  - registers `.aligna` so that double-clicking a project opens it in Aligna;
  - installs the license files beside the program;
  - upgrades in place when a newer installer is run (fixed AppId).
- **Open a project at launch:** `Aligna.exe <path to .aligna>` opens that project after
  the window appears. A path that does not exist or cannot be read shows the usual
  "Could not open project" message; the app still starts with an empty project.
- **Delete track:** each track header has a small "×" button, and right-clicking the
  header shows "Delete track". The track and all its clips are removed as one undo step
  named "Delete Track". No confirmation question: Ctrl+Z brings it back.
- **Delete and split from menus:** deleting a clip (a "part") already works with
  Delete/Backspace. It becomes discoverable:
  - the clip right-click menu gains "Split at playhead" and "Delete";
  - the Edit menu gains "Split at Playhead" (S) and "Delete" (Del), enabled when at
    least one clip is selected;
  - right-clicking a clip that is not selected selects it (alone) first, so the menu
    acts on what was clicked; right-clicking a selected clip keeps the selection.
- **Install updates (added by the owner on 2026-11-14, "Can we add 'install updates'
  to the gui also?"):** a Help menu with "Check for Updates...". It asks GitHub for
  the latest release and compares it with the running version.
  - Up to date: says so.
  - Newer, on Windows, and the release has `Aligna-windows-setup.exe`: offers
    Install / Release notes / Later. Install downloads the installer with a progress
    window (cancellable), checks its size and SHA-256 against what GitHub reported,
    asks about unsaved changes, starts the installer silently and quits. The installer
    reopens Aligna when it is done.
  - Newer, otherwise (macOS, Linux, or no installer in the release): offers to open
    the release page.
  - No connection or an unusable answer: says the check failed and offers the
    releases page.
- README documents all of the above.

## Non-goals

- macOS `.dmg` and Linux `.deb` (the owner is on Windows; they can follow).
- Code signing. Windows will still show the "unknown publisher" warning the first time.
- An application icon, checking for updates automatically (Aligna only contacts the
  network when the user chooses Check for Updates), single-instance behaviour (a second double-click
  opens a second Aligna window, as starting the program twice does today).
- Deleting audio files from disk. Aligna never deletes source or take files.

## Anti-goals

- Requiring administrator rights to install.
- Any delete that cannot be undone.
- Running a downloaded file that is not this project's installer: download links
  outside `github.com/erikhector/Aligna/releases/download/` are refused, and a
  download whose size or hash does not match is deleted, not run.
- Deleting or editing while recording or while audio is loading (same guards as every
  other edit).

## Approaches

- **Inno Setup script built in CI (chosen):** one small `.iss` file, per-user install
  and file association built in, widely used for open-source Windows programs.
- **CPack + NSIS:** stays inside CMake, but per-user install and file association need
  hand-written NSIS snippets; more moving parts for the same result.
- **MSI (WiX):** best for company-managed deployment, heavy for a single-user tool.

## Constraints

- The installer can only be built and run on Windows. It is verified by the GitHub
  Actions Windows job (build the installer, install silently, check the installed files
  and the `.aligna` registration, uninstall silently), not on the development machine.
- The installer version comes from the CMake project version; no second place to bump.
- A track header must not be destroyed from inside its own button or menu callback: the
  delete is deferred to the message loop.

## Left to the builder

Exact button placement in the header, menu ordering, and the wording of README text.
