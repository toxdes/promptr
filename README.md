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
sudo apt install libgtk-4-dev
```

### Arch Linux

```sh
sudo pacman -S gtk4
```

## Usage

Launch the binary — a borderless window appears:

```sh
./promptr
```

- Type a query in the multi-line text input.
- Optionally select an **Agent** and a **Model** from the drop-downs.
- Press **Enter** or click **Submit** to run `opencode run`.
- **Shift+Enter** inserts a newline in the prompt.
- The command output appears in the read-only textarea below.
- Use **Copy** to copy the output to the clipboard.
- Press **Escape** or click **Close** to hide the window (process stays
  resident).  Subsequent launches reuse the same instance.
- Click **Close & Quit** to fully exit.

## Configuration

Edit `config.h` to add agent/model names and adjust window defaults,
then rebuild with `make`.

## License

MIT
