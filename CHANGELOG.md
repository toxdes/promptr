# Changelog

## Unreleased

## 0.3.0 - 2026-11-26

### Added

- OpenRouter and OpenCode provider plugins, with searchable model catalogs loaded on demand.
- Modal Preferences for provider profiles, connection tests, editor fonts, line numbers, and shortcuts. Provider secrets use the system keyring.
- Per-conversation Markdown agent context and optional project directories.

### Changed

- Native GTK tabs support inline renaming. Conversation history loads in pages, and replies continue when switching tabs.
- Release packages include both native provider executables. Yesb builds the packages and supports stable AUR builds from a tagged source release.

### Fixed

- Provider startup and cancellation have time limits. Connection errors show scrollable details.
- Model selection stays responsive when OpenRouter has no API key.
- OpenCode 2 receives the configured agent context through a private server. Connection tests default to `opencode/big-pickle` when no model is set.

## 0.2.3 - 2026-06-08

### Changed

- Start with equally sized panes and hide Agent and Model labels in narrow panes.

### Fixed

- Show a status indicator and dialog when a command returns no output.

## 0.2.2 - 2026-06-04

### Added

- Keyboard shortcuts to cycle through agents and models.
- Show the tab name in the window title and reveal truncated picker values on hover.

### Fixed

- Apply the configured layout to new tabs and dock a popped-out answer when closing it.
- Explain how to install or configure OpenCode when it cannot start.

## 0.2.1 - 2026-06-04

### Fixed

- Cancel running subprocesses when closing tabs or the app.
- Fix crashes around popped-out answers, the log window, and delayed UI callbacks.

## 0.2.0 - 2026-06-02

### Added

- Independent tabs with saved conversations and a native menu bar.
- Separate debug and release builds with isolated state.

### Fixed

- Preserve the layout when replacing the last tab and close popups when quitting.

## 0.1.10 - 2026-05-24

### Added

- Numbered question and answer history for follow-up prompts.

## 0.1.9 - 2026-05-23

### Added

- Switch pane layouts, pop out the answer, and edit prompts in a source view.
- APT and RPM repository publishing and prebuilt AUR tarballs.

## 0.1.8 - 2026-05-20

### Added

- A Follow up checkbox to continue the previous session.

## 0.1.7 - 2026-05-20

### Added

- `ctrl+enter` to submit and two presses of `esc` to cancel.

### Fixed

- Restore focus after requests and avoid waiting on a cancelled subprocess.

## 0.1.6 - 2026-05-19

### Changed

- Make the GTK renderer configurable and default to Cairo.

## 0.1.5 - 2026-05-17

### Added

- AUR publishing automation.

### Fixed

- Remove D-Bus activation from the desktop launcher.

## 0.1.4 - 2026-05-17

### Added

- AppImages for x86-64 and ARM64, a session log, and a shortcut reference.

### Changed

- Launch in a regular window by default.

## 0.1.3 - 2026-05-16

### Added

- Configurable window decorations and configuration migration.

## 0.1.2 - 2026-05-16

### Changed

- Improve the status line and show the app version.

### Fixed

- Exclude stale artifacts from cross-platform Docker builds.

## 0.1.1 - 2026-05-16

### Added

- Configure the OpenCode executable path.

## 0.1.0 - 2026-05-16

Changelog not maintained.
