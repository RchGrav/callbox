/* console/mux_layout.c - where panes are: windows and slots, the layout of
 * a window on screen, focus, adding, swapping, moving and arranging panes,
 * and navigation between them. */
#include "mux.h"

int mux_win_count(const struct mux *m, int w) {
    int k = 0;
    while (k < 4 && m->slot[w * 4 + k]) k++;
    return k;
}

int mux_pane_total(const struct mux *m) {
    int n = 0;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) n += mux_win_count(m, w);
    return n;
}

/* A window's number is its key, 1-9. */
int mux_window_key(int w) {
    return w + 1;
}

/* The next window, after w, that has panes; w itself if none other does. */
int mux_next_window(const struct mux *m, int w) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = (w + i) % MUX_MAX_WINDOWS;
        if (mux_win_count(m, c) > 0) return c;
    }
    return w;
}

/* The lines of window w, on a rows x cols screen: the column of the line
 * between left and right, and the row of the line in each column (left
 * column, right column). A share of 0 means half: a window never resized. */
static int pct_or_half(int pct) {
    return pct ? pct : 50;
}

void mux_lines_of(const struct mux *m, int w, int rows, int cols, int *left, int *ltop, int *rtop) {
    *left = (cols - 1) * pct_or_half(m->colpct[w]) / 100;
    *ltop = (rows - 1) * pct_or_half(m->lpct[w]) / 100;
    *rtop = (rows - 1) * pct_or_half(m->rpct[w]) / 100;
    if (*left < 1) *left = 1;
    if (*left > cols - 2) *left = cols - 2;
    if (*ltop < 1) *ltop = 1;
    if (*ltop > rows - 2) *ltop = rows - 2;
    if (*rtop < 1) *rtop = 1;
    if (*rtop > rows - 2) *rtop = rows - 2;
}

/* Where slot k of window w (n panes) goes, on a rows x cols screen. One
 * column of line separates left from right, one row top from bottom: A left,
 * B right, C under A, D under B. */
static void place(const struct mux *m, int w, int n, int k, int rows, int cols, int *r, int *c, int *h, int *wd) {
    if (n <= 1 || cols < 3 || rows < 3) {
        *r = 0, *c = 0, *h = rows, *wd = cols;
        return;
    }
    int left, ltop, rtop;
    mux_lines_of(m, w, rows, cols, &left, &ltop, &rtop);
    int rcol = left + 1, rw = cols - rcol;
    switch (k) {
    case 0: /* A: the left column, or its top from three on */
        *r = 0, *c = 0, *wd = left, *h = n >= 3 ? ltop : rows;
        break;
    case 1: /* B: the right column, or its top once there are four */
        *r = 0, *c = rcol, *wd = rw, *h = n == 4 ? rtop : rows;
        break;
    case 2: /* C: under A */
        *r = ltop + 1, *c = 0, *wd = left, *h = rows - ltop - 1;
        break;
    default: /* D: under B */
        *r = rtop + 1, *c = rcol, *wd = rw, *h = rows - rtop - 1;
        break;
    }
}

/* Every pane where it goes; a pane toggled full takes its window's whole
 * screen (the others in that window wait behind it, at their sizes). */
/* Inline, leaving the person's screen for the alternate one: first one
 * plain frame on theirs - the pane alone, the cursor where the program has
 * it - which erases the corner label and anything this compositor drew
 * there; the alternate screen then keeps their screen as it is. */
void mux_go_alt(struct mux *m) {
    struct pane *f = NULL;
    for (int i = 0; i < MUX_MAX_PANES && !f; i++) f = m->slot[i];
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

void mux_relayout(struct mux *m) {
    /* An inline terminal still shows the person's lines above the launch,
     * which no pane holds: once a second pane splits the screen it is all
     * ours - on the alternate screen, so leaving gives theirs back - and
     * the next frame paints all of it. (Before the panes are resized: the
     * last frame on the person's screen is the pane as it was.) */
    if (m->assumed && mux_pane_total(m) > 1) mux_go_alt(m);
    m->arows = m->trows - (mux_bar_on(m) ? 1 : 0);
    if (m->chat) chat_terminal_resized(m->chat, m->arows, m->tcols); /* chat stays clear of the bar */
    /* A name bar sits on the line above its pane: the split line under the
     * panes above, or - at the top of the screen - the pane's own first row,
     * which Info takes from it. */
    bool info = mux_info_on(m);
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        int n = mux_win_count(m, w);
        for (int k = 0; k < n; k++) {
            struct pane *p = m->slot[w * 4 + k];
            int r, c, h, wd;
            if (p == m->full) r = 0, c = 0, h = m->arows, wd = m->tcols;
            else place(m, w, n, k, m->arows, m->tcols, &r, &c, &h, &wd);
            p->brow = r > 0 ? r - 1 : r;
            if (info && r == 0 && h > 1) {
                r++;
                h--;
            }
            p->row = r, p->col = c;
            if (p->rows != h || p->cols != wd) {
                p->rows = h, p->cols = wd;
                mux_pane_resize(p, h, wd);
            }
        }
    }
    m->dirty = true;
    m->advertise = true;
}

/* Keep each window's panes dense from its start, in arrival order (there is
 * no C before a B). Windows keep their numbers: an empty one is a gap. The
 * view moves on if its window emptied. */
static void compact(struct mux *m) {
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        struct pane *in[4];
        int k = 0;
        for (int i = 0; i < 4; i++)
            if (m->slot[w * 4 + i]) in[k++] = m->slot[w * 4 + i];
        for (int i = 0; i < 4; i++) m->slot[w * 4 + i] = i < k ? in[i] : NULL;
    }
    if (m->full) {
        bool there = false;
        for (int i = 0; i < MUX_MAX_PANES && !there; i++) there = m->slot[i] == m->full;
        if (!there) m->full = NULL;
    }
    if (mux_win_count(m, m->view) == 0) m->view = mux_next_window(m, m->view);
    int n = mux_win_count(m, m->view);
    if (m->focus < m->view * 4 || m->focus >= m->view * 4 + n) m->focus = m->view * 4;
}

struct pane *mux_focused(const struct mux *m) {
    return m->slot[m->focus];
}

void mux_set_focus(struct mux *m, int slot) {
    if (slot < 0 || slot >= MUX_MAX_PANES || !m->slot[slot]) return;
    /* chat talks to the agent you are in: moving to an agent's pane while
     * chat is up moves the conversation with you (Tab still reaches anyone
     * else). Any other pane is an app: the one Assist asks about. */
    struct pane *to = m->slot[slot];
    if (m->chat && m->focus != slot) {
        if (to->agent[0]) chat_show_call(m->chat, to->id, to->name);
        else {
            m->app = to;
            chat_set_screen(m->chat, to->scr, to->name);
        }
    }
    m->focus = slot;
    m->view = slot / 4;
    m->dirty = true;
    m->advertise = true;
}

/* After panes left: close up the windows, lay them out again, and point chat
 * at the pane now in focus (the one it showed may be gone). */
void mux_settle(struct mux *m) {
    compact(m);
    mux_relayout(m);
    struct pane *f = mux_focused(m);
    bool app_here = false;
    for (int i = 0; i < MUX_MAX_PANES && !app_here; i++) app_here = m->app && m->slot[i] == m->app;
    if (!app_here) m->app = f && !f->agent[0] ? f : NULL;
    if (m->chat) {
        struct pane *a = m->app ? m->app : f;
        if (a) chat_set_screen(m->chat, a->scr, a->name);
    }
    m->dirty = true;
}

/* A new pane joins the window on screen when it has room, else the first
 * window that does. Focus goes to it. */
int mux_add_pane(struct mux *m, struct pane *p) {
    for (int i = 0; i < MUX_MAX_WINDOWS; i++) {
        int w = i == 0 ? m->view : i - 1;
        if (i > 0 && w == m->view) continue;
        int n = mux_win_count(m, w);
        if (n < 4) {
            m->slot[w * 4 + n] = p;
            mux_relayout(m);
            mux_set_focus(m, w * 4 + n);
            return 0;
        }
    }
    return -1;
}

bool mux_has_pane(const struct mux *m, const char *id) {
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strcmp(m->slot[i]->id, id)) return true;
    return false;
}

/* A pane by its process's name or id: what backmux swap takes from a shell
 * or an agent, which cannot see this screen. -1 if none. */
int mux_slot_of_name(const struct mux *m, const char *w) {
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strcmp(m->slot[i]->name, w)) return i;
    size_t l = strlen(w);
    for (int i = 0; l >= 4 && i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strncmp(m->slot[i]->id, w, l)) return i;
    return -1;
}

/* Two panes trade places: their windows and positions. */
void mux_do_swap(struct mux *m, int a, int b) {
    if (a == b || !m->slot[a] || !m->slot[b]) return;
    struct pane *t = m->slot[a];
    m->slot[a] = m->slot[b];
    m->slot[b] = t;
    mux_relayout(m);
    m->dirty = true;
}

/* The active pane goes to window w (its key's), at the end; the view stays
 * where it is. 0 moved; -1 that window is full. */
int mux_move_pane(struct mux *m, int slot, int w) {
    struct pane *p = m->slot[slot];
    if (!p || w == slot / 4) return 0;
    int n = mux_win_count(m, w);
    if (n >= 4) return -1;
    m->slot[slot] = NULL;
    m->slot[w * 4 + n] = p;
    if (m->full == p) m->full = NULL;
    int view = m->view;
    compact(m);
    if (mux_win_count(m, view) > 0) {
        m->view = view;
        m->focus = view * 4;
    }
    mux_relayout(m);
    return 0;
}

int mux_slot_at(const struct mux *m, int row, int col) {
    int n = mux_win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        const struct pane *p = m->slot[m->view * 4 + k];
        if (m->full && m->full == mux_focused(m) && p != m->full) continue;
        int top = mux_info_on(m) && p->brow < p->row ? p->brow : p->row; /* its name bar too */
        if (row >= top && row < p->row + p->rows && col >= p->col && col < p->col + p->cols) return m->view * 4 + k;
    }
    return -1;
}

/* A number key as a window: 1-9. -1 if not one. */
int mux_window_of_key(unsigned char c) {
    if (c >= '1' && c <= '9') return c - '1';
    return -1;
}

int mux_slot_of(const struct mux *m, const struct pane *p) {
    for (int i = 0; p && i < MUX_MAX_PANES; i++)
        if (m->slot[i] == p) return i;
    return -1;
}

/* Focus stays with the pane, wherever a rearrangement put it. */
void mux_follow(struct mux *m, struct pane *p) {
    int s = mux_slot_of(m, p);
    if (s >= 0) mux_set_focus(m, s);
}

/* After a pane was sent somewhere: focus stays with it while it is still on
 * this window; sent to another window, the view stays here. Why: sending a pane
 * away is not going after it. */
void mux_follow_here(struct mux *m, struct pane *p) {
    int s = mux_slot_of(m, p);
    if (s >= 0 && s / 4 == m->view) mux_set_focus(m, s);
    else if (!m->slot[m->focus] || m->focus / 4 != m->view) m->focus = m->view * 4;
    m->dirty = true;
}

static int by_pane_name(const void *a, const void *b) {
    return strcasecmp((*(struct pane *const *)a)->name, (*(struct pane *const *)b)->name);
}

void mux_arrange(struct mux *m, enum arrange how) {
    struct pane *all[MUX_MAX_PANES], *active = mux_focused(m);
    int pos[MUX_MAX_PANES], n = 0;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) {
            pos[n] = i;
            all[n++] = m->slot[i];
            m->slot[i] = NULL;
        }
    if (how == ARRANGE_ALPHA) {
        /* the same places, filled in name order */
        qsort(all, (size_t)n, sizeof(all[0]), by_pane_name);
        for (int i = 0; i < n; i++) m->slot[pos[i]] = all[i];
    } else if (how == ARRANGE_DENSE) {
        /* four to a window from the first: 12 fill windows 1-3 */
        for (int i = 0; i < n; i++) m->slot[i] = all[i];
    } else {
        /* every window used before any splits: 12 put two in each of windows 1-3
         * and one in each of windows 4-9 */
        int k = 0;
        for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
            int here = n / MUX_MAX_WINDOWS + (w < n % MUX_MAX_WINDOWS);
            for (int j = 0; j < here && j < 4; j++) m->slot[w * 4 + j] = all[k++];
        }
    }
    mux_relayout(m);
    if (active) mux_follow(m, active);
    m->dirty = true;
}

/* Adjacent pane in the requested direction; -1 if none. */
int mux_neighbor(const struct mux *m, char dir) {
    struct pane *f = mux_focused(m);
    if (!f || m->full) return -1;
    int best = -1, bestd = INT_MAX, n = mux_win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        int s = m->view * 4 + k;
        const struct pane *p = m->slot[s];
        if (p == f) continue;
        bool rows = p->row < f->row + f->rows && f->row < p->row + p->rows;
        bool cols = p->col < f->col + f->cols && f->col < p->col + p->cols;
        int d = INT_MAX;
        if (dir == 'D' && rows && p->col + p->cols <= f->col) d = f->col - (p->col + p->cols);
        if (dir == 'C' && rows && p->col >= f->col + f->cols) d = p->col - (f->col + f->cols);
        if (dir == 'A' && cols && p->row + p->rows <= f->row) d = f->row - (p->row + p->rows);
        if (dir == 'B' && cols && p->row >= f->row + f->rows) d = p->row - (f->row + f->rows);
        if (d < bestd) bestd = d, best = s;
    }
    return best;
}

int mux_prev_window(const struct mux *m, int w) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = (w - i + MUX_MAX_WINDOWS) % MUX_MAX_WINDOWS;
        if (mux_win_count(m, c) > 0) return c;
    }
    return w;
}

static int clamp_pct(int v) {
    return v < MUX_PCT_MIN ? MUX_PCT_MIN : v > MUX_PCT_MAX ? MUX_PCT_MAX : v;
}

/* Resize: the active pane grows toward a side, or shrinks from it, by
 * moving the line on that side; a side with no line (the screen's edge) does
 * nothing. */
void mux_resize_toward(struct mux *m, char dir, bool grow) {
    struct pane *f = mux_focused(m);
    int w = m->view, n = mux_win_count(m, w);
    if (!f || m->full || n < 2) return;
    int step = grow ? MUX_RESIZE_STEP : -MUX_RESIZE_STEP;
    bool left_col = f->col == 0;
    if (dir == 'C' && left_col) m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) + step);
    if (dir == 'D' && !left_col) m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) - step);
    int *col = left_col ? &m->lpct[w] : &m->rpct[w];
    bool split = left_col ? n >= 3 : n == 4;
    bool top = f->row == 0;
    if (split && dir == 'B' && top) *col = clamp_pct(pct_or_half(*col) + step);
    if (split && dir == 'A' && !top) *col = clamp_pct(pct_or_half(*col) - step);
    mux_relayout(m);
}
