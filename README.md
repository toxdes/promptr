# promptr

A GTK4 overlay application that runs `opencode` commands through a
resizable window.  Uses layer-shell on wlroots compositors (Sway,
Hyprland, River) and works on X11 as a regular window.

## Features

- Multi-line prompt input with monospace font
- Real-time command preview as you type or change dropdown selections
- Agent and Model dropdown pickers with configurable options
- Line numbers in the output textarea (GtkSourceView)
- Gutter marks: click to toggle, Copy button copies marked lines only
- Marked-line summary label ("Marked lines: all", "Marked lines: 1,2,30")
- Async subprocess execution with process-group cancellation
- Desktop notification on copy (configurable)
- Session-based log files and popup (\"Log...\" button)
- Status bar with context-sensitive hints on hover
- Keyboard shortcuts table popup (\"Shortcuts...\" button)
- Single-instance: re-invoking shows the existing window instantly
- Per-command temp directories — cleaned up on exit
- Error detection: stderr displayed on non-zero exit codes
- Configurable via runtime config file or compile-time defaults

## Dependencies

| Dependency | Debian / Ubuntu (trixie+) | Arch Linux |
|---|---|---|
| GTK 4 | `libgtk-4-dev` | `gtk4` |
| GtkSourceView 5 | `libgtksourceview-5-dev` | `gtksourceview5` |
| Layer Shell (gtk4) | `libgtk4-layer-shell-dev` | `gtk4-layer-shell` |

Build tools: `gcc` (or `clang`), `make`, `pkg-config`.

### Installing build dependencies

**Debian / Ubuntu (trixie or later):**
```sh
sudo apt install libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev
```

**Arch Linux:**
```sh
sudo pacman -S gtk4 gtksourceview5 gtk4-layer-shell
```

## Building

```sh
make
```

Optionally with clang:
```sh
make CC=clang
```

## Installing

```sh
sudo make install
```

Installs to `/usr/local`.  Use `PREFIX` to change the destination:

```sh
sudo make install PREFIX=/usr
```

Installed files:

```
<prefix>/bin/promptr
<prefix>/share/icons/hicolor/scalable/apps/promptr.svg
<prefix>/share/applications/com.toxdes.promptr.desktop
```

Uninstall:

```sh
sudo make uninstall
```

## Configuration

promptr uses two layers of configuration:

### Compile-time defaults (`config.h`)

Fallback values used when no runtime config exists.  Edit this file
and rebuild to change defaults.

### Runtime config (`~/.config/promptr/config`)

Created automatically on first launch with all options commented.
Runtime values take precedence over `config.h`.  Edit and restart.

```ini
[preferences]
# Window size in pixels
width=900
height=700

# Escape key hides window (0 or 1)
escape_hides=1

# Layer-shell overlay on wlroots compositors (0 or 1)
layer_shell=0

# Desktop notification on copy (0 or 1)
notify_on_copy=1

# Gutter mark color in hex
mark_bg_color=#33cc7f

# Keyboard shortcuts (GTK accelerator format)
kb_focus_prompt=<Control>k
kb_copy_marked=<Control><Shift>c
kb_close=<Control>q
kb_quit=<Control><Shift>q
kb_log=<Control><Shift>d
kb_shortcuts=<Control>F1

# Path to the opencode binary
opencode_path=opencode

# Model dropdown options (comma-separated)
model_options=opencode-go/deepseek-v4-flash,opencode-go/deepseek-v4-pro,opencode/big-pickle,opencode/deepseek-v4-flash-free,None

# Agent dropdown options (comma-separated)
agent_options=linux_cmd,None

# Default marked lines (0=all, -1=none, or comma-separated 1-based)
marked_lines=1

# Window decorations when layer-shell is disabled (0 or 1)
decorated=0

# Font size in pt for prompt text (0 = system default)
prompt_font_size=0

# Font size in pt for output text (0 = system default)
output_font_size=0
```

## Usage

```sh
./promptr
```

- Type a query in the **Prompt** field.  Shift+Enter inserts a newline.
- Optionally select an **Agent** and a **Model** from the dropdowns.
- Press **Enter** or click **Submit** to execute.
- Output appears in the **Output** area with line numbers and gutter marks.
- Click the gutter to toggle marks.  Copy copies only marked lines.
- Press **Escape** or click **Close** to hide the window.  The process
  stays resident; re-launching shows the window instantly.
- Click **Close & Quit** to exit.  Last selected model and agent are
  persisted to `~/.local/share/promptr/state`.
- Click **Log...** to view the session log (submits, timings, errors).
- Click **Shortcuts...** or press `Ctrl+F1` to see all keyboard shortcuts.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `ctrl+k` | Focus prompt with all text selected |
| `ctrl+shift+c` | Copy marked lines |
| `ctrl+q` | Close window |
| `ctrl+shift+q` | Close & Quit |
| `ctrl+shift+d` | Open session log |
| `ctrl+f1` | Open keyboard shortcuts popup |
| `esc` | Hide window (configurable) |
| `enter` | Submit prompt |
| `shift+enter` | Newline in prompt |

All shortcuts are configurable in `~/.config/promptr/config`.

## Packaging

Build `.deb`, `.rpm`, `.AppImage`, and a PKGBUILD for all platforms
with Docker:

```sh
python3 build-all.py
```

For AppImages (adds ~100 MB per arch):
```sh
python3 build-all.py --include-appimage
```

Requires Docker with buildx.  Output in `dist/`:

```
dist/
  promptr_0.1.4_amd64.deb
  promptr_0.1.4_arm64.deb
  promptr-0.1.4-1.x86_64.rpm
  promptr-0.1.4-1.aarch64.rpm
  promptr-0.1.4-amd64.AppImage
  promptr-0.1.4-arm64.AppImage
  PKGBUILD
```

## AUR (Arch Linux)

```sh
yay -S promptr-git       # build from latest git
yay -S promptr-bin       # pre-built binary (faster)
```

## Release flow

1. Bump the version in `VERSION`, commit
2. Tag the release: `git tag v0.1.4 && git push origin v0.1.4`
3. Build packages: `python3 build-all.py [--include-appimage]`
4. Create a GitHub release and upload artifacts from `dist/`
5. Push to AUR: `python3 release-aur.py`

The `promptr-git` AUR package auto-updates via `git describe --tags` —
it only needs a re-push when dependencies or build steps change.

## License

MIT
