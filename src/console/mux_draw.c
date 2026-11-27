/* console/mux_draw.c - drawing a frame: panes, split lines, name bars,
 * framed floating windows, then chat, help and the bar over them. */
#include "mux.h"

int mux_put(struct mux *m, int row, int col, int max, const char *text, const struct comp_style *st) {
    return max > 0 ? comp_text(m->comp, row, col, max, text, strlen(text), st) : 0;
}

/* Display columns of UTF-8 text (every character here is one column). */
int mux_text_cols(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

/* A floating window: a filled panel in a rounded frame, its title set into
 * the top edge and a hint into the bottom one. */
void mux_draw_frame(struct mux *m, int y, int x, int h, int w, const char *title, const char *hint) {
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    for (int c = x + 1; c < x + w - 1; c++) {
        comp_put(m->comp, y, c, 0x2500, &ST_FRAME);
        comp_put(m->comp, y + h - 1, c, 0x2500, &ST_FRAME);
    }
    for (int r = y + 1; r < y + h - 1; r++) {
        comp_put(m->comp, r, x, 0x2502, &ST_FRAME);
        comp_put(m->comp, r, x + w - 1, 0x2502, &ST_FRAME);
    }
    comp_put(m->comp, y, x, 0x256d, &ST_FRAME);
    comp_put(m->comp, y, x + w - 1, 0x256e, &ST_FRAME);
    comp_put(m->comp, y + h - 1, x, 0x2570, &ST_FRAME);
    comp_put(m->comp, y + h - 1, x + w - 1, 0x256f, &ST_FRAME);
    if (title && *title) {
        int used = mux_put(m, y, x + 2, w - 4, " ", &ST_FRAME_TITLE);
        used += mux_put(m, y, x + 2 + used, w - 4 - used, title, &ST_FRAME_TITLE);
        mux_put(m, y, x + 2 + used, w - 4 - used, " ", &ST_FRAME_TITLE);
    }
    if (hint && *hint) {
        int hl = mux_text_cols(hint) + 2;
        if (hl < w - 4) {
            int hx = x + w - 2 - hl;
            mux_put(m, y + h - 1, hx, hl, " ", &ST_BOX_DIM);
            mux_put(m, y + h - 1, hx + 1, hl - 1, hint, &ST_BOX_DIM);
            mux_put(m, y + h - 1, hx + hl - 1, 1, " ", &ST_BOX_DIM);
        }
    }
}

/* The split lines, from who owns each quarter of the screen: a line runs
 * between two quarters with different owners (an empty quarter too), never
 * through a pane that spans both. Each line cell's glyph comes from which
 * of its neighbors are lines, so every junction draws itself. */
static void draw_separators(struct mux *m) {
    int R = m->arows, C = m->tcols, X = m->gx, YL = m->gyl, YR = m->gyr;
    if (C < 3 || R < 3) return;
    struct pane *const *cell = m->cell;
    unsigned char *line = calloc((size_t)R * (size_t)C, 1);
    if (!line) return;
#define LN(r, c) line[(size_t)(r) * (size_t)C + (size_t)(c)]
    for (int r = 0; r < R; r++) {
        if (r == YL || r == YR) continue; /* where rows meet: decided below */
        struct pane *lo = r < YL ? cell[0] : cell[2], *ro = r < YR ? cell[1] : cell[3];
        if (lo != ro) LN(r, X) = 1;
    }
    if (cell[0] != cell[2])
        for (int c = 0; c < X; c++) LN(YL, c) = 1;
    if (cell[1] != cell[3])
        for (int c = X + 1; c < C; c++) LN(YR, c) = 1;
    int jr[2] = {YL, YR};
    for (int k = 0; k < 2; k++) {
        int r = jr[k];
        if ((r > 0 && LN(r - 1, X)) || (r + 1 < R && LN(r + 1, X)) || LN(r, X - 1) || LN(r, X + 1)) LN(r, X) = 1;
    }
    static const uint32_t glyph[16] = {
        /* u d l r */
        0x2502, 0x2500, 0x2500, 0x2500, 0x2502, 0x250c, 0x2510, 0x252c,
        0x2502, 0x2514, 0x2518, 0x2534, 0x2502, 0x251c, 0x2524, 0x253c,
    };
    const struct pane *f = m->active;
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            if (!LN(r, c)) continue;
            unsigned mask = (r > 0 && LN(r - 1, c) ? 8u : 0) | (r + 1 < R && LN(r + 1, c) ? 4u : 0) |
                            (c > 0 && LN(r, c - 1) ? 2u : 0) | (c + 1 < C && LN(r, c + 1) ? 1u : 0);
            bool near = f && f->visible && r >= f->row - 1 && r <= f->row + f->rows && c >= f->col - 1 &&
                        c <= f->col + f->cols;
            uint32_t g = glyph[mask];
            if (r == 0 && mux_info_on(m) && (mask & 4u)) g = 0x252c; /* ┬: the line meets the name bars */
            comp_put(m->comp, r, c, g, near ? &ST_SEP_FOCUS : &ST_SEP);
        }
#undef LN
    free(line);
}

/* A key as a person reads it: NUL is Ctrl-Space, a control byte Ctrl-X, a
 * printable byte itself. */
void mux_key_label(unsigned char k, char out[16]) {
    if (k == 0) snprintf(out, 16, "Ctrl-Space");
    else if (k < 0x20) snprintf(out, 16, "Ctrl-%c", k | 0x40);
    else snprintf(out, 16, "%c", k);
}

/* Jump: the panes of this terminal by name, with their window's number. */
static void draw_jump(struct mux *m) {
    int rows = mux_pane_total(m);
    int w = 52 < m->tcols - 4 ? 52 : m->tcols - 4, h = rows + 4 < m->arows - 2 ? rows + 4 : m->arows - 2;
    if (w < 16 || h < 5) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2, k = 0;
    mux_draw_frame(m, y, x, h, w, "Go to pane", "Up/Down  Enter  Esc");
    for (int i = 0; i < MUX_SLOTS && k < h - 4; i++) {
        if (!m->shown[i]) continue;
        const struct comp_style *st = k == m->pick_sel ? &ST_SEL : &ST_BOX;
        char line[ALIAS_MAX_LEN + 128], at[4];
        const struct pane *p = m->shown[i];
        mux_position_label(m, p, at);
        snprintf(line, sizeof(line), " %-3s %-*s %-10s win %d", at, w - 26 > 4 ? w - 26 : 4, p->name,
                 p->agent[0] ? p->agent : p->program, mux_window_key(i / MUX_POS));
        comp_fill(m->comp, y + 2 + k, x + 1, w - 2, st);
        mux_put(m, y + 2 + k, x + 1, w - 2, line, st);
        k++;
    }
}

/* Bytes as a person reads them: 512K, 3.1G. */
void mux_human_bytes(uint64_t b, char *out, size_t n) {
    if (b >= (uint64_t)10 << 30) snprintf(out, n, "%lluG", (unsigned long long)(b >> 30));
    else if (b >= (uint64_t)1 << 30) snprintf(out, n, "%.1fG", (double)b / (double)(1u << 30));
    else if (b >= (uint64_t)1 << 20) snprintf(out, n, "%lluM", (unsigned long long)(b >> 20));
    else snprintf(out, n, "%lluK", (unsigned long long)(b >> 10));
}

/* A pane's name bar: its letter and flags at the left, its name boxed in the
 * middle (click it, or the prefix then n, to type a new one over it), and at
 * the right what it runs and its load. The line is green beside the focused
 * pane, as the split lines are. */
static void draw_name_bar(struct mux *m, struct pane *p, struct comp_want *want) {
    bool focus = p == m->active;
    int row = p->brow, col = p->col, w = p->cols;
    if (row < 0 || row >= m->arows || w < 4) return;
    const struct comp_style *line = focus ? &ST_SEP_FOCUS : &ST_NB_LINE;
    for (int c = col; c < col + w; c++) comp_put(m->comp, row, c, 0x2500, line);
    bool editing = m->prompt != PROMPT_NONE && m->prompt_pane == p;
    /* the box: ┤ name ├, centered */
    char shown[ALIAS_MAX_LEN + 1];
    snprintf(shown, sizeof(shown), "%s", editing ? m->in : p->name);
    int nl = mux_text_cols(shown), bw = nl + 4;
    if (editing && bw < 14) bw = 14; /* room to type */
    if (bw > w - 2) bw = w - 2;
    int bx = col + (w - bw) / 2;
    p->box_col = bx;
    p->box_w = bw;
    const struct comp_style *nst = editing ? (m->in_fresh ? &ST_NB_FRESH : &ST_NB_EDIT) : focus ? &ST_NB_NAME_F : &ST_NB_NAME;
    comp_put(m->comp, row, bx, 0x2524, line); /* ┤ */
    comp_fill(m->comp, row, bx + 1, bw - 2, editing ? &ST_NB_EDIT : nst);
    int used = mux_put(m, row, bx + 2, bw - 4, shown, nst);
    comp_put(m->comp, row, bx + bw - 1, 0x251c, line); /* ├ */
    if (editing) {
        want->cursor_visible = true;
        want->cursor_row = row;
        want->cursor_col = bx + 2 + used < bx + bw - 1 ? bx + 2 + used : bx + bw - 2;
    }
    /* left: its place in the base ("1A" - the same in every view, the
     * name swap and every command take), then what is true of it now */
    int lx = col + 1, lend = bx - 1;
    char at[4], chip[8];
    mux_position_label(m, p, at);
    snprintf(chip, sizeof(chip), " %s ", at);
    if (lx + 4 < lend) {
        bool chosen = m->swapping && m->swap_first == mux_base_slot(m, p);
        lx += mux_put(m, row, lx, lend - lx, chip, m->swapping ? (chosen ? &ST_SEL : &ST_LETTER) : &ST_NB_LETTER);
        lx++;
    }
    char flags[4][40];
    int nf = 0;
    if (editing) snprintf(flags[nf++], 40, " %s ", m->prompt == PROMPT_SAVE ? "Save as" : "Rename");
    if (mux_pane_locked(p)) snprintf(flags[nf++], 40, " LOCKED ");
    if (m->full == p) snprintf(flags[nf++], 40, " ZOOM ");
    if (focus && !mux_bar_on(m)) {
        const char *mode = m->mode == MODE_PANE     ? "PANE"
                           : m->mode == MODE_TAB    ? "WINDOW"
                           : m->mode == MODE_RESIZE ? "RESIZE"
                           : m->mode == MODE_MOVE   ? "MOVE"
                           : m->swapping            ? "SWAP"
                                                    : NULL;
        if (mode && nf < 4) snprintf(flags[nf++], 40, " %s ", mode);
        int unread = m->chat && !chat_visible(m->chat) ? chat_unread(m->chat) : 0;
        if (unread && nf < 4) snprintf(flags[nf++], 40, " chat %d ", unread);
    }
    for (int i = 0; i < nf; i++) {
        int fl = mux_text_cols(flags[i]);
        if (lx + fl >= lend) break;
        lx += mux_put(m, row, lx, lend - lx, flags[i], &ST_NB_FLAG);
        lx++;
    }
    if (focus && !mux_bar_on(m) && m->now < m->notice_until && lx + 4 < lend) {
        lx += mux_put(m, row, lx, lend - lx, " ", &ST_NOTE);
        lx += mux_put(m, row, lx, lend - lx - 1, m->notice, &ST_NOTE);
        mux_put(m, row, lx, lend - lx, " ", &ST_NOTE);
    }
    /* right: what it runs, its CPU and memory - the least needed go first */
    int rstart = bx + bw + 1, rend = col + w - 1;
    char cpu[16] = "", mem[16] = "";
    if (p->cpu_pct >= 0) snprintf(cpu, sizeof(cpu), "%d%%", p->cpu_pct);
    if (p->rss) mux_human_bytes(p->rss, mem, sizeof(mem));
    const char *what = p->agent[0] ? p->agent : p->program;
    for (int drop = 0; drop <= 3; drop++) {
        char text[96] = "";
        size_t k = 0;
        bool has_what = what[0] && drop < 3, has_cpu = cpu[0] && drop < 2, has_mem = mem[0] && drop < 1;
        if (!has_what && !has_cpu && !has_mem) break;
        k += (size_t)snprintf(text + k, sizeof(text) - k, " ");
        if (has_what) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s", what);
        if (has_cpu) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", has_what ? "  " : "", cpu);
        if (has_mem) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", has_what || has_cpu ? "  " : "", mem);
        snprintf(text + k, sizeof(text) - k, " ");
        int tl = mux_text_cols(text);
        if (rend - tl < rstart) continue;
        int tx = rend - tl;
        int wl = has_what ? mux_text_cols(what) : 0;
        mux_put(m, row, tx, 1, " ", &ST_NB_STAT);
        if (has_what) mux_put(m, row, tx + 1, wl, what, p->agent[0] ? &ST_NB_AGENT : &ST_NB_STAT);
        mux_put(m, row, tx + 1 + wl, tl - 1 - wl, text + 1 + (has_what ? strlen(what) : 0), &ST_NB_STAT);
        break;
    }
}

void mux_render(struct mux *m) {
    mux_advertise(m);
    if (!m->dirty) return;
    m->dirty = false;
    comp_begin(m->comp, NULL);
    struct pane *f = m->active && m->active->visible ? m->active : NULL;
    bool full = m->full && m->full == f;
    /* the panes in view, each once */
    struct pane *vis[4];
    int n = 0;
    for (int c = 0; c < 4; c++) {
        bool seen = false;
        for (int k = 0; k < n; k++) seen = seen || vis[k] == m->cell[c];
        if (m->cell[c] && !seen) vis[n++] = m->cell[c];
    }
    for (int k = 0; k < n; k++) {
        struct pane *p = vis[k];
        struct hold_snapshot snap;
        if (hold_screen_snapshot(p->scr, &snap) == 0) {
            comp_blit(m->comp, &snap, p->row, p->col, p->rows, p->cols);
            hold_snapshot_free(&snap);
        }
    }
    if (!full) draw_separators(m);
    if (n == 0 && m->arows > 2) { /* nothing in this part of the grid */
        static const char empty[] = "Nothing here: Ctrl-Space l opens a process, Alt-arrows move";
        int el = mux_text_cols(empty);
        mux_put(m, m->arows / 2, el < m->tcols ? (m->tcols - el) / 2 : 0, m->tcols, empty, &ST_NB_STAT);
    }
    struct comp_want want = {0};
    struct hold_screen_modes modes = {0};
    if (f) hold_screen_modes(f->scr, &modes);
    want.app_cursor_keys = modes.app_cursor_keys;
    want.app_keypad = modes.app_keypad;
    want.bracketed_paste = modes.bracketed_paste;
    /* clicks and drags come here (click to move between panes, chat's
     * window dragged); plain movement only when the focused program asked
     * for it, which it then gets */
    want.mouse = modes.mouse == HOLD_MOUSE_MOVE ? HOLD_MOUSE_MOVE : HOLD_MOUSE_DRAG;
    want.cursor_visible = false;
    if (f) {
        struct hold_snapshot snap;
        if (hold_screen_snapshot(f->scr, &snap) == 0) {
            want.cursor_row = f->row + snap.cursor_row;
            want.cursor_col = f->col + snap.cursor_col;
            want.cursor_visible = snap.modes.cursor_visible;
            hold_snapshot_free(&snap);
        }
        want.cursor_style = hold_screen_cursor_style(f->scr);
        want.title = hold_screen_title(f->scr);
    }
    /* name bars: every pane's while Info is on; else only a pane with
     * something to show right now (swap's letters, a name being typed, a
     * notice with no bar to say it in) */
    bool info = mux_info_on(m);
    for (int k = 0; k < n; k++) {
        struct pane *p = vis[k];
        bool needed = m->swapping || (m->prompt != PROMPT_NONE && m->prompt_pane == p) ||
                      (p == f && m->now < m->notice_until && !mux_bar_on(m));
        if (info || needed) draw_name_bar(m, p, &want);
    }
    /* chat over the panes, then any other floating window over chat: only
     * one is up at a time anyway */
    if (m->chat) chat_draw(m->chat, m->comp, m->now, &want);
    if (m->fl == FL_PROCS) {
        mux_draw_procs(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_JUMP) {
        draw_jump(m);
        want.cursor_visible = false;
    }
    if (m->help) {
        mux_draw_help(m);
        want.cursor_visible = false;
    }
    if (mux_bar_on(m)) mux_draw_bar(m);
    else if (!f && m->now < m->notice_until) mux_put(m, m->trows - 1, 0, m->tcols, m->notice, &ST_BAD); /* no pane to say it in */
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (comp_flush(m->comp, &want, &buf, &len, &cap) == 0 && len) (void)hold_write_all(STDOUT_FILENO, buf, len);
    free(buf);
}
