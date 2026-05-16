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

#define DEFAULT_WIDTH  700
#define DEFAULT_HEIGHT 500

/* Set to 0 to disable Escape-key hide behaviour */
#define ESCAPE_HIDES_WINDOW 1

#endif
