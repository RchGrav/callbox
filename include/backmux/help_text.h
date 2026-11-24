#ifndef HOLD_HELP_TEXT_H
#define HOLD_HELP_TEXT_H

#include <stdbool.h>

/* The help pages, compiled in from src/cli/help/ (scripts/gen_help.sh writes
 * gen/help_text.c). Pure data with no dependencies, so any layer may print a
 * page: hold_usage (runtime) prints the overview, hold_show_help (cli) the
 * rest. A page's name is its path under src/cli/help without .txt:
 * "overview", "guides/tui", "reference/all", "commands/send". */
struct hold_help_page {
    const char *name;
    const char *const *lines; /* each ends in \n; NULL-terminated */
};
extern const struct hold_help_page hold_help_pages[]; /* {NULL, NULL}-terminated */

/* Print a page to stdout: 0, or -1 when no page has that name. */
int hold_help_print(const char *name);
bool hold_help_exists(const char *name);

#endif /* HOLD_HELP_TEXT_H */
