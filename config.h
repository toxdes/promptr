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
#define KB_CLOSE              "<Control>q"
#define KB_QUIT               "<Control><Shift>q"
#define KB_LOG                "<Control><Shift>d"
#define KB_SHORTCUTS          "<Control>F1"

/* Layer-shell overlay on wlroots compositors (0 or 1) */
#define LAYER_SHELL_ENABLED   0

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

/* Window decorations when layer-shell is disabled (0 or 1) */
#define DECORATED_DEFAULT        1

/* Font sizes in pt for prompt and output textviews (0 = system default) */
#define PROMPT_FONT_SIZE_DEFAULT 0
#define OUTPUT_FONT_SIZE_DEFAULT 0

/* Command section expanded by default (0 or 1) */
#define COMMAND_EXPANDED_DEFAULT 0

/* Hex color for marked-line gutter indicator */
#define MARK_BG_COLOR            "#33cc7f"

/*
 * GSK renderer backend (set before gtk_init, not switchable at runtime):
 *   cairo  — software (CPU), no GPU allocations, ~half baseline memory
 *   ngl    — OpenGL (GPU), GTK 4.14+ default, highest memory usage
 *   gl     — OpenGL (GPU), deprecated in 4.14, aliases to ngl
 *   vulkan — Vulkan (GPU), similar memory to GL; requires Vulkan driver
 */
#define GSK_RENDERER_DEFAULT     "cairo"

#ifndef VERSION
#define VERSION "unknown"
#endif

#endif
