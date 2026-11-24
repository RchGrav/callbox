#include "backmux/config.h"
#include "backmux/core.h"
#include "backmux/screen.h"
#include "overlay.h"

/* The compositor of a backmux shell's display: one frame of cells per update,
 * built from the shell's screen (the base layer) and whatever is drawn over
 * it, then written to the attached terminal as the difference from what that
 * terminal already shows. It is the only writer to that terminal, so a
 * redraw by the shell can never paint over the chat window, and uncovering a
 * cell shows the shell's current content: every frame starts again from the
 * live screen. */

struct term_state {
    bool app_cursor_keys, app_keypad, bracketed_paste, focus_report;
    uint8_t mouse;
    bool cursor_visible;
    int cursor_row, cursor_col;
    int cursor_style;
    char title[256];
};

struct compositor {
    int rows, cols;
    struct hold_cell *frame; /* the frame being composed */
    struct hold_cell *shown; /* what the terminal shows */
    bool shown_valid;
    bool term_known; /* term below is what the terminal is set to */
    struct term_state term;
};

static const struct hold_cell blank_cell = {
    .ch = {' ', 0, 0},
    .width = 1,
    .flags = HOLD_CELL_FG_DEFAULT | HOLD_CELL_BG_DEFAULT,
};

struct compositor *comp_new(void) {
    return calloc(1, sizeof(struct compositor));
}

void comp_free(struct compositor *c) {
    if (!c) return;
    free(c->frame);
    free(c->shown);
    free(c);
}

void comp_invalidate(struct compositor *c) {
    c->shown_valid = false;
    c->term_known = false;
}

/* The terminal already shows base (an inline session: the program's own
 * output is on it, from where the launch began, and above that the
 * person's lines): take that as what is shown, so the next frame paints
 * only what differs - a chat window's cells, not the whole screen. The
 * terminal's modes are still sent. */
void comp_assume(struct compositor *c, const struct hold_snapshot *base) {
    if (!c->frame) return;
    comp_begin(c, base);
    memcpy(c->shown, c->frame, (size_t)c->rows * (size_t)c->cols * sizeof(*c->shown));
    c->shown_valid = true;
    c->term_known = false;
}

void comp_forget(struct compositor *c, int row, int col, int n) {
    if (!c->shown || row < 0 || row >= c->rows) return;
    if (col < 0) n += col, col = 0;
    if (col + n > c->cols) n = c->cols - col;
    /* width 255 matches no real cell, so the next frame writes them */
    for (int i = 0; i < n; i++) c->shown[(size_t)row * (size_t)c->cols + (size_t)(col + i)].width = 255;
}

int comp_set_size(struct compositor *c, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows > HOLD_TERM_MAX_DIM) rows = HOLD_TERM_MAX_DIM;
    if (cols > HOLD_TERM_MAX_DIM) cols = HOLD_TERM_MAX_DIM;
    if (rows == c->rows && cols == c->cols && c->frame) return 0;
    size_t n = (size_t)rows * (size_t)cols;
    struct hold_cell *frame = calloc(n, sizeof(*frame)), *shown = calloc(n, sizeof(*shown));
    if (!frame || !shown) {
        free(frame);
        free(shown);
        return -1;
    }
    free(c->frame);
    free(c->shown);
    c->frame = frame;
    c->shown = shown;
    c->rows = rows;
    c->cols = cols;
    /* A resized terminal has rearranged what it showed: repaint it all. Its
     * modes are untouched. */
    c->shown_valid = false;
    return 0;
}

void comp_size(const struct compositor *c, int *rows, int *cols) {
    *rows = c->rows;
    *cols = c->cols;
}

void comp_begin(struct compositor *c, const struct hold_snapshot *base) {
    if (!c->frame) return;
    for (int r = 0; r < c->rows; r++) {
        struct hold_cell *row = c->frame + (size_t)r * c->cols;
        for (int col = 0; col < c->cols; col++) {
            if (!base || r >= base->rows || col >= base->cols) {
                row[col] = blank_cell;
                continue;
            }
            row[col] = base->cells[(size_t)r * base->cols + col];
            if (row[col].width > 2) row[col].width = 1;
            if (row[col].ch[0] == 0 && row[col].width != 0) row[col].ch[0] = ' ';
        }
        /* A wide character the terminal's edge cuts in half, or a right half
         * with nothing to its left, cannot be drawn. */
        if (row[c->cols - 1].width == 2) {
            row[c->cols - 1].ch[0] = ' ';
            row[c->cols - 1].ch[1] = row[c->cols - 1].ch[2] = 0;
            row[c->cols - 1].width = 1;
        }
        for (int col = 0; col < c->cols; col++) {
            if (row[col].width == 0 && (col == 0 || row[col - 1].width != 2)) {
                row[col].ch[0] = ' ';
                row[col].width = 1;
            }
        }
    }
}

/* A pane: snap's top-left rows x cols drawn at (row, col), clipped to the
 * frame. A wide character cut by the pane's right edge becomes a blank, as
 * one cut by the terminal's edge does. Why a blit and not a base layer:
 * with panes, several programs' screens share one frame. */
void comp_blit(struct compositor *c, const struct hold_snapshot *snap, int row, int col, int rows, int cols) {
    if (!c->frame || !snap) return;
    for (int r = 0; r < rows && r < snap->rows; r++) {
        int fr = row + r;
        if (fr < 0 || fr >= c->rows) continue;
        struct hold_cell *dst = c->frame + (size_t)fr * c->cols;
        for (int k = 0; k < cols && k < snap->cols; k++) {
            int fc = col + k;
            if (fc < 0 || fc >= c->cols) continue;
            struct hold_cell cell = snap->cells[(size_t)r * snap->cols + k];
            if (cell.width > 2) cell.width = 1;
            if (cell.ch[0] == 0 && cell.width != 0) cell.ch[0] = ' ';
            bool cut = (cell.width == 2 && (k == cols - 1 || fc == c->cols - 1)) || (cell.width == 0 && k == 0);
            if (cut) {
                cell.ch[0] = ' ';
                cell.ch[1] = cell.ch[2] = 0;
                cell.width = 1;
            }
            dst[fc] = cell;
        }
    }
}

static void style_cell(struct hold_cell *cell, const struct comp_style *st) {
    cell->attrs = st->attrs;
    cell->flags = 0;
    memset(cell->fg, 0, 3);
    memset(cell->bg, 0, 3);
    cell->fg_idx = cell->bg_idx = 0;
    if (st->fg < 0) cell->flags |= HOLD_CELL_FG_DEFAULT;
    else {
        cell->flags |= HOLD_CELL_FG_INDEXED;
        cell->fg_idx = (uint8_t)st->fg;
    }
    if (st->bg < 0) cell->flags |= HOLD_CELL_BG_DEFAULT;
    else {
        cell->flags |= HOLD_CELL_BG_INDEXED;
        cell->bg_idx = (uint8_t)st->bg;
    }
}

/* Make (row, col) a blank of its own: whatever half of a wide character sat
 * there takes its other half with it. */
static void unpair(struct compositor *c, struct hold_cell *row, int col) {
    if (row[col].width == 0 && col > 0 && row[col - 1].width == 2) {
        row[col - 1].ch[0] = ' ';
        row[col - 1].ch[1] = row[col - 1].ch[2] = 0;
        row[col - 1].width = 1;
    }
    if (row[col].width == 2 && col + 1 < c->cols && row[col + 1].width == 0) {
        row[col + 1].ch[0] = ' ';
        row[col + 1].ch[1] = row[col + 1].ch[2] = 0;
        row[col + 1].width = 1;
    }
}

void comp_put(struct compositor *c, int row, int col, uint32_t cp, const struct comp_style *st) {
    if (!c->frame || row < 0 || row >= c->rows || col < 0 || col >= c->cols) return;
    struct hold_cell *cells = c->frame + (size_t)row * c->cols;
    int w = cp < 0x80 ? 1 : hold_char_width(cp);
    if (cp < 0x20 || cp == 0x7f || w == 0) {
        cp = ' ';
        w = 1;
    }
    if (w == 2 && col + 1 >= c->cols) {
        cp = ' ';
        w = 1;
    }
    unpair(c, cells, col);
    if (w == 2) unpair(c, cells, col + 1);
    cells[col].ch[0] = cp;
    cells[col].ch[1] = cells[col].ch[2] = 0;
    cells[col].width = (uint8_t)w;
    style_cell(&cells[col], st);
    if (w == 2) {
        cells[col + 1] = cells[col];
        cells[col + 1].ch[0] = 0;
        cells[col + 1].width = 0;
    }
}

void comp_fill(struct compositor *c, int row, int col, int n, const struct comp_style *st) {
    for (int i = 0; i < n; i++) comp_put(c, row, col + i, ' ', st);
}

/* One codepoint of UTF-8; malformed bytes read as U+FFFD, one at a time. */
size_t comp_utf8_next(const char *s, size_t len, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    size_t need = u[0] < 0x80 ? 1 : (u[0] & 0xe0) == 0xc0 ? 2 : (u[0] & 0xf0) == 0xe0 ? 3 : (u[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (need == 0 || need > len) {
        *cp = 0xfffd;
        return 1;
    }
    uint32_t v = need == 1 ? u[0] : need == 2 ? u[0] & 0x1fu : need == 3 ? u[0] & 0x0fu : u[0] & 0x07u;
    for (size_t i = 1; i < need; i++) {
        if ((u[i] & 0xc0) != 0x80) {
            *cp = 0xfffd;
            return 1;
        }
        v = (v << 6) | (u[i] & 0x3fu);
    }
    *cp = v;
    return need;
}

int comp_text(struct compositor *c, int row, int col, int max_cols, const char *text, size_t len,
              const struct comp_style *st) {
    int used = 0;
    struct hold_cell *last = NULL;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += comp_utf8_next(text + i, len - i, &cp);
        if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
        int w = cp < 0x80 ? 1 : hold_char_width(cp);
        if (w == 0) {
            /* a combining mark rides on the character before it */
            if (last && cp >= 0x80)
                for (int k = 1; k < HOLD_CELL_CHARS; k++)
                    if (!last->ch[k]) {
                        last->ch[k] = cp;
                        break;
                    }
            continue;
        }
        if (used + w > max_cols) break;
        comp_put(c, row, col + used, cp, st);
        last = (c->frame && row >= 0 && row < c->rows && col + used >= 0 && col + used < c->cols)
                   ? &c->frame[(size_t)row * c->cols + col + used]
                   : NULL;
        used += w;
    }
    return used;
}

/* ---- the difference, as bytes -------------------------------------------- */

struct obuf {
    char *p;
    size_t len, cap;
    bool bad;
};

static void ob_add(struct obuf *o, const char *s, size_t n) {
    if (o->bad) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char *p = realloc(o->p, cap);
        if (!p) {
            o->bad = true;
            return;
        }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void ob_str(struct obuf *o, const char *s) {
    ob_add(o, s, strlen(s));
}

static void ob_fmt(struct obuf *o, const char *fmt, ...) {
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) ob_add(o, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static bool pen_eq(const struct hold_cell *a, const struct hold_cell *b) {
    const uint8_t mask = HOLD_CELL_FG_DEFAULT | HOLD_CELL_BG_DEFAULT | HOLD_CELL_FG_INDEXED | HOLD_CELL_BG_INDEXED;
    if (a->attrs != b->attrs || (a->flags & mask) != (b->flags & mask)) return false;
    if (!(a->flags & HOLD_CELL_FG_DEFAULT)) {
        if (a->flags & HOLD_CELL_FG_INDEXED) {
            if (a->fg_idx != b->fg_idx) return false;
        } else if (memcmp(a->fg, b->fg, 3)) return false;
    }
    if (!(a->flags & HOLD_CELL_BG_DEFAULT)) {
        if (a->flags & HOLD_CELL_BG_INDEXED) {
            if (a->bg_idx != b->bg_idx) return false;
        } else if (memcmp(a->bg, b->bg, 3)) return false;
    }
    return true;
}

static bool cell_eq(const struct hold_cell *a, const struct hold_cell *b) {
    return a->width == b->width && !memcmp(a->ch, b->ch, sizeof(a->ch)) && pen_eq(a, b);
}

/* One SGR that sets the whole pen. A palette color is sent as its index, so
 * the person's terminal draws it in its own colors. */
static void emit_pen(struct obuf *o, const struct hold_cell *c) {
    static const struct {
        uint8_t attr;
        char code;
    } attrs[] = {
        {HOLD_ATTR_BOLD, '1'}, {HOLD_ATTR_DIM, '2'}, {HOLD_ATTR_ITALIC, '3'}, {HOLD_ATTR_UNDERLINE, '4'},
        {HOLD_ATTR_BLINK, '5'}, {HOLD_ATTR_REVERSE, '7'}, {HOLD_ATTR_CONCEAL, '8'}, {HOLD_ATTR_STRIKE, '9'},
    };
    ob_str(o, "\033[0");
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
        if (c->attrs & attrs[i].attr) ob_fmt(o, ";%c", attrs[i].code);
    if (c->flags & HOLD_CELL_FG_INDEXED) {
        if (c->fg_idx < 8) ob_fmt(o, ";3%u", c->fg_idx);
        else if (c->fg_idx < 16) ob_fmt(o, ";9%u", c->fg_idx - 8);
        else ob_fmt(o, ";38;5;%u", c->fg_idx);
    } else if (!(c->flags & HOLD_CELL_FG_DEFAULT)) {
        ob_fmt(o, ";38;2;%u;%u;%u", c->fg[0], c->fg[1], c->fg[2]);
    }
    if (c->flags & HOLD_CELL_BG_INDEXED) {
        if (c->bg_idx < 8) ob_fmt(o, ";4%u", c->bg_idx);
        else if (c->bg_idx < 16) ob_fmt(o, ";10%u", c->bg_idx - 8);
        else ob_fmt(o, ";48;5;%u", c->bg_idx);
    } else if (!(c->flags & HOLD_CELL_BG_DEFAULT)) {
        ob_fmt(o, ";48;2;%u;%u;%u", c->bg[0], c->bg[1], c->bg[2]);
    }
    ob_str(o, "m");
}

/* Every cell that differs from what the terminal shows. Each run starts with
 * an absolute cursor move, and so does whatever follows a character that is
 * not plain ASCII: terminals disagree about the width of some of those, and
 * a wrong guess must not shift the rest of the row. */
static void paint(struct compositor *c, struct obuf *o) {
    const struct hold_cell *pen = NULL;
    for (int r = 0; r < c->rows; r++) {
        const struct hold_cell *row = c->frame + (size_t)r * c->cols;
        const struct hold_cell *old = c->shown + (size_t)r * c->cols;
        int at = -1, drawn = -1;
        for (int col = 0; col < c->cols;) {
            if (cell_eq(&row[col], &old[col])) {
                col++;
                continue;
            }
            if (row[col].width == 0) {
                /* a right half is drawn by its left half */
                if (col > 0 && row[col - 1].width == 2 && drawn != col - 1) col--;
                else {
                    col++;
                    continue;
                }
            }
            const struct hold_cell *cell = &row[col];
            if (at != col) ob_fmt(o, "\033[%d;%dH", r + 1, col + 1);
            if (!pen || !pen_eq(pen, cell)) {
                emit_pen(o, cell);
                pen = cell;
            }
            bool plain = true;
            if (cell->ch[0] == 0) ob_add(o, " ", 1);
            for (int k = 0; k < HOLD_CELL_CHARS && cell->ch[k]; k++) {
                char u[4];
                size_t w = hold_utf8_encode(cell->ch[k], u, sizeof(u));
                ob_add(o, u, w);
                if (cell->ch[k] >= 0x80 || k > 0) plain = false;
            }
            drawn = col;
            col += cell->width == 2 ? 2 : 1;
            at = plain ? col : -1;
        }
    }
    if (pen) ob_str(o, "\033[0m");
}

static void sanitize_title(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++) out[k++] = ((unsigned char)*in < 0x20 || *in == 0x7f) ? ' ' : *in;
    out[k] = '\0';
}

int comp_flush(struct compositor *c, const struct comp_want *want, char **buf, size_t *len, size_t *cap) {
    if (!c->frame) return 0;
    size_t ncells = (size_t)c->rows * (size_t)c->cols;
    bool cleared = !c->shown_valid;
    if (cleared)
        for (size_t i = 0; i < ncells; i++) c->shown[i] = blank_cell;
    struct obuf cells = {0}, o = {.p = *buf, .len = *len, .cap = *cap};
    paint(c, &cells);
    bool painting = cleared || cells.len > 0;
    bool known = c->term_known;
    struct term_state *t = &c->term;

    if (painting) {
        /* One atomic update where the terminal supports it, and no cursor
         * hopping across the cells where it does not. */
        ob_str(&o, "\033[?2026h");
        if (!known || t->cursor_visible) ob_str(&o, "\033[?25l");
        t->cursor_visible = false;
        if (cleared) ob_str(&o, "\033[0m\033[H\033[2J");
        if (cells.len) ob_add(&o, cells.p, cells.len);
    }
    if (!known || t->app_cursor_keys != want->app_cursor_keys)
        ob_str(&o, want->app_cursor_keys ? "\033[?1h" : "\033[?1l");
    if (!known || t->app_keypad != want->app_keypad) ob_str(&o, want->app_keypad ? "\033=" : "\033>");
    if (!known || t->bracketed_paste != want->bracketed_paste)
        ob_str(&o, want->bracketed_paste ? "\033[?2004h" : "\033[?2004l");
    if (!known || t->focus_report != want->focus_report) ob_str(&o, want->focus_report ? "\033[?1004h" : "\033[?1004l");
    if (!known || t->mouse != want->mouse) {
        ob_str(&o, "\033[?1000l\033[?1002l\033[?1003l");
        if (want->mouse == HOLD_MOUSE_NONE) ob_str(&o, "\033[?1006l");
        else
            ob_fmt(&o, "\033[?%dh\033[?1006h",
                   want->mouse == HOLD_MOUSE_MOVE ? 1003 : want->mouse == HOLD_MOUSE_DRAG ? 1002 : 1000);
    }
    if (known ? t->cursor_style != want->cursor_style : want->cursor_style != 0)
        ob_fmt(&o, "\033[%d q", want->cursor_style);
    char title[sizeof(t->title)];
    sanitize_title(want->title, title, sizeof(title));
    if (title[0] && (!known || strcmp(title, t->title) != 0)) {
        ob_str(&o, "\033]2;");
        ob_str(&o, title);
        ob_str(&o, "\033\\");
        memcpy(t->title, title, sizeof(t->title));
    }
    if (want->bell) ob_str(&o, "\a");
    int crow = want->cursor_row < 0 ? 0 : want->cursor_row >= c->rows ? c->rows - 1 : want->cursor_row;
    int ccol = want->cursor_col < 0 ? 0 : want->cursor_col >= c->cols ? c->cols - 1 : want->cursor_col;
    if (painting || !known || t->cursor_row != crow || t->cursor_col != ccol) ob_fmt(&o, "\033[%d;%dH", crow + 1, ccol + 1);
    if (!known || t->cursor_visible != want->cursor_visible) ob_str(&o, want->cursor_visible ? "\033[?25h" : "\033[?25l");
    if (painting) ob_str(&o, "\033[?2026l");

    free(cells.p);
    *buf = o.p;
    *len = o.len;
    *cap = o.cap;
    if (cells.bad || o.bad) {
        comp_invalidate(c); /* the terminal may hold half a frame: start over next time */
        return -1;
    }
    t->app_cursor_keys = want->app_cursor_keys;
    t->app_keypad = want->app_keypad;
    t->bracketed_paste = want->bracketed_paste;
    t->focus_report = want->focus_report;
    t->mouse = want->mouse;
    t->cursor_style = want->cursor_style;
    t->cursor_row = crow;
    t->cursor_col = ccol;
    t->cursor_visible = want->cursor_visible;
    memcpy(c->shown, c->frame, ncells * sizeof(*c->shown));
    c->shown_valid = true;
    c->term_known = true;
    return 0;
}
