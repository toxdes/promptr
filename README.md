# promptr

A GTK4 overlay application that runs `opencode` commands through a
resizable window. Uses layer-shell on wlroots compositors (Sway,
Hyprland, River) and works on X11 as a regular window.

## Contents

- [Installation](#installation)
- [Usage](#usage)
- [Configuration](#configuration)
- [Build](#build)
- [Releases](#releases)
- [License](#license)

## Installation

### Ubuntu / Debian

Download the `.deb` from the [latest release](https://github.com/toxdes/promptr/releases/latest),
then:

```sh
sudo dpkg -i promptr_*.deb
```

### Fedora / RHEL

Download the `.rpm` from the [latest release](https://github.com/toxdes/promptr/releases/latest),
then:

```sh
sudo rpm -i promptr-*.rpm
```

### Arch Linux

Build from latest source:

```sh
yay -S promptr-git
```

Or install the pre-built binary (faster):

```sh
yay -S promptr-bin
```

### Manual install

See [Build](#build) to compile from source.

## Usage

```sh
promptr
```

- Type a query in the **Prompt** field. Shift+Enter inserts a newline.
- Optionally select an **Agent** and a **Model** from the dropdowns.
- Press **Enter** or click **Submit** to execute.
- Output appears in the **Output** area with line numbers and gutter marks.
- Click the gutter to toggle marks. **Copy Marked Lines** copies only
  marked lines.
- Press **Escape** or **Close** to hide the window.
- **Close & Quit** exits. Model and agent are persisted to
  `~/.local/share/promptr/state`.
- Click **Log...** to view the session log.
- Click **Shortcuts...** or press `ctrl+f1` to see all shortcuts.

### Keyboard shortcuts

| Shortcut       | Action                              |
| -------------- | ----------------------------------- |
| `ctrl+k`       | Focus prompt with all text selected |
| `ctrl+shift+c` | Copy marked lines                   |
| `ctrl+q`       | Close window                        |
| `ctrl+shift+q` | Close & Quit                        |
| `ctrl+shift+d` | Open session log                    |
| `ctrl+f1`      | Open shortcuts popup                |
| `esc`          | Hide window (configurable)          |
| `enter`        | Submit prompt                       |
| `shift+enter`  | Newline in prompt                   |

All shortcuts are configurable in `~/.config/promptr/config`.

## Configuration

promptr uses two layers of configuration:

### Compile-time defaults (`config.h`)

Fallback values used when no runtime config exists. Edit this file
and rebuild to change defaults.

### Runtime config (`~/.config/promptr/config`)

Created automatically on first launch. Runtime values take precedence
over `config.h`. Edit and restart.

```ini
[preferences]
width=900
height=700
escape_hides=1
layer_shell=0
notify_on_copy=1
mark_bg_color=#33cc7f
kb_focus_prompt=<Control>k
kb_copy_marked=<Control><Shift>c
kb_close=<Control>q
kb_quit=<Control><Shift>q
kb_log=<Control><Shift>d
kb_shortcuts=<Control>F1
opencode_path=opencode
model_options=opencode-go/deepseek-v4-flash,opencode-go/deepseek-v4-pro,...
agent_options=linux_cmd,None
marked_lines=1
decorated=0
prompt_font_size=0
output_font_size=0
```

## Build

### Install build dependencies

**Debian / Ubuntu:**

```sh
sudo apt install gcc make pkg-config \
  libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev
```

**Arch Linux:**

```sh
sudo pacman -S gcc make pkg-config \
  gtk4 gtksourceview5 gtk4-layer-shell
```

### Compile

```sh
make          # debug build
make release  # optimized build
```

### Install from source

```sh
sudo make install
```

Installs to `/usr/local`. Use `PREFIX` to change the destination:

```sh
sudo make install PREFIX=/usr
```

## Releases

Build `.deb`, `.rpm`, `.AppImage`, and a PKGBUILD for all platforms
with Docker:

```sh
python3 build-all.py
```

For AppImages:

```sh
python3 build-all.py --include-appimage
```

Requires Docker with `buildx`. Output in `dist/`:

```
dist/
  promptr_x.y.z_amd64.deb
  promptr_x.y.z_arm64.deb
  promptr-x.y.z-1.x86_64.rpm
  promptr-x.y.z-1.aarch64.rpm
  promptr-x.y.z-amd64.AppImage
  promptr-x.y.z-arm64.AppImage
  PKGBUILD
```

### Release workflow

1. Bump the version in `VERSION`, commit
2. Tag: `git tag v0.1.4 && git push origin v0.1.4`
3. Build: `python3 build-all.py [--include-appimage]`, we skip AppImage for smaller releases.
4. Create a GitHub release and upload artifacts from `dist/`
5. Push to AUR: `python3 release-aur.py`

## License

MIT
