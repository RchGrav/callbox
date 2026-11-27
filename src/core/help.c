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

const char *const *hold_help_lines(const char *name) {
    const struct hold_help_page *p = find_page(name);
    return p ? p->lines : NULL;
}

/* A line of tags alone ("[agent inside]"): who the block below it is for.
 * Sets *agent / *inside to 1, 0, or -1 (either); [all] sets both to -1. */
static bool tag_line(const char *l, int *agent, int *inside) {
    size_t n = strcspn(l, "\n");
    if (n < 3 || l[0] != '[' || l[n - 1] != ']') return false;
    int a = -1, in = -1;
    for (size_t i = 1; i < n - 1;) {
        while (i < n - 1 && l[i] == ' ') i++;
        size_t w = i;
        while (i < n - 1 && l[i] != ' ') i++;
        size_t len = i - w;
        if (len == 0) continue;
        if (len == 5 && !strncmp(l + w, "agent", 5)) a = 1;
        else if (len == 6 && !strncmp(l + w, "person", 6)) a = 0;
        else if (len == 6 && !strncmp(l + w, "inside", 6)) in = 1;
        else if (len == 7 && !strncmp(l + w, "outside", 7)) in = 0;
        else if (len == 3 && !strncmp(l + w, "all", 3)) continue;
        else return false; /* a bracketed line of text, not tags */
    }
    *agent = a;
    *inside = in;
    return true;
}

/* The facts about a reader a page can name. Any other {word} is text. */
static const char *const reader_keys[] = {
    "name", "name_raw", "parent", "parent_raw", "last_from", "last_from_raw", "last_at", "inbox", "inbox_raw", "next", "chat",
};

static const char *var_value(const struct hold_help_view *v, const char *key, size_t len) {
    bool known = false;
    for (size_t i = 0; i < sizeof(reader_keys) / sizeof(reader_keys[0]) && !known; i++)
        known = strlen(reader_keys[i]) == len && !strncmp(reader_keys[i], key, len);
    if (!known) return NULL;
    for (size_t i = 0; i < v->nvars; i++)
        if (strlen(v->vars[i].key) == len && !strncmp(v->vars[i].key, key, len))
            return v->vars[i].value ? v->vars[i].value : "";
    return "";
}

/* One line with its {key}s filled; false when it is left out. */
static bool fill_line(const char *l, const struct hold_help_view *v, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = l; *p;) {
        const char *close = *p == '{' ? strchr(p, '}') : NULL;
        if (close) {
            bool neg = p[1] == '!';
            const char *key = p + 1 + neg;
            const char *val = var_value(v, key, (size_t)(close - key));
            if (val) {
                if (v->full) return false; /* about the reader: not in the whole page */
                if (neg ? *val != '\0' : *val == '\0') return false;
                if (!neg)
                    for (const char *q = val; *q && o + 1 < n; q++) out[o++] = *q;
                p = close + 1;
                continue;
            }
        }
        if (o + 1 < n) out[o++] = *p;
        p++;
    }
    out[o] = '\0';
    return true;
}

int hold_help_print_view(const char *name, const struct hold_help_view *view) {
    const struct hold_help_page *p = find_page(name);
    if (!p) return -1;
    static const struct hold_help_view whole = {.full = true};
    const struct hold_help_view *v = view ? view : &whole;
    int agent = -1, inside = -1;
    char line[1024];
    for (const char *const *l = p->lines; *l; l++) {
        if (tag_line(*l, &agent, &inside)) continue;
        if (!v->full && ((agent >= 0 && agent != v->agent) || (inside >= 0 && inside != v->inside))) continue;
        if (fill_line(*l, v, line, sizeof(line))) fputs(line, stdout);
    }
    fflush(stdout);
    return 0;
}

int hold_help_print(const char *name) {
    return hold_help_print_view(name, NULL);
}
