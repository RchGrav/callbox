/* console/mux_slide.c - the view sliding to where it goes next.
 *
 * When the view moves half a window or a whole one along one axis (Alt-
 * arrows past the edge, a window key, o), the panes slide over for about
 * 140 ms instead of jumping. Why: a half-window shift puts what was on the
 * right on the left, and a jump reads as everything changing at once; the
 * slide shows which way it went and that nothing was lost. It is optional
 * (config slide off, or Slide on the bar), never holds up a key - one
 * pressed mid-slide ends it at once and goes where it was meant to - and
 * held keys move at full speed, since each new move starts from where the
 * last one landed.
 *
 * How: the panes' frame before the move and the one after are composed
 * (the programs are not asked anything), and each frame of the slide is the
 * two side by side, offset a little further, easing out. */
#include "mux.h"

#define SLIDE_MS 140

/* How long a slide lasts. A test build (-DHOLD_TESTING) takes it from
 * HOLD_TEST_SLIDE_MS, so the suite can see a frame between two views on a
 * busy machine instead of sampling a 140 ms window it may miss. */
static int slide_ms(void) {
#ifdef HOLD_TESTING
    const char *t = getenv("HOLD_TEST_SLIDE_MS");
    if (t && atoi(t) > 0) return atoi(t);
#endif
    return SLIDE_MS;
}

bool mux_sliding(const struct mux *m) {
    return m->slide_from.cells != NULL;
}

void mux_slide_stop(struct mux *m) {
    free(m->slide_from.cells);
    free(m->slide_to.cells);
    memset(&m->slide_from, 0, sizeof(m->slide_from));
    memset(&m->slide_to, 0, sizeof(m->slide_to));
    m->slide_pending = false;
    m->dirty = true;
}

/* Called before the view moves to (nx, ny): the frame it leaves. */
void mux_slide_begin(struct mux *m, int nx, int ny) {
    int dx = nx - m->vx, dy = ny - m->vy;
    if (mux_sliding(m)) mux_slide_stop(m); /* a move mid-slide: from where the last one lands */
    /* along one axis, half a window or a whole one; on a screen worth it */
    if (!g_slide || (dx && dy) || (!dx && !dy) || abs(dx) > 2 || abs(dy) > 2 || m->tcols < 20 || m->arows < 6) return;
    struct comp_want want;
    mux_compose_panes(m, &want);
    if (comp_take_frame(m->comp, &m->slide_from) != 0) return;
    m->slide_pending = true;
    m->slide_dx = dx, m->slide_dy = dy;
    m->dirty = true;
}

static double ease_out(double t) {
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

static const struct hold_cell *cell_at(const struct hold_snapshot *s, int r, int c) {
    return &s->cells[(size_t)r * s->cols + (size_t)c];
}

/* One frame of the slide into the compositor, then chat, the bar and the
 * rest over it. false when there is no slide (or it just ended). */
bool mux_slide_frame(struct mux *m, struct comp_want *want) {
    if (!mux_sliding(m)) return false;
    /* what the terminal is told while the screen moves: no cursor, the
     * mouse still reported (a click mid-slide still lands) */
    memset(want, 0, sizeof(*want));
    want->mouse = HOLD_MOUSE_DRAG;
    if (m->slide_pending) { /* the view has moved: the frame it arrives at */
        mux_compose_panes(m, want);
        if (comp_take_frame(m->comp, &m->slide_to) != 0) {
            mux_slide_stop(m);
            return false;
        }
        m->slide_pending = false;
        m->slide_start = m->now;
    }
    double t = (double)(m->now - m->slide_start) / slide_ms();
    if (t >= 1.0 || m->slide_from.rows != m->slide_to.rows || m->slide_from.cols != m->slide_to.cols) {
        mux_slide_stop(m);
        return false;
    }
    int rows = m->arows < m->slide_from.rows ? m->arows : m->slide_from.rows, cols = m->slide_from.cols;
    bool across = m->slide_dx != 0;
    int span = across ? cols : rows, d = across ? m->slide_dx : m->slide_dy;
    /* how far the content travels: half the screen per quadrant */
    int travel = abs(d) == 2 ? span : (span + 1) / 2;
    int s = (int)(ease_out(t) * travel + 0.5);
    struct hold_snapshot frame = {.rows = (uint16_t)rows, .cols = (uint16_t)cols};
    frame.cells = malloc((size_t)rows * (size_t)cols * sizeof(*frame.cells));
    if (!frame.cells) {
        mux_slide_stop(m);
        return false;
    }
    /* the strip: going forward, the old frame then the new one's last
     * stretch; going back, the new one's first stretch then the old frame */
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int pos = across ? c : r;
            int i = d > 0 ? pos + s : pos + travel - s; /* index into the strip */
            const struct hold_snapshot *src;
            int at;
            if (d > 0) {
                src = i < span ? &m->slide_from : &m->slide_to;
                at = i < span ? i : i - span + (span - travel);
            } else {
                src = i < travel ? &m->slide_to : &m->slide_from;
                at = i < travel ? i : i - travel;
            }
            if (at < 0) at = 0;
            if (at >= span) at = span - 1;
            struct hold_cell *out = &frame.cells[(size_t)r * cols + (size_t)c];
            *out = *(across ? cell_at(src, r, at) : cell_at(src, at, c));
        }
    /* a wide character cut at the seam cannot be drawn: blanks there */
    for (int r = 0; r < rows; r++) {
        struct hold_cell *row = &frame.cells[(size_t)r * cols];
        for (int c = 0; c < cols; c++) {
            bool orphan = row[c].width == 0 && (c == 0 || row[c - 1].width != 2);
            bool cut = row[c].width == 2 && (c + 1 >= cols || row[c + 1].width != 0);
            if (orphan || cut) {
                memset(&row[c], 0, sizeof(row[c]));
                row[c].ch[0] = ' ';
                row[c].width = 1;
                row[c].flags = HOLD_CELL_FG_DEFAULT | HOLD_CELL_BG_DEFAULT;
            }
        }
    }
    comp_begin(m->comp, NULL);
    comp_blit(m->comp, &frame, 0, 0, rows, cols);
    free(frame.cells);
    want->cursor_visible = false; /* no cursor while the screen moves */
    mux_finish_frame(m, want);
    m->dirty = true; /* the next frame of it */
    return true;
}
