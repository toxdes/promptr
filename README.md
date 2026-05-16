# promptr

A GTK4 overlay application that runs `opencode` commands through a
resizable, always-on-top window.

## Building

```sh
make
```

## Installing dependencies

### Debian / Ubuntu

```sh
sudo apt install libgtk-4-dev libgtk4-layer-shell-dev libgtksourceview-5-dev
```

### Arch Linux

```sh
sudo pacman -S gtk4 gtk4-layer-shell gtksourceview5
```

## Usage

Launch the binary — a borderless overlay window appears (uses layer-shell
on wlroots-based compositors like Sway, Hyprland, and River):

```sh
./promptr
```

- Type a query in the multi-line text input (the **Prompt** field).
- Optionally select an **Agent** and a **Model** from the drop-downs.
- The **CMD** preview updates in real time as you type or change
  selections, showing exactly what command will run.
- Press **Enter** or click **Submit** to run `opencode run`.
- **Shift+Enter** inserts a newline in the prompt.
- The command output appears in the read-only **Output** textarea below
  with line numbers in the gutter.
- Use **Copy** to copy all output to the clipboard.  Click a line number
  in the gutter to copy a single line.
- Press **Escape** or click **Close** to hide the window (process stays
  resident).  Subsequent launches reuse the same instance.
- Click **Close & Quit** to fully exit.  Your last selected model and
  agent are persisted to `~/.local/share/promptr/state` and restored on
  the next launch.

## Configuration

Edit `config.h` to add agent/model names and adjust window defaults,
then rebuild with `make`.

## License

MIT
