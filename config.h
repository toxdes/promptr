#ifndef CONFIG_H
#define CONFIG_H

/*
 * Compile-time fallback defaults for promptr.
 * All values here can be overridden at runtime via
 * ~/.config/promptr/config (created on first launch).
 */

#define DEFAULT_WIDTH         900
#define DEFAULT_HEIGHT        700

/* Set to 0 to disable Escape-key hide behaviour */
#define ESCAPE_HIDES_WINDOW   1

/*
 * Keyboard shortcuts (GTK accelerator string format, e.g. "<Control>k").
 */
#define KB_FOCUS_PROMPT       "<Control>k"
#define KB_COPY_MARKED        "<Control><Shift>c"
#define KB_QUIT               "<Control>q"

/* Layer-shell overlay on wlroots compositors (0 or 1) */
#define LAYER_SHELL_ENABLED   1

/* Comma-separated options for Agent/Model pickers (first=default) */
#define DEFAULT_AGENT_OPTIONS "linux_cmd,None"
#define DEFAULT_MODEL_OPTIONS                                                  \
  "opencode-go/deepseek-v4-flash,"                                             \
  "opencode-go/deepseek-v4-pro,"                                               \
  "opencode/big-pickle,"                                                       \
  "opencode/deepseek-v4-flash-free,None"

/* Path to the opencode binary */
#define OPENCODE_PATH            "opencode"

/* Default marked lines (0=all, -1=none, or comma-separated 1-based) */
#define DEFAULT_MARKED_LINES_STR "1"

/* Desktop notification on copy (0 or 1) */
#define NOTIFY_ON_COPY           1

/* Hex color for marked-line gutter indicator */
#define MARK_BG_COLOR            "#33cc7f"

#ifndef VERSION
#define VERSION "unknown"
#endif

#endif
