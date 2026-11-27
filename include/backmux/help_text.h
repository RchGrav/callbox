#ifndef HOLD_HELP_TEXT_H
#define HOLD_HELP_TEXT_H

#include <stdbool.h>
#include <stddef.h>

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

/* Print a page to stdout: 0, or -1 when no page has that name. The page
 * prints whole, as hold_help_print_view does with no reader. */
int hold_help_print(const char *name);
bool hold_help_exists(const char *name);
/* A page's lines (each ends in \n), or NULL: the keys panel reads keys/ pages. */
const char *const *hold_help_lines(const char *name);

/* Help for its reader. Why: an agent pays for every token it reads, and a
 * page written for someone else's case reads as a missing feature (an
 * agent inside backmux, reading the page for senders outside it, reported
 * features as missing that it had). A page marks blocks for one kind of
 * reader with a line of tags alone - [agent], [person], [inside],
 * [outside], combined as [agent inside] - and [all] ends the block. Lines
 * name facts about the reader as {key}; a line whose key has no value is
 * left out, and {!key} keeps a line only when key has no value. */
struct hold_help_var {
    const char *key, *value; /* value NULL or "": none */
};
struct hold_help_view {
    bool full;          /* every block, no line about the reader */
    bool agent, inside; /* who reads it, when not full */
    const struct hold_help_var *vars;
    size_t nvars;
};
int hold_help_print_view(const char *name, const struct hold_help_view *view);

#endif /* HOLD_HELP_TEXT_H */
