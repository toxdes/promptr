#ifndef CONFIG_H
#define CONFIG_H

/*
 * Hardcoded options for Agent and Model pickers.
 * The first entry "None" is always selected by default and
 * means the corresponding CLI flag (--agent / --model) is
 * omitted from the generated command.
 *
 * Add actual agent/model names after "None".  Example:
 *   {"None", "Planner", "Executor", NULL};
 *
 * If no additional entries are provided (array has only "None"
 * followed by NULL), the picker will show "None" and be
 * disabled.
 */

static const char *AGENT_OPTIONS[] = {
    "None",
    "linux_cmd",
    NULL
};

static const char *MODEL_OPTIONS[] = {
    "None",
    "opencode-go/deepseek-v4-flash",
    "opencode-go/deepseek-v4-pro",
    "opencode/big-pickle",
    "opencode/deepseek-v4-flash-free",
    NULL
};

#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600

/* Set to 0 to disable Escape-key hide behaviour */
#define ESCAPE_HIDES_WINDOW 1

/*
 * Keyboard shortcuts (GTK accelerators, e.g. "<Control>k").
 * Set to an empty string "" to disable a binding.
 */
#define KB_FOCUS_PROMPT  "<Control>k"
#define KB_COPY_MARKED   "<Control><Shift>c"
#define KB_QUIT          "<Control>q"

/* Set to 0 to use a regular window instead of wlroots layer-shell.
 * Layer-shell makes the window float above tiling (like rofi) but
 * prevents user-resizing.  Disable it if you need manual resizing
 * and configure floating in your compositor instead. */
#define LAYER_SHELL_ENABLED 0

/*
 * Lines to pre-mark in the output gutter (1-based line numbers).
 * Marks appear as blue dots in the gutter.  The Copy button copies
 * only marked lines.
 *
 *   { 0 }      — mark all lines
 *   { -1 }     — mark no lines (manual click-to-toggle only)
 *   { 1, 5, 10, -1 }  — mark lines 1, 5, and 10
 *   { 1, 2, 3, 4, 5, 6, -1 }  — mark lines 1 through 6
 *
 * The array MUST be terminated with -1.
 * You can click any line number in the gutter to toggle its mark.
 */
static const int DEFAULT_MARKED_LINES[] = { 0 };

#endif
