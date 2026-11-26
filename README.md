# promptr
<p align="center">
  <img width="585" height="422" alt="image" src="https://github.com/user-attachments/assets/f3b0e059-9a01-42e5-9cde-cb901a60fabc" />
</p>

A native GTK4 chat frontend with independent provider plugins. Each tab has its
own conversation, Markdown agent context and optional project directory.
Promptr saves portable history and supports switching providers mid-conversation.
OpenCode CLI and OpenRouter adapters are included. Layer-shell is supported on
wlroots compositors.

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
Architectures: amd64 arm64
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

Build the latest tagged release from source:

```sh
yay -S promptr
```

Or install the prebuilt release:

```sh
yay -S promptr-bin
```

Or build the latest development source:

```sh
yay -S promptr-git
```

### AppImage

Download the [x86-64 AppImage](https://packages.toxdes.com/releases/promptr-0.3.0-amd64.AppImage)
or the [ARM64 AppImage](https://packages.toxdes.com/releases/promptr-0.3.0-arm64.AppImage).

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

OpenRouter and OpenCode profiles ship with Promptr. New installations start with
OpenRouter selected and its advertised models in the model picker. Model
discovery refreshes that list automatically in the background for the selected
provider. Unused providers are not started. Switching providers updates the
model choices without discarding portable conversation history.

1. Enter a question and press `ctrl+enter`. If OpenRouter has no API key, the
   error offers **Preferences** for that provider. Open **Edit → Preferences** to change settings.
2. In Preferences, open **Providers** and choose a profile. Select OpenRouter and
   enter its API key, or select OpenCode to use its existing login.
   **Test connection** sends a request and may use quota. Click **Apply** after testing; changing connection settings
   requires another test. Use **Add provider** for additional profiles.
   Executable paths, custom arguments, and manual model IDs are under
   **Advanced**. Multiple profiles can use the same plugin with different
   accounts or endpoints. Tests show elapsed time and can be cancelled.
   Permanent OpenCode authentication or quota errors are reported immediately;
   OpenCode connection tests limit model requests to 50 seconds.
   OpenCode 2 runs with a private server so each request receives its agent context.
   OpenCode tests use `opencode/big-pickle` when no default model is selected.
3. Select a provider from **Conversation → Provider** and a model from the picker.
   Open the model picker and type a fragment such as `free` to filter model IDs.
   Agent and Model pickers are disabled when `None` is their only choice.
   **Conversation → Model → Enter model ID** accepts a manual choice. `None` uses the
   profile/backend default. Manifest models remain available if discovery fails.
4. Choose an agent from `~/.config/promptr/agents/*.md`, or use
   **Conversation → Agent → Choose Markdown file** for Markdown stored elsewhere. The complete
   document is delivered as context. **Conversation → Project → Choose directory** supplies a
   project directory; **Clear directory** removes that association.
5. Enable **Follow up** to continue the saved conversation. With Follow up
   unchecked, the next submission discards the previous conversation and starts
   fresh in the same tab. A missing API key does not discard saved history.

Replies continue and save their output when you switch tabs or hide the window.
Detached output docks when you switch tabs.

Double-click unused space in the tab strip to open a tab. Double-clicks in the
editors keep their normal text-selection behavior. The output shows the newest
answer first, followed by older `Q.n: …` / `A: …` blocks in reverse turn order.

Closing a running tab asks whether to keep it open or cancel and close. Partial
output is preserved. Closed conversations remain saved across restarts:
**File → Saved conversations** restores or explicitly deletes them, and `ctrl+shift+t`
restores the most recently closed tab. Quitting cancels pending work and waits
for cleanup before exiting.

Large transcripts initially show a bounded recent page. **Conversation → Load older messages** moves
through earlier content and **Latest messages** returns to the newest page. Paging the
view does not truncate the history sent to a provider. A conversation too large
for the IPC/backend limits produces an error instead of silently dropping turns.

Click the output gutter to mark lines; **Copy Marked Lines** copies the marked
selection. **Close window** hides the window; **Quit** exits.

### Providers and credentials

API keys entered into provider settings use the desktop's system Secret Service
through libsecret. An available, unlocked keyring is required to save entered
secrets; there is no plaintext fallback. Profile files contain only nonsecret
settings and keyring references. Providers may also use their own login or
supported environment variables: the OpenCode adapter uses OpenCode's login,
and OpenRouter accepts `OPENROUTER_API_KEY` when its key field is left blank.

Promptr does not perform backend tools, web search or fetching itself. Those
features are available through providers/backends that implement them. OpenRouter
is currently a text API adapter and does not implement a tool loop or project
file access. OpenCode owns its tools, permissions and optional backend sessions.

The public [provider protocol](lib/promptr-protocol/PROTOCOL.md) documents
versioned JSON-RPC over stdin/stdout, manifests, settings schemas and lifecycle
rules. Install a third-party plugin under
`~/.config/promptr/plugins/<name>/plugin.json`, with its executable alongside it.
Plugins can be implemented in any language. No HTTP listener is required between
Promptr and a plugin; adapters choose their own backend transport.

### Storage and migration

Threads use small `.meta` files plus append-only `.journal` transcripts under
`~/.local/share/promptr/tabs/`. Index files are disposable and can be rebuilt.
Drafts and opaque provider state are stored separately. The session file tracks
open-tab order and selection. Closing or unloading a conversation never deletes
project files or backend sessions.

Legacy `.conf`/`.txt` histories are imported when viewed or submitted. Migration
retains the original files and preserves recoverable history, drafts and
selections. A missing legacy agent needs to be selected from a Markdown file;
a legacy provider name needs an appropriate tested profile. Failed/interrupted
replies are retained and never automatically resubmitted.

Debug builds store settings and history under `promptr-debug`.
Promptr finds bundled plugins beside the executable in a source checkout or under
the installation prefix. Set `PROMPTR_PLUGIN_DIR` to use another plugin directory.

### Keyboard shortcuts

| Shortcut       | Action                              |
| -------------- | ----------------------------------- |
| `ctrl+enter`   | Submit prompt                       |
| `enter`        | Newline in prompt                   |
| `ctrl+k`       | Focus prompt with all text selected |
| `ctrl+b`       | Toggle horizontal / vertical layout |
| `ctrl+o`       | Pop out output into its own window  |
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
over `config.h`. Edit and restart. Provider connection settings belong in
**Preferences**. The legacy `agent_options` and `model_options` preferences
are retained for compatibility; agent files and provider profiles now supply
those choices. Set `provider` to a profile UUID or plugin name to choose a new-tab default.
The shipped default is `openrouter`; an empty value also uses that default.

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
marked_lines=1
decorated=0
query_font=Monospace 11
answer_font=Monospace 11
```

## Build

### Install build dependencies

**Debian / Ubuntu:**

```sh
sudo apt install gcc make pkg-config \
  libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev \
  libcurl4-openssl-dev libjson-glib-dev libsecret-1-dev
```

**Arch Linux:**

```sh
sudo pacman -S gcc make pkg-config \
  gtk4 gtksourceview5 gtk4-layer-shell curl json-glib libsecret
```

### Compile

```sh
make debug    # debug GUI and both providers
make test     # offline core checks, no GUI or paid requests
make config   # compare generated preference defaults; does not launch GTK
# An optimized packaging build is a separate, explicit release step.
```

Additional checks:

```sh
python3 tests/run.py --sanitize
python3 tests/test_adapters.py   # localhost HTTP fixture and fake CLI; no external API
python3 tests/test_packaging.py  # asset modes and relocated plugin discovery
python3 tests/run_ui.py          # compile the native UI harness; no windows launched
```

The adapter checks need permission to bind a localhost port in restricted
sandboxes. GUI checks and real service tests are separate from this offline
suite. After explicit permission, `python3 tests/run_ui.py --run` exercises native
provider setup, tab residency, cancellation and restoration and saves screenshots
and measurements under `/tmp/promptr-ui-review`. Release builds and publishing are not part of normal development checks.

### Install from source

```sh
sudo make install
```

Installs to `/usr/local`. Use `PREFIX` to change the destination:

```sh
sudo make install PREFIX=/usr
```

### GTK diagnostics

On GTK 4.24.0 through 4.24.2, Promptr routes the missing Inhibit portal warning
and three frame-timing warnings to `Gdk` debug output. Other warnings, including
theme sizing warnings, keep their original severity.

To include these diagnostics when investigating GTK issues:

```sh
G_MESSAGES_DEBUG=Gdk ./promptr-debug
```

## Releases

Download v0.3.0 packages from `packages.toxdes.com`. Append `.sha256` to a
download URL to get its checksum.

| Format | x86-64 | ARM64 |
| --- | --- | --- |
| Debian | [Download](https://packages.toxdes.com/releases/promptr_0.3.0_amd64.deb) | [Download](https://packages.toxdes.com/releases/promptr_0.3.0_arm64.deb) |
| RPM | [Download](https://packages.toxdes.com/releases/promptr-0.3.0-1.x86_64.rpm) | [Download](https://packages.toxdes.com/releases/promptr-0.3.0-1.aarch64.rpm) |
| Tarball | [Download](https://packages.toxdes.com/releases/promptr_0.3.0_amd64.tar.gz) | [Download](https://packages.toxdes.com/releases/promptr_0.3.0_arm64.tar.gz) |

See [CHANGELOG.md](CHANGELOG.md) for release notes.

For building packages and the full release workflow, see [releases.md](releases.md).

## License

MIT
