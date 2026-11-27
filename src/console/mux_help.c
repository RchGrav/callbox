/* console/mux_help.c - the keys panel (prefix ?): the prefix's keys, or the
 * active mode's, framed. */
#include "mux.h"

/* Keyboard help: the keys after the prefix, or the active mode's, in a
 * framed panel - sections in two columns when the terminal is wide enough. */
struct help_line {
    const char *key, *what; /* key NULL: a section heading (what) */
};
static const struct help_line keys_prefix[] = {
    {NULL, "Panes"},
    {"Enter", "New terminal (your shell)"},
    {"| -", "Split right / down: new terminal"},
    {"l", "Open a process (launcher)"},
    {"o O", "Next / previous pane"},
    {"Alt-arrows", "Next pane; past the edge, slide"},
    {"s", "Swap places (2C, or C here)"},
    {"z", "Zoom the pane, or back"},
    {"p", "Pane mode: arrows, a attach, g go to"},
    {NULL, "Windows and views"},
    {"t", "Window mode: 1-9, arrows on the grid"},
    {"r", "Resize mode"},
    {"m", "Move mode (in the base view)"},
    {"a", "Sort by name (view, toggle)"},
    {"w", "Spread over windows (view)"},
    {"d", "Pack four to a window (view)"},
    {NULL, "Display"},
    {"f", "Bottom bar on/off"},
    {"i", "Info: name bars on/off"},
    {"\\", "Lock pane (keys to its program)"},
    {NULL, "Chat and names"},
    {"c", "Chat (Esc closes it)"},
    {"n", "Rename the process"},
    {";", "Save its profile"},
    {NULL, "Session"},
    {"", ""}, /* the detach key, filled in when drawn */
    {"", ""}, /* the prefix twice */
    {"?", "These keys"},
};
static const struct help_line keys_pane[] = {
    {NULL, "Pane mode"},
    {"arrows h j k l", "Move to the next pane"},
    {"a", "Attach a process as a pane"},
    {"g", "Go to a pane by name"},
    {"z", "Zoom the pane, or back"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_tab[] = {
    {NULL, "Window mode"},
    {"1-9", "Go to that window; leave the mode"},
    {"arrows h j k l", "The window that way on the grid"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_resize[] = {
    {NULL, "Resize mode"},
    {"arrows h j k l", "Grow the pane that way"},
    {"Shift-arrows H J K L", "Shrink it from that side"},
    {"=", "Even the lines again"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_move[] = {
    {NULL, "Move mode (in the base view)"},
    {"arrows h j k l", "Swap with the pane that way"},
    {"1-9", "Send it to that window; leave the mode"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};

static void draw_help_lines(struct mux *m, const struct help_line *l, int n, int y, int x, int w, int keyw) {
    for (int i = 0; i < n; i++) {
        if (!l[i].key) {
            if (i > 0) y++; /* a blank line before each section but the first */
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
    struct help_line lines[sizeof(keys_prefix) / sizeof(keys_prefix[0])];
    const struct help_line *l = lines;
    int n = (int)(sizeof(keys_prefix) / sizeof(keys_prefix[0]));
    char pk[16], dk[16], twice[40], detach[48];
    mux_key_label(m->prefix, pk);
    mux_key_label(m->detach_key, dk);
    memcpy(lines, keys_prefix, sizeof(keys_prefix));
    snprintf(twice, sizeof(twice), "%s", pk);
    snprintf(detach, sizeof(detach), "Detach the pane; it runs on");
    for (int i = 0, k = 0; i < n; i++)
        if (lines[i].key && !lines[i].key[0]) {
            if (k++ == 0) lines[i] = (struct help_line){dk, detach};
            else lines[i] = (struct help_line){twice, "Send it to the program"};
        }
#define KEYS(arr) (l = (arr), n = (int)(sizeof(arr) / sizeof((arr)[0])))
    switch (m->mode) {
    case MODE_PANE: KEYS(keys_pane); break;
    case MODE_TAB: KEYS(keys_tab); break;
    case MODE_RESIZE: KEYS(keys_resize); break;
    case MODE_MOVE: KEYS(keys_move); break;
    default: break;
    }
#undef KEYS
    char title[64];
    if (m->mode == MODE_NONE) snprintf(title, sizeof(title), "Keys after %s", pk);
    else snprintf(title, sizeof(title), "Keys");
    int keyw = 13;
    /* two columns when it is tall and the terminal is wide: split at the
     * section heading nearest the middle */
    int split = n, total = help_height(l, n);
    if (m->mode == MODE_NONE && m->tcols >= 84 && total + 4 > m->arows - 2) {
        int best = n, bestd = INT_MAX;
        for (int i = 1; i < n; i++)
            if (!l[i].key) {
                int d = abs(help_height(l, i) - help_height(l + i, n - i));
                if (d < bestd) bestd = d, best = i;
            }
        split = best;
    }
    int colw = 13 + 38, cols = split < n ? 2 : 1;
    int inner_h = split < n ? (help_height(l, split) > help_height(l + split, n - split) ? help_height(l, split)
                                                                                       : help_height(l + split, n - split))
                            : total;
    int w = cols * colw + (cols - 1) * 3 + 4, h = inner_h + 4;
    if (w > m->tcols - 2) w = m->tcols - 2;
    if (h > m->arows) h = m->arows;
    if (w < 20 || h < 5) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2;
    mux_draw_frame(m, y, x, h, w, title, "any key closes  \xc2\xb7  backmux help panes");
    draw_help_lines(m, l, split, y + 2, x + 2, colw, keyw);
    if (split < n) draw_help_lines(m, l + split, n - split, y + 2, x + 2 + colw + 3, w - 4 - colw - 3, keyw);
}
