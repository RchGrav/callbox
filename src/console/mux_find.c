/* console/mux_find.c - a pane, or a place, by the word for it.
 *
 * One rule wherever a pane is named on this terminal (backmux swap, the
 * layouts' requests): a place ("1A".."9E", either case), a process name,
 * or a process id, each by the shortest prefix that is unique. An exact
 * name or id wins outright; anything that could mean two things is refused,
 * naming both. Why: a person, a script and an agent can all name a pane the
 * same way, and never move the wrong one. */
#include "mux.h"

/* The pane the word names, or NULL. *slot gets the base place named (a
 * place may be empty: then the pane is NULL and *slot >= 0); *why says why
 * nothing matched or why the word is refused. */
struct pane *mux_find(const struct mux *m, const char *word, int *slot, const char **why) {
    static char msg[160];
    *slot = -1;
    *why = NULL;
    if (!word || !*word) {
        *why = "nothing named";
        return NULL;
    }
    size_t wl = strlen(word);
    for (int i = 0; i < MUX_SLOTS; i++) { /* exact name or id */
        struct pane *p = m->base[i];
        if (p && (!strcmp(p->name, word) || !strcmp(p->id, word))) {
            *slot = i;
            return p;
        }
    }
    int place = mux_parse_position(word);
    struct pane *hit = NULL;
    int hits = 0, hit_slot = -1;
    char first[ALIAS_MAX_LEN + 1] = "", second[ALIAS_MAX_LEN + 1] = "";
    for (int i = 0; i < MUX_SLOTS; i++) {
        struct pane *p = m->base[i];
        if (!p || (strncmp(p->name, word, wl) != 0 && strncmp(p->id, word, wl) != 0)) continue;
        if (hits == 0) snprintf(first, sizeof(first), "%s", p->name);
        else if (hits == 1) snprintf(second, sizeof(second), "%s", p->name);
        hits++;
        hit = p;
        hit_slot = i;
    }
    if (place >= 0 && hits > 0) {
        snprintf(msg, sizeof(msg), "'%s' is both place %c%c and %s: give more of the name", word, word[0],
                 toupper((unsigned char)word[1]), first);
        *why = msg;
        return NULL;
    }
    if (place >= 0) {
        *slot = place;
        return m->base[place];
    }
    if (hits > 1) {
        snprintf(msg, sizeof(msg), "'%s' could be %s or %s%s: give more of it", word, first, second, hits > 2 ? " (or more)" : "");
        *why = msg;
        return NULL;
    }
    if (hits == 0) {
        snprintf(msg, sizeof(msg), "%s is not a pane in that terminal", word);
        *why = msg;
        return NULL;
    }
    *slot = hit_slot;
    return hit;
}
