/* console/mux_input.c - the terminal's input: keys split from escape
 * sequences, the prefix, the mouse, and keys passed to the focused pane. */
#include "mux.h"

/* A pointer report, SGR form: b;x;y then M (press, motion) or m (release). */
void mux_handle_mouse(struct mux *m, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1;
    bool motion = (b & 32) != 0;
    bool wheel = (b & 64) != 0;
    int button = b & 3;
    bool press = !motion && !release && !wheel;
    /* the bar: a press on a button does what its key does, whatever else is up */
    if (mux_bar_on(m) && row == m->trows - 1) {
        if (!press) return;
        for (int i = 0; i < m->nhits; i++)
            if (col >= m->hits[i].x0 && col < m->hits[i].x1) {
                m->prompt = PROMPT_NONE; /* a click elsewhere keeps the name as it was */
                mux_bar_click(m, m->hits[i].act, m->hits[i].arg);
                return;
            }
        return;
    }
    if (m->prompt != PROMPT_NONE) {
        /* typing a name: a click in its box keeps typing; anywhere else keeps
         * the name as it was */
        struct pane *pp = m->prompt_pane;
        bool in_box = pp && row == pp->brow && col >= pp->box_col && col < pp->box_col + pp->box_w;
        if (press && !in_box) {
            m->prompt = PROMPT_NONE;
            m->prompt_pane = NULL;
            m->dirty = true;
        }
        return;
    }
    if (m->swapping) {
        /* a click on a pane picks it, as its place would */
        struct pane *sp = mux_pane_at(m, row, col);
        if (!motion && !release && !wheel && sp) mux_swap_choose(m, mux_base_slot(m, sp));
        return;
    }
    if (m->fl == FL_JUMP) {
        /* a click anywhere puts the list away */
        if (!motion && !release) {
            m->fl = FL_NONE;
            m->dirty = true;
        }
        return;
    } else if (m->fl == FL_PROCS) {
        /* a click on a list's name shows that list; on an entry, opens it;
         * anywhere else closes the launcher */
        if (motion || release) return;
        if (row == m->pick_tabs_row && col >= m->pick_tab_x[0] && col < m->pick_tab_x[PK_CATS]) {
            for (int c = 0; c < PK_CATS; c++)
                if (col >= m->pick_tab_x[c] && col < m->pick_tab_x[c + 1]) {
                    m->pick_cat = (enum pick_cat)c;
                    mux_load_picks(m);
                }
            m->dirty = true;
            return;
        }
        int k = row - m->pick_top + m->pick_first;
        if (row >= m->pick_top && row < m->pick_top + m->pick_rows && col >= m->pick_x && col < m->pick_x + m->pick_w &&
            k >= 0 && k < m->npicks) {
            static const unsigned char enter = '\r';
            m->pick_sel = k;
            mux_floater_key(m, &enter, 1);
        } else {
            m->fl = FL_NONE;
            m->dirty = true;
        }
        return;
    } else if (m->fl != FL_NONE) {
        return;
    }
    /* A click changes focus; pointer motion alone does not. A click on a
     * name box types a new name over it. */
    struct pane *p = mux_pane_at(m, row, col);
    if (press && p && mux_info_on(m) && row == p->brow && p->brow == p->row - 1) {
        if (p != m->active) mux_set_focus(m, p);
        if (col >= p->box_col && col < p->box_col + p->box_w && p->id[0]) mux_prompt_open(m, PROMPT_RENAME);
        return; /* the name bar is not the program's */
    }
    if (press && p && p != m->active) {
        mux_set_focus(m, p);
        return; /* the click that moves focus is not the program's */
    }
    if (!p || p != m->active) return;
    if (row < p->row || row >= p->row + p->rows) return; /* its name bar is not the program's */
    struct hold_screen_modes modes;
    hold_screen_modes(p->scr, &modes);
    if (modes.mouse == HOLD_MOUSE_NONE) return;
    if (motion && modes.mouse == HOLD_MOUSE_CLICK) return;
    if (motion && modes.mouse == HOLD_MOUSE_DRAG && button == 3) return;
    enum hold_mouse_event ev = wheel ? (button == 0 ? HOLD_MOUSE_WHEEL_UP : HOLD_MOUSE_WHEEL_DOWN)
                               : motion ? HOLD_MOUSE_MOTION : release ? HOLD_MOUSE_RELEASE : HOLD_MOUSE_PRESS;
    int mods = ((b & 4) ? HOLD_MOD_SHIFT : 0) | ((b & 8) ? HOLD_MOD_ALT : 0) | ((b & 16) ? HOLD_MOD_CTRL : 0);
    hold_screen_discard_output(p->scr);
    hold_screen_mouse_event(p->scr, ev, button == 3 ? 0 : button + 1, row - p->row, col - p->col, mods);
    char enc[64];
    size_t k = hold_screen_take_output(p->scr, enc, sizeof(enc));
    if (k) mux_pane_send(p, enc, k);
}

static void flush_keys(struct mux *m) {
    struct pane *p = m->active;
    if (m->out_len && p) mux_pane_send(p, m->out, m->out_len);
    m->out_len = 0;
}

void mux_to_pane(struct mux *m, const unsigned char *b, size_t n) {
    if (m->out_len + n > sizeof(m->out)) flush_keys(m);
    if (n > sizeof(m->out)) {
        struct pane *p = m->active;
        if (p) mux_pane_send(p, b, n);
        return;
    }
    memcpy(m->out + m->out_len, b, n);
    m->out_len += n;
}

/* Length of a recognized escape sequence, or 0 when incomplete. A lone
 * ESC at a read boundary is treated as the Esc key. */
static size_t esc_len(const unsigned char *s, size_t n, bool at_end) {
    if (n < 2) return at_end ? 1 : 0;
    if (s[1] == '[') {
        for (size_t i = 2; i < n; i++)
            if (s[i] >= 0x40 && s[i] <= 0x7e) return i + 1;
        return n >= 32 ? n : 0;
    }
    if (s[1] == 'O') return n >= 3 ? 3 : 0;
    if (s[1] == 0x1b && n >= 3 && s[2] == '[') { /* Esc-prefixed arrow: Alt on some terminals */
        size_t k = esc_len(s + 1, n - 1, at_end);
        return k ? k + 1 : 0;
    }
    return 2; /* Alt and a key */
}

/* One key, not the prefix's: whoever has the keyboard gets it. */
static void key_in(struct mux *m, const unsigned char *k, size_t n) {
    if (m->prompt != PROMPT_NONE) {
        mux_prompt_key(m, k, n);
    } else if (m->swapping) {
        mux_swap_key(m, k, n);
    } else if (m->fl != FL_NONE) {
        mux_floater_key(m, k, n);
    } else if (m->mode != MODE_NONE) {
        mux_mode_key(m, k, n);
    } else if (m->help) {
        m->help = false; /* any key puts the keys away */
        m->dirty = true;
    } else if (m->chat && chat_has_keyboard(m->chat)) {
        chat_input(m->chat, k, n, m->now);
    } else {
        /* Alt-arrows move between panes without the prefix, and past the
         * screen's edge the view slides half a window that way, across the
         * grid of windows - unless the pane is locked (its program gets
         * them), or the grid ends there (then they are the program's too). */
        int mod = 1;
        char dir = mux_arrow_of(k, n, &mod, false);
        bool mine = dir && mod == 3 && !mux_pane_locked(m->active);
        struct pane *to = mine ? mux_neighbor(m, dir) : NULL;
        if (to) {
            flush_keys(m);
            mux_set_focus(m, to);
        } else if (mine && (flush_keys(m), mux_shift(m, dir))) {
            /* moved: the view went that way */
        } else {
            mux_to_pane(m, k, n);
        }
    }
}

/* Returns 1 to detach. */
int mux_handle_input(struct mux *m, const unsigned char *buf, size_t n) {
    unsigned char all[4096 + 64];
    size_t total = 0;
    memcpy(all, m->esc, m->esc_len);
    total = m->esc_len;
    size_t take = n < sizeof(all) - total ? n : sizeof(all) - total;
    memcpy(all + total, buf, take);
    total += take;
    m->esc_len = 0;
    int rc = 0;
    for (size_t i = 0; i < total && !rc;) {
        unsigned char b = all[i];
        if (b == 0x1b) {
            size_t k = esc_len(all + i, total - i, true);
            if (k == 0) {
                size_t left = total - i < sizeof(m->esc) ? total - i : sizeof(m->esc);
                memcpy(m->esc, all + i, left);
                m->esc_len = left;
                break;
            }
            const unsigned char *s = all + i;
            i += k;
            /* a pointer report: always ours to route, and never the key the
             * prefix waits for */
            if (k > 3 && s[1] == '[' && s[2] == '<') {
                int mb = 0, mx = 0, my = 0;
                char tmp[40];
                size_t tl = k - 3 < sizeof(tmp) - 1 ? k - 3 : sizeof(tmp) - 1;
                memcpy(tmp, s + 3, tl);
                tmp[tl] = '\0';
                if (sscanf(tmp, "%d;%d;%d", &mb, &mx, &my) == 3) {
                    flush_keys(m);
                    /* chat picks out what is its own (its window, a drag
                     * of it) and hands the rest back */
                    if (m->fl == FL_NONE && !m->swapping && m->prompt == PROMPT_NONE && m->chat)
                        chat_input(m->chat, s, k, m->now);
                    else mux_handle_mouse(m, mb, mx, my, s[k - 1] == 'm');
                }
                continue;
            }
            if (m->lead) {
                m->lead = false; /* a sequence after the prefix: cancelled */
                m->dirty = true;
                continue;
            }
            key_in(m, s, k);
            continue;
        }
        size_t k = 1;
        if (b >= 0xc0) { /* a UTF-8 character, whole */
            k = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
            if (k > total - i) k = total - i;
        }
        const unsigned char *s = all + i;
        i += k;
        if (m->lead) {
            m->lead = false;
            m->dirty = true;
            flush_keys(m);
            rc = k == 1 ? mux_command(m, b) : 0;
            continue;
        }
        if (k == 1 && b == m->prefix) {
            m->lead = true; /* waits for the next key, however long it takes */
            continue;
        }
        key_in(m, s, k);
    }
    flush_keys(m);
    if (m->chat && chat_wants_frame(m->chat)) m->dirty = true;
    return rc;
}
