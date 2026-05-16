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
- Marked-line summary label ("Marked: all", "Marked: 1,2,30")
- Async subprocess execution with process-group cancellation
- Desktop notification on copy (configurable)
- Keyboard shortcuts (Ctrl+K focus, Ctrl+Shift+C copy, Ctrl+Q quit)
- Single-instance: re-invoking shows the existing window instantly
- Per-command temp directories -- cleaned up on exit
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
<prefix>/share/applications/promptr.desktop
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
width=800
height=600

# Escape key hides window (0 or 1)
escape_hides=1

# Layer-shell overlay on wlroots compositors (0 or 1)
layer_shell=1

# Desktop notification on copy (0 or 1)
notify_on_copy=1

# Gutter mark color in hex
mark_bg_color=#33cc7f

# Keyboard shortcuts (GTK accelerator format)
kb_focus_prompt=<Control>k
kb_copy_marked=<Control><Shift>c
kb_quit=<Control>q

# Model dropdown options (comma-separated)
model_options=opencode/big-pickle,opencode/deepseek-v4-pro,None

# Agent dropdown options (comma-separated)
agent_options=linux_cmd,None

# Default marked lines (0=all, -1=none, or comma-separated 1-based)
marked_lines=0
```

## Usage

```sh
./promptr
```

- Type a query in the **Prompt** field.  Shift+Enter inserts a newline.
- Optionally select an **Agent** and a **Model** from the dropdowns.
- The **Command** preview updates in real time, showing exactly what
  will run.
- Press **Enter** or click **Submit** to execute.
- Output appears in the **Output** area with line numbers and gutter marks.
- Click the gutter to toggle marks.  Copy copies only marked lines.
- Press **Escape** or click **Close** to hide the window.  The process
  stays resident; re-launching shows the window instantly.
- Click **Close & Quit** to exit.  Last selected model and agent are
  persisted to `~/.local/share/promptr/state`.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+K` | Focus prompt with all text selected |
| `Ctrl+Shift+C` | Copy marked lines |
| `Ctrl+Q` | Close & Quit |
| `Escape` | Hide window (configurable) |

## Packaging

Build `.deb`, `.rpm`, and a PKGBUILD for all platforms with Docker:

```sh
python3 build-all.py
```

Requires Docker with buildx.  Output in `dist/`:

```
dist/
  promptr_0.1.0_amd64.deb
  promptr_0.1.0_arm64.deb
  promptr-0.1.0-1.x86_64.rpm
  promptr-0.1.0-1.aarch64.rpm
  PKGBUILD
```

Version is read from the `VERSION` file at the repo root.  Bump it and
tag before building packages for a release.

## License

MIT
