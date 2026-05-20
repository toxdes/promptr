# AGENTS.md

## Build

- `make` — debug build with `-Og -ggdb3` and `-pedantic` (default)
- `make release` — release build with `-O2` and `-Werror`
- After editing `.c` / `.h` files, run `make` (not release) — CI handles release
- Before committing, run `./format-all.py`

## Code style

- C11 + GLib/GTK4 idioms
- `g_autofree`, `g_autoptr`, `g_autofree char *` for automatic cleanup — always
- Use `GString` for string building; return with `g_string_free(out, FALSE)`
- Use `G_N_ELEMENTS()` for array bounds — never hardcode lengths
- Forward-declare all `static` functions called before their definition
- Prefer `gtk_widget_set_tooltip_text()` for hover hints
- Pango markup only when GTK's widget API can't express the intent directly

## Debug logging

- `log_append(win, "event -> detail", ...)` for session log entries
- `g_warning(...)` for runtime anomalies (conflicts, missing files)
- Guard all GTK assertions: `GTK_IS_*`, `GTK_IS_TEXT_BUFFER`, etc.
- When a widget pointer might be uninitialized, check before use
- For memory-sensitive code, prefer GLib allocation with `g_new0` / `g_free`

## Safety

- Never write past buffer bounds — use `g_strlcpy`, `g_strndup`, checked indices
- Always check `NULL` return from allocation functions (`fopen`, `g_malloc`, etc.)
- Always check `NULL` return from widget creation (though GTK rarely fails)
- Guard `signal_connect` / `signal_connect_data` callbacks against destroyed widgets
- Use `-Werror`-compatible code paths (no unused variables, no implicit declarations)
- Widgets created but not yet parented must be guarded before `gtk_text_view_get_buffer`, etc.

## UI / UX

- No emojis in source code, comments, or UI strings
- Keyboard shortcuts displayed in lowercase: `ctrl+shift+c`, `enter`, `esc`
- Use `accel_to_human()` to convert GTK accelerator format
- Status bar shows context-sensitive hints on hover via `status_bar_on_hover()`
- Config keys must be added to all three places: `config.h`, `src/configfile.c`
  `CONFIG_DEFAULTS[]` (with `.comment` matching `config.h`'s comment), and the
  repo `config` template file. The initial config is generated at runtime from
  `CONFIG_DEFAULTS[]` — no separate `DEFAULT_CONFIG` string literal exists.
  Run `make config` to verify alignment.

## Files of note

| Path | Role |
|------|------|
| `config.h` | Compile-time defaults |
| `src/configfile.c` | Runtime config + migration (`~/.config/promptr/config`) |
| `src/window.c` | Main UI construction, state machine, callbacks |
| `src/window.h` | `AppWindow` struct definition |
| `src/command.c` | Async subprocess execution |
| `src/state.c` / `state.h` | Model/agent selection persistence |
| `config` | Reference config template (keep in sync with `CONFIG_DEFAULTS[]` output) |
| `VERSION` | Single-line version string |
