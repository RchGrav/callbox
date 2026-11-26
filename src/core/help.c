#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/help_text.h"

/* Help text is compiled from src/cli/help into the table declared in
 * backmux/help_text.h; runtime lookup does not read documentation files. */

static const struct hold_help_page *find_page(const char *name) {
    for (const struct hold_help_page *p = hold_help_pages; name && p->name; p++)
        if (!strcmp(p->name, name)) return p;
    return NULL;
}

bool hold_help_exists(const char *name) {
    return find_page(name) != NULL;
}

int hold_help_print(const char *name) {
    const struct hold_help_page *p = find_page(name);
    if (!p) return -1;
    for (const char *const *l = p->lines; *l; l++) fputs(*l, stdout);
    fflush(stdout);
    return 0;
}
