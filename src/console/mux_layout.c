/* console/mux_layout.c - the screen over the grid of windows.
 *
 * The nine windows sit in a 3x3 grid, read in order (1 2 3 / 4 5 6 /
 * 7 8 9), so the whole is 6x6 quadrants. The screen is a 2x2-quadrant
 * viewport onto it that moves in half-window steps: on one window, between
 * two side by side or one above the other, or where four meet. Why half
 * steps: the neighbor across a window's edge is then never more than half a
 * screen away, and moving keeps what was beside you in view.
 *
 * The screen is four cells, each owned by the pane on that quadrant (or
 * nobody). A pane is drawn over the cells it owns. A pane wider or taller
 * than what is in view is fitted to the part that is: between windows side
 * by side only tall panes keep their height, between windows above each
 * other only wide ones keep their width, and where four meet every pane is
 * one quadrant. Aligned on one window, its own split lines apply; between
 * windows the lines are halfway. */
#include "mux.h"

static int pct_or_half(int pct) {
    return pct ? pct : 50;
}

int mux_view_window(const struct mux *m) {
    return (m->vy / 2) * 3 + m->vx / 2;
}

bool mux_view_aligned(const struct mux *m) {
    return !(m->vx & 1) && !(m->vy & 1);
}

int mux_window_key(int w) {
    return w + 1;
}

/* The pane on canvas quadrant (qx, qy), in the view. */
static struct pane *quad_owner(const struct mux *m, int qx, int qy) {
    if (qx < 0 || qy < 0 || qx > 5 || qy > 5) return NULL;
    int w = (qy / 2) * 3 + qx / 2, lx = qx % 2, ly = qy % 2;
    unsigned mask = mux_window_mask(m->shown, w);
    for (int p = 0; p < MUX_POS; p++) {
        struct pane *pn = m->shown[w * MUX_POS + p];
        if (!pn) continue;
        int x, y, wd, h;
        mux_shape_rect(mask, p, &x, &y, &wd, &h);
        if (lx >= x && lx < x + wd && ly >= y && ly < y + h) return pn;
    }
    return NULL;
}

/* Inline, leaving the person's screen for the alternate one: first one
 * plain frame on theirs - the pane alone, the cursor where the program has
 * it - which erases anything this compositor drew there; the alternate
 * screen then keeps their screen as it is. */
void mux_go_alt(struct mux *m) {
    struct pane *f = NULL;
    for (int i = 0; i < MUX_SLOTS && !f; i++) f = m->base[i];
    if (f) {
        struct comp_want want = {0};
        struct hold_snapshot snap;
        comp_begin(m->comp, NULL);
        if (hold_screen_snapshot(f->scr, &snap) == 0) {
            comp_blit(m->comp, &snap, 0, 0, snap.rows, snap.cols);
            want.cursor_row = snap.cursor_row;
            want.cursor_col = snap.cursor_col;
            want.cursor_visible = snap.modes.cursor_visible;
            hold_snapshot_free(&snap);
        }
        char *buf = NULL;
        size_t len = 0, cap = 0;
        if (comp_flush(m->comp, &want, &buf, &len, &cap) == 0 && len) (void)hold_write_all(STDOUT_FILENO, buf, len);
        free(buf);
    }
    hold_console_leave_inline();
    comp_invalidate(m->comp);
    m->assumed = false;
    m->dirty = true;
}

/* Whether the screen is the compositor's for good: backmux shell, or more
 * than one pane. Only then do the bar and the name bars show. Why: with one
 * program and chat up for a moment, a row taken and given back would make
 * the program redraw twice for nothing. */
bool mux_decorated(const struct mux *m) {
    return m->resident || mux_pane_total(m) > 1;
}

bool mux_bar_on(const struct mux *m) {
    return g_bar && mux_decorated(m) && m->trows > 3;
}

bool mux_info_on(const struct mux *m) {
    return g_info && mux_decorated(m);
}

/* Where the lines are for this view: the column between left and right,
 * the row in the left and the right column. On one window its own shares
 * (rows-first shapes - A across the top, or E across the bottom - have one
 * row for both); between windows, halfway. */
static void view_lines(const struct mux *m, int rows, int cols, int *x, int *yl, int *yr) {
    int lp = 50, rp = 50, cp = 50;
    if (mux_view_aligned(m)) {
        int w = mux_view_window(m);
        unsigned mask = mux_window_mask(m->shown, w);
        bool rows_first = (mask & (1u << POS_E)) || !(mask & (1u << POS_B));
        cp = pct_or_half(m->colpct[w]);
        lp = pct_or_half(m->lpct[w]);
        rp = rows_first ? lp : pct_or_half(m->rpct[w]);
    }
    *x = (cols - 1) * cp / 100;
    *yl = (rows - 1) * lp / 100;
    *yr = (rows - 1) * rp / 100;
    if (*x < 1) *x = 1;
    if (*x > cols - 2) *x = cols - 2;
    if (*yl < 1) *yl = 1;
    if (*yl > rows - 2) *yl = rows - 2;
    if (*yr < 1) *yr = 1;
    if (*yr > rows - 2) *yr = rows - 2;
}

void mux_relayout(struct mux *m) {
    /* An inline terminal still shows the person's lines above the launch,
     * which no pane holds: once a second pane splits the screen it is all
     * ours - on the alternate screen, so leaving gives theirs back. */
    if (m->assumed && mux_pane_total(m) > 1) mux_go_alt(m);
    mux_view_compute(m);
    m->arows = m->trows - (mux_bar_on(m) ? 1 : 0);
    if (m->chat) chat_terminal_resized(m->chat, m->arows, m->tcols); /* chat stays clear of the bar */
    int rows = m->arows, cols = m->tcols;
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->base[i]) m->base[i]->visible = false;
    for (int c = 0; c < 4; c++) m->cell[c] = quad_owner(m, m->vx + c % 2, m->vy + c / 2);
    if (m->full && mux_base_slot(m, m->full) >= 0 && m->full == m->active)
        for (int c = 0; c < 4; c++) m->cell[c] = m->full;
    if (cols < 3 || rows < 3) /* too small to split: the active pane alone */
        for (int c = 0; c < 4; c++) m->cell[c] = m->active ? m->active : m->cell[0];
    int x, yl, yr;
    view_lines(m, rows, cols, &x, &yl, &yr);
    m->gx = x, m->gyl = yl, m->gyr = yr;
    /* A name bar sits on the line above its pane: the split line, or - at
     * the top of the screen - the pane's own first row, which Info takes. */
    bool info = mux_info_on(m);
    for (int c = 0; c < 4; c++) {
        struct pane *p = m->cell[c];
        if (!p || p->visible) continue;
        bool tl = m->cell[0] == p, tr = m->cell[1] == p, bl = m->cell[2] == p, br = m->cell[3] == p;
        bool left = tl || bl, right = tr || br, top = tl || tr, bottom = bl || br;
        int split = left ? yl : yr;
        int r0 = top ? 0 : split + 1, r1 = bottom ? rows : split;
        int c0 = left ? 0 : x + 1, c1 = right ? cols : x;
        if (cols < 3 || rows < 3) r0 = 0, r1 = rows, c0 = 0, c1 = cols;
        p->brow = r0 > 0 ? r0 - 1 : 0;
        if (info && r0 == 0 && r1 - r0 > 1) r0++;
        p->visible = true;
        p->row = r0, p->col = c0;
        if (p->rows != r1 - r0 || p->cols != c1 - c0) {
            p->rows = r1 - r0, p->cols = c1 - c0;
            mux_pane_resize(p, p->rows, p->cols);
        }
    }
    m->dirty = true;
    m->advertise = true;
}

/* ---- focus ---------------------------------------------------------------- */

/* Chat talks to the agent you are in; any other pane is an app - the one
 * Assist asks about. */
static void focus_told(struct mux *m, struct pane *to) {
    if (!m->chat || !to) return;
    if (to->agent[0]) chat_show_call(m->chat, to->id, to->name);
    else {
        m->app = to;
        chat_set_screen(m->chat, to->scr, to->name);
    }
}

/* Make p the active pane; the view follows when it is off screen, to its
 * window. */
void mux_set_focus(struct mux *m, struct pane *p) {
    if (!p || mux_base_slot(m, p) < 0) return;
    if (m->active != p) focus_told(m, p);
    m->active = p;
    if (!p->visible) {
        int s = mux_shown_slot(m, p);
        if (s >= 0) mux_align(m, s / MUX_POS);
    }
    m->dirty = true;
    m->advertise = true;
}

void mux_align(struct mux *m, int w) {
    int nx = (w % 3) * 2, ny = (w / 3) * 2;
    if (nx != m->vx || ny != m->vy) mux_slide_begin(m, nx, ny);
    m->vx = nx, m->vy = ny;
    mux_relayout(m);
}

/* After panes left or the view changed: lay out again, and keep a pane in
 * focus - the active one, wherever it is now, or the first in view. */
void mux_settle(struct mux *m) {
    mux_relayout(m);
    if (m->active && mux_base_slot(m, m->active) < 0) m->active = NULL;
    if (!m->active)
        for (int c = 0; c < 4 && !m->active; c++) m->active = m->cell[c];
    for (int i = 0; i < MUX_SLOTS && !m->active; i++) m->active = m->shown[i];
    bool app_here = m->app && mux_base_slot(m, m->app) >= 0;
    if (!app_here) m->app = m->active && !m->active->agent[0] ? m->active : NULL;
    if (m->active) mux_set_focus(m, m->active);
    if (m->chat) {
        struct pane *a = m->app ? m->app : m->active;
        if (a) chat_set_screen(m->chat, a->scr, a->name);
    }
    m->dirty = true;
}

/* After a pane was sent away (swap, move): the view stays where it is - sending
 * a pane away is not going after it - so focus stays in view: on the active
 * pane if it is still here, else the first pane in view. */
void mux_focus_in_view(struct mux *m) {
    if (m->active && m->active->visible) return;
    for (int c = 0; c < 4; c++)
        if (m->cell[c]) {
            focus_told(m, m->cell[c]);
            m->active = m->cell[c];
            m->dirty = true;
            return;
        }
}

/* An attach: into the base where the view says, made the active pane. */
int mux_add_pane(struct mux *m, struct pane *p) {
    if (!mux_base_add(m, p)) return -1;
    mux_relayout(m);
    focus_told(m, p);
    m->active = NULL;
    mux_set_focus(m, p);
    mux_relayout(m);
    return 0;
}

/* The pane drawn at a screen cell (its name bar too, while Info is on). */
struct pane *mux_pane_at(const struct mux *m, int row, int col) {
    for (int c = 0; c < 4; c++) {
        struct pane *p = m->cell[c];
        if (!p) continue;
        int top = mux_info_on(m) ? p->brow : p->row;
        if (row >= top && row < p->row + p->rows && col >= p->col && col < p->col + p->cols) return p;
    }
    return NULL;
}

/* ---- moving between panes and windows --------------------------------------- */

/* The pane on screen next to the active one that way ('A' up, 'B' down, 'C'
 * right, 'D' left), NULL if none. */
struct pane *mux_neighbor(const struct mux *m, char dir) {
    const struct pane *f = m->active;
    if (!f || !f->visible || (m->full && m->full == f)) return NULL;
    struct pane *best = NULL;
    int bestd = INT_MAX;
    for (int c = 0; c < 4; c++) {
        struct pane *p = m->cell[c];
        if (!p || p == f) continue;
        bool rows = p->row < f->row + f->rows && f->row < p->row + p->rows;
        bool cols = p->col < f->col + f->cols && f->col < p->col + p->cols;
        int d = INT_MAX;
        if (dir == 'D' && rows && p->col + p->cols <= f->col) d = f->col - (p->col + p->cols);
        if (dir == 'C' && rows && p->col >= f->col + f->cols) d = p->col - (f->col + f->cols);
        if (dir == 'A' && cols && p->row + p->rows <= f->row) d = f->row - (p->row + p->rows);
        if (dir == 'B' && cols && p->row >= f->row + f->rows) d = p->row - (f->row + f->rows);
        if (d < bestd) bestd = d, best = p;
    }
    return best;
}

static bool column_has(const struct mux *m, int qx) {
    return quad_owner(m, qx, m->vy) || quad_owner(m, qx, m->vy + 1);
}

static bool row_has(const struct mux *m, int qy) {
    return quad_owner(m, m->vx, qy) || quad_owner(m, m->vx + 1, qy);
}

/* Past the edge of the screen: the view moves half a window that way (past
 * quadrants with nothing in them), and the pane that comes into view next
 * to where you were becomes the active one. false at the grid's edge. */
bool mux_shift(struct mux *m, char dir) {
    int nx = m->vx, ny = m->vy;
    bool found = false;
    if (dir == 'C')
        for (nx = m->vx + 1; nx <= 4 && !found; nx++) found = column_has(m, nx + 1);
    else if (dir == 'D')
        for (nx = m->vx - 1; nx >= 0 && !found; nx--) found = column_has(m, nx);
    else if (dir == 'B')
        for (ny = m->vy + 1; ny <= 4 && !found; ny++) found = row_has(m, ny + 1);
    else if (dir == 'A')
        for (ny = m->vy - 1; ny >= 0 && !found; ny--) found = row_has(m, ny);
    if (!found) return false;
    if (dir == 'C') nx--;
    if (dir == 'D') nx++;
    if (dir == 'B') ny--;
    if (dir == 'A') ny++;
    if (dir == 'C' || dir == 'D') ny = m->vy;
    else nx = m->vx;
    int old_mid = m->active ? m->active->row + m->active->rows / 2 : 0;
    int old_cmid = m->active ? m->active->col + m->active->cols / 2 : 0;
    mux_slide_begin(m, nx, ny);
    m->vx = nx, m->vy = ny;
    mux_relayout(m);
    /* the pane on the side that came in, nearest where you were */
    struct pane *best = NULL;
    int bestd = INT_MAX;
    for (int c = 0; c < 4; c++) {
        bool incoming = (dir == 'C' && c % 2 == 1) || (dir == 'D' && c % 2 == 0) || (dir == 'B' && c / 2 == 1) ||
                        (dir == 'A' && c / 2 == 0);
        struct pane *p = m->cell[c];
        if (!incoming || !p) continue;
        int d = dir == 'C' || dir == 'D' ? abs(p->row + p->rows / 2 - old_mid) : abs(p->col + p->cols / 2 - old_cmid);
        if (d < bestd) bestd = d, best = p;
    }
    if (best) mux_set_focus(m, best);
    return true;
}

/* The next (step 1) or previous (-1) window that has panes, from w. */
int mux_window_step(const struct mux *m, int w, int step) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = ((w + step * i) % MUX_MAX_WINDOWS + MUX_MAX_WINDOWS) % MUX_MAX_WINDOWS;
        if (mux_window_count(m->shown, c) > 0) return c;
    }
    return w;
}

/* The next pane (step 1) or the one before (-1), in the order on screen
 * across every window: A to E, window 1 to 9, round again. */
void mux_cycle(struct mux *m, int step) {
    int at = mux_shown_slot(m, m->active);
    for (int i = 1; i <= MUX_SLOTS; i++) {
        int s = ((at + step * i) % MUX_SLOTS + MUX_SLOTS) % MUX_SLOTS;
        if (m->shown[s]) {
            mux_set_focus(m, m->shown[s]);
            mux_relayout(m);
            return;
        }
    }
}

static int clamp_pct(int v) {
    return v < MUX_PCT_MIN ? MUX_PCT_MIN : v > MUX_PCT_MAX ? MUX_PCT_MAX : v;
}

/* Resize: the active pane grows toward a side, or shrinks from it, by
 * moving the line on that side; on one window only (between windows the
 * lines are halfway, nobody's to move). Returns false when there is no
 * line that way. */
bool mux_resize_toward(struct mux *m, char dir, bool grow) {
    struct pane *f = m->active;
    if (!f || !f->visible || !mux_view_aligned(m) || (m->full && m->full == f)) return false;
    int w = mux_view_window(m);
    unsigned mask = mux_window_mask(m->shown, w);
    bool rows_first = (mask & (1u << POS_E)) || !(mask & (1u << POS_B));
    int step = grow ? MUX_RESIZE_STEP : -MUX_RESIZE_STEP;
    bool left = f->col == 0, top = f->row == 0 || f->brow == 0, right = f->col + f->cols >= m->tcols,
         bottom = f->row + f->rows >= m->arows;
    if ((dir == 'C' && right) || (dir == 'D' && left) || (dir == 'A' && top) || (dir == 'B' && bottom)) return false;
    /* growing right moves the line on its right rightward; growing left
     * moves the line on its left leftward (shrinking: the other way) */
    if (dir == 'C') m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) + step);
    else if (dir == 'D') m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) - step);
    else {
        int *row = rows_first || left ? &m->lpct[w] : &m->rpct[w];
        *row = clamp_pct(pct_or_half(*row) + (dir == 'B' ? step : -step));
    }
    m->layout_dirty = true;
    mux_relayout(m);
    return true;
}
