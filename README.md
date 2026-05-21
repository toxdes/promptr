# promptr
<p align="center">
  <img width="585" height="422" alt="image" src="https://github.com/user-attachments/assets/f3b0e059-9a01-42e5-9cde-cb901a60fabc" />
</p>

A GTK4 overlay application that runs `opencode` commands through a
resizable window. Supports `layer-shell` on wlroots compositors. 

## Contents

- [Installation](#installation)
- [Usage](#usage)
- [Configuration](#configuration)
- [Build](#build)
- [Releases](#releases)
- [License](#license)

## Installation

### Ubuntu / Debian

Set up the apt repository:

```sh
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://packages.toxdes.com/apt/pubkey.gpg \
  | sudo tee /etc/apt/keyrings/promptr.asc > /dev/null
sudo chmod a+r /etc/apt/keyrings/promptr.asc

sudo tee /etc/apt/sources.list.d/promptr.sources <<'EOF'
Types: deb
URIs: https://packages.toxdes.com/apt
Suites: stable
Components: main
Signed-By: /etc/apt/keyrings/promptr.asc
EOF

sudo apt update
sudo apt install promptr
```

### Fedora / RHEL

Set up the DNF repository:

```sh
sudo rpm --import https://packages.toxdes.com/rpm/pubkey.gpg

sudo tee /etc/yum.repos.d/promptr.repo <<'EOF'
[promptr]
name=Promptr
baseurl=https://packages.toxdes.com/rpm
gpgcheck=1
gpgkey=https://packages.toxdes.com/rpm/pubkey.gpg
enabled=1
EOF

sudo dnf install promptr
```

### Arch Linux

promptr is available in the AUR. Install with an AUR helper:

```sh
yay -S promptr-bin
```

Or build from latest source:

```sh
yay -S promptr-git
```

### AppImage

For now, `.AppImage` packages are available in select releases. Check the [releases page](https://github.com/toxdes/promptr/releases).

Once you have the `promptr-*.AppImage`, run:
```sh
chmod +x promptr-*.AppImage
./promptr-*.AppImage
```

### Manual install

See [Build](#build) to compile from source.

## Usage

```sh
promptr
```

- Type a query in the **Prompt** field. Enter inserts a newline.
- Optionally select an **Agent** and a **Model** from the dropdowns.
- Press **Ctrl+Enter** or click **Submit** to execute.
- Output appears in the **Output** area with line numbers and gutter marks.
- Click the gutter to toggle marks. **Copy Marked Lines** copies only
  marked lines.
- Press **Escape** during a running query to interrupt (press again to confirm).
- **Close** hides the window. **Close & Quit** exits. Model and agent are
  persisted to `~/.local/share/promptr/state`.

### Keyboard shortcuts

| Shortcut       | Action                              |
| -------------- | ----------------------------------- |
| `ctrl+enter`   | Submit prompt                       |
| `enter`        | Newline in prompt                   |
| `ctrl+k`       | Focus prompt with all text selected |
| `ctrl+shift+c` | Copy marked lines                   |
| `ctrl+q`       | Close window                        |
| `ctrl+shift+q` | Close & Quit                        |
| `ctrl+shift+d` | Open session log                    |
| `ctrl+f1`      | Open shortcuts popup                |
| `esc`          | Cancel running query (double-escape)|

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
layer_shell=0
notify_on_copy=1
mark_bg_color=#33cc7f
kb_focus_prompt=<Control>k
kb_copy_marked=<Control><Shift>c
kb_close=<Control>q
kb_quit=<Control><Shift>q
kb_log=<Control><Shift>d
kb_shortcuts=<Control>F1
kb_submit=<Control>Return
kb_cancel=
command_expanded=0
gsk_renderer=cairo
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

Pre-built `.deb` and `.rpm` packages are available on the
[GitHub releases page](https://github.com/toxdes/promptr/releases/latest).

For building packages and the full release workflow, see [releases.md](releases.md).

## License

MIT
