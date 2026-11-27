/* console/mux_help.c - the keys panel (prefix ?): the prefix's keys, or the
 * active mode's, framed. */
#include "mux.h"
#include "backmux/help_text.h"

/* Keyboard help: the keys after the prefix, or the active mode's, in a
 * framed panel - sections in two columns when the terminal is wide enough. */
struct help_line {
    const char *key, *what; /* key NULL: a section heading (what) */
};
/* The keys come from the help pages src/cli/help/keys/ (prefix.txt, and
 * one per mode): the same text backmux help keys prints, so the panel and
 * the page cannot disagree. A line at the margin is a section; an indented
 * one is a key, then two or more spaces, then what it does. The q and
 * Ctrl-Space entries stand for the detach key and the prefix, and show the
 * configured ones. (Groups and labels per Rich's critique of the earlier
 * panel: by function, a verb and its object, each mode its own entry.) */
#define KEYS_MAX 48
static struct help_line g_keys[KEYS_MAX];
static char g_text[KEYS_MAX][2][48];

static int load_keys(const char *page, const char *pk, const char *dk) {
    const char *const *l = hold_help_lines(page);
    int n = 0;
    for (; l && *l && n < KEYS_MAX; l++) {
        size_t len = strcspn(*l, "\n");
        if (len == 0) continue;
        if ((*l)[0] != ' ') {
            snprintf(g_text[n][1], sizeof(g_text[n][1]), "%.*s", (int)len, *l);
            g_keys[n] = (struct help_line){NULL, g_text[n][1]};
            n++;
            continue;
        }
        const char *k = *l + strspn(*l, " "), *gap = strstr(k, "  ");
        if (!gap || gap >= *l + len) continue;
        const char *w = gap + strspn(gap, " ");
        snprintf(g_text[n][0], sizeof(g_text[n][0]), "%.*s", (int)(gap - k), k);
        snprintf(g_text[n][1], sizeof(g_text[n][1]), "%.*s", (int)(*l + len - w), w);
        if (!strcmp(g_text[n][0], "q")) snprintf(g_text[n][0], sizeof(g_text[n][0]), "%s", dk);
        else if (!strcmp(g_text[n][0], "Ctrl-Space")) snprintf(g_text[n][0], sizeof(g_text[n][0]), "%s", pk);
        g_keys[n] = (struct help_line){g_text[n][0], g_text[n][1]};
        n++;
    }
    return n;
}

static void draw_help_lines(struct mux *m, const struct help_line *l, int n, int y, int x, int w, int keyw, int last) {
    for (int i = 0; i < n && y <= last; i++) { /* never past the frame: a short terminal shows what fits */
        if (!l[i].key) {
            if (i > 0) y++; /* a blank line before each section but the first */
            if (y > last) break;
            mux_put(m, y++, x, w, l[i].what, &ST_HEAD);
            continue;
        }
        mux_put(m, y, x + 1, keyw, l[i].key, &ST_KEY);
        mux_put(m, y++, x + 1 + keyw, w - 1 - keyw, l[i].what, &ST_BOX);
    }
}

static int help_height(const struct help_line *l, int n) {
    int h = 0;
    for (int i = 0; i < n; i++) h += !l[i].key && i > 0 ? 2 : 1;
    return h;
}

void mux_draw_help(struct mux *m) {
    char pk[16], dk[16];
    mux_key_label(m->prefix, pk);
    mux_key_label(m->detach_key, dk);
    const char *page = m->mode == MODE_PANE     ? "keys/pane"
                       : m->mode == MODE_TAB    ? "keys/window"
                       : m->mode == MODE_RESIZE ? "keys/resize"
                       : m->mode == MODE_MOVE   ? "keys/move"
                                                : "keys/prefix";
    int n = load_keys(page, pk, dk);
    const struct help_line *l = g_keys;
    if (n == 0) return;
    char title[64];
    if (m->mode == MODE_NONE) snprintf(title, sizeof(title), "Keys after %s", pk);
    else if (!l[0].key) { /* a mode: its name is the title, not a heading too */
        snprintf(title, sizeof(title), "%s", l[0].what);
        l++, n--;
    } else snprintf(title, sizeof(title), "Keys");
    /* Columns as wide as their keys and labels; two of them, split at the
     * section heading nearest the middle, when one is too tall for the
     * screen and two fit across it (an 80-column terminal does). */
    int keyw = 0, labw = 0;
    for (int i = 0; i < n; i++) {
        int kl = l[i].key ? (int)strlen(l[i].key) : 0, wl = (int)strlen(l[i].what) - (l[i].key ? 0 : 1);
        if (kl > keyw) keyw = kl;
        if (wl > labw) labw = wl;
    }
    keyw += 2;
    int colw = 1 + keyw + labw, split = n, total = help_height(l, n);
    if (total + 4 > m->arows - 2 && 2 * colw + 2 + 4 <= m->tcols - 2) {
        int best = n, bestd = INT_MAX;
        for (int i = 1; i < n; i++)
            if (!l[i].key) {
                int d = abs(help_height(l, i) - help_height(l + i, n - i));
                if (d < bestd) bestd = d, best = i;
            }
        split = best;
    }
    int cols = split < n ? 2 : 1;
    int inner_h = split < n ? (help_height(l, split) > help_height(l + split, n - split) ? help_height(l, split)
                                                                                       : help_height(l + split, n - split))
                            : total;
    int w = cols * colw + (cols - 1) * 2 + 4, h = inner_h + 4;
    if (w > m->tcols - 2) w = m->tcols - 2;
    if (h > m->arows) h = m->arows;
    if (w < 20 || h < 5) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2, last = y + h - 3;
    mux_draw_frame(m, y, x, h, w, title, m->mode == MODE_NONE ? "any key closes  \xc2\xb7  in a mode, ? lists its keys" : "? or Esc closes");
    draw_help_lines(m, l, split, y + 2, x + 2, colw, keyw, last);
    if (split < n) draw_help_lines(m, l + split, n - split, y + 2, x + 2 + colw + 2, w - 4 - colw - 2, keyw, last);
}
