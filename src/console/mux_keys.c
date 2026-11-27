/* console/mux_keys.c - the prefix's commands, the modes, swap, the name
 * prompt, and the launcher's and pane list's keys. */
#include "mux.h"

void mux_prompt_open(struct mux *m, enum prompt kind) {
    struct pane *p = mux_focused(m);
    if (!p || !p->id[0]) return;
    m->prompt = kind;
    m->prompt_pane = p;
    snprintf(m->in, sizeof(m->in), "%s", p->name);
    m->in_len = strlen(m->in);
    m->in_fresh = true; /* the first key typed replaces the name; Enter or Esc keeps it */
    m->dirty = true;
}

/* A changed name uses rename, which also saves the profile. An unchanged
 * save prompt uses save; an unchanged rename prompt has no effect. */
static void prompt_done(struct mux *m) {
    struct pane *p = m->prompt_pane;
    enum prompt kind = m->prompt;
    m->prompt = PROMPT_NONE;
    m->prompt_pane = NULL;
    if (!p || mux_slot_of(m, p) < 0) return;
    m->in[m->in_len] = '\0';
    bool changed = m->in_len > 0 && strcmp(m->in, p->name) != 0;
    char err[200];
    if (changed) {
        char *args[] = {"rename", p->id, m->in, NULL};
        if (mux_run_self(args, err, sizeof(err), NULL, 0) != 0) {
            mux_notice(m, "not renamed: %s", err[0] ? err : "backmux rename failed");
            return;
        }
        snprintf(p->name, sizeof(p->name), "%s", m->in);
        if (m->chat && p == mux_focused(m)) chat_set_screen(m->chat, p->scr, p->name);
        m->advertise = true;
        mux_notice(m, kind == PROMPT_SAVE ? "saved as %s" : "renamed to %s", p->name);
        return;
    }
    if (kind == PROMPT_SAVE) {
        char *args[] = {"save", p->id, NULL};
        if (mux_run_self(args, err, sizeof(err), NULL, 0) != 0) mux_notice(m, "not saved: %s", err[0] ? err : "backmux save failed");
        else mux_notice(m, "saved");
    }
}

void mux_prompt_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && k[0] == 0x1b) { /* Esc: nothing renamed, nothing saved */
        m->prompt = PROMPT_NONE;
        m->prompt_pane = NULL;
        return;
    }
    if (n == 1 && (k[0] == '\r' || k[0] == '\n')) {
        prompt_done(m);
        return;
    }
    if (n == 1 && (k[0] == 0x7f || k[0] == 0x08)) {
        m->in_fresh = false; /* Backspace edits the name rather than replacing it */
        if (m->in_len > 0) m->in[--m->in_len] = '\0';
        return;
    }
    if (n == 1 && k[0] == 0x15) { /* Ctrl-U */
        m->in_len = 0;
        m->in[0] = '\0';
        return;
    }
    /* a name is short printable text: anything else is not part of one */
    for (size_t i = 0; i < n; i++)
        if (k[i] < 0x21 || k[i] == 0x7f) return;
    if (m->in_fresh) {
        m->in_fresh = false;
        m->in_len = 0;
        m->in[0] = '\0';
    }
    if (m->in_len + n < sizeof(m->in) && m->in_len + n <= ALIAS_MAX_LEN) {
        memcpy(m->in + m->in_len, k, n);
        m->in_len += n;
        m->in[m->in_len] = '\0';
    }
}

void mux_swap_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && (k[0] == 0x1b || k[0] == '\r' || k[0] == '\n')) {
        m->swapping = false;
        return;
    }
    if (n != 1) return;
    int w = mux_window_of_key(k[0]);
    if (w >= 0) { /* a number sends the pane to that window */
        int from = m->swap_first >= 0 ? m->swap_first : m->focus;
        struct pane *p = m->slot[from];
        if (mux_move_pane(m, from, w) != 0) mux_notice(m, "window %d is full", mux_window_key(w));
        else mux_follow_here(m, p);
        m->swapping = false;
        return;
    }
    int s = mux_letter_slot(k[0]);
    if (s < 0) return;
    if (m->swap_first < 0) {
        if (m->slot[s]) m->swap_first = s; /* the first is a pane */
        return;
    }
    struct pane *p = m->slot[m->swap_first];
    if (m->slot[s]) {
        mux_do_swap(m, m->swap_first, s);
    } else if (mux_move_pane(m, m->swap_first, s / 4) != 0) { /* past the panes there: to that window */
        mux_notice(m, "window %d is full", mux_window_key(s / 4));
    }
    mux_follow_here(m, p);
    m->swapping = false;
}

/* An arrow, plain or with a modifier: 'A' up 'B' down 'C' right 'D' left,
 * *mod 1 plain, 2 Shift, 3 Alt; h j k l as arrows (H J K L with Shift). 0 if
 * not one. */
char mux_arrow_of(const unsigned char *k, size_t n, int *mod, bool letters) {
    *mod = 1;
    if (n == 3 && k[0] == 0x1b && (k[1] == '[' || k[1] == 'O') && k[2] >= 'A' && k[2] <= 'D') return (char)k[2];
    if (n == 6 && k[0] == 0x1b && k[1] == '[' && k[2] == '1' && k[3] == ';' && k[5] >= 'A' && k[5] <= 'D') {
        *mod = k[4] - '0';
        return (char)k[5];
    }
    if (n == 4 && k[0] == 0x1b && k[1] == 0x1b && k[2] == '[' && k[3] >= 'A' && k[3] <= 'D') { /* Esc-prefixed: Alt */
        *mod = 3;
        return (char)k[3];
    }
    if (!letters || n != 1) return 0;
    static const char from[] = "hjklHJKL", to[] = "DBACDBAC";
    const char *hit = strchr(from, k[0]);
    if (!hit || !k[0]) return 0;
    *mod = hit - from >= 4 ? 2 : 1;
    return to[hit - from];
}

static void leave_mode(struct mux *m) {
    m->mode = MODE_NONE;
    m->help = false;
    m->dirty = true;
}

void mux_mode_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && (k[0] == 0x1b || k[0] == '\r' || k[0] == '\n')) {
        if (m->help && k[0] == 0x1b) m->help = false; /* Close help before leaving the mode. */
        else leave_mode(m);
        return;
    }
    if (n == 1 && k[0] == '?') {
        m->help = !m->help;
        return;
    }
    m->help = false;
    int mod = 1;
    char dir = mux_arrow_of(k, n, &mod, true);
    int w = n == 1 ? mux_window_of_key(k[0]) : -1;
    struct pane *f = mux_focused(m);
    switch (m->mode) {
    case MODE_PANE:
        if (dir) {
            int s = mux_neighbor(m, dir);
            if (s >= 0) mux_set_focus(m, s);
        } else if (n == 1 && k[0] == 'a') {
            leave_mode(m);
            m->pick_cat = PK_TTY;
            mux_open_floater(m, FL_PROCS);
        } else if (n == 1 && k[0] == 'g') {
            leave_mode(m);
            mux_open_floater(m, FL_JUMP);
        } else if (n == 1 && k[0] == 'z' && f && mux_win_count(m, m->view) > 1) {
            m->full = m->full == f ? NULL : f;
            mux_relayout(m);
        }
        return;
    case MODE_TAB:
        if (w >= 0) {
            if (mux_win_count(m, w) == 0) mux_notice(m, "window %d is empty", mux_window_key(w));
            else {
                mux_set_focus(m, w * 4);
                leave_mode(m);
            }
        } else if (dir == 'D' || dir == 'C') {
            int to = dir == 'D' ? mux_prev_window(m, m->view) : mux_next_window(m, m->view);
            mux_set_focus(m, to * 4);
        }
        return;
    case MODE_RESIZE:
        if (dir) mux_resize_toward(m, dir, mod != 2);
        else if (n == 1 && k[0] == '=') {
            m->colpct[m->view] = m->lpct[m->view] = m->rpct[m->view] = 0;
            mux_relayout(m);
        }
        return;
    case MODE_MOVE:
        if (dir) {
            int s = mux_neighbor(m, dir);
            if (s >= 0 && f) {
                mux_do_swap(m, m->focus, s);
                mux_follow(m, f);
            }
        } else if (w >= 0 && f) {
            if (mux_move_pane(m, m->focus, w) != 0) mux_notice(m, "window %d is full", mux_window_key(w));
            else {
                mux_follow_here(m, f);
                leave_mode(m);
            }
        }
        return;
    default:
        return;
    }
}

/* Handle input for the launcher or pane-name picker. seq is one byte or one
 * complete escape sequence. */
void mux_floater_key(struct mux *m, const unsigned char *seq, size_t n) {
    bool esc = n == 1 && seq[0] == 0x1b;
    bool up = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'A';
    bool down = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'B';
    bool enter = n == 1 && (seq[0] == '\r' || seq[0] == '\n');
    m->dirty = true;
    if (esc) {
        m->fl = FL_NONE;
        return;
    }
    bool left = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'D';
    bool right = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'C';
    bool tab = n == 1 && seq[0] == '\t';
    switch (m->fl) {
    case FL_PROCS:
        if (left || right || tab) {
            m->pick_cat = (enum pick_cat)((m->pick_cat + (left ? PK_CATS - 1 : 1)) % PK_CATS);
            mux_load_picks(m);
        } else if (up && m->pick_sel > 0) {
            m->pick_sel--;
        } else if (down && m->pick_sel + 1 < m->npicks) {
            m->pick_sel++;
        } else if (enter && m->npicks > 0) {
            struct pick p = m->picks[m->pick_sel];
            m->fl = FL_NONE;
            mux_launch_pick(m, &p);
        }
        return;
    case FL_JUMP: {
        int rows = 0;
        for (int i = 0; i < MUX_MAX_PANES; i++) rows += m->slot[i] != NULL;
        if (up && m->pick_sel > 0) m->pick_sel--;
        else if (down && m->pick_sel + 1 < rows) m->pick_sel++;
        else if (enter) {
            for (int i = 0, k = 0; i < MUX_MAX_PANES; i++) {
                if (!m->slot[i]) continue;
                if (k++ == m->pick_sel) mux_set_focus(m, i);
            }
            m->fl = FL_NONE;
        }
        return;
    }
    default:
        return;
    }
}

void mux_open_floater(struct mux *m, enum floater f) {
    if (m->chat && f != FL_NONE) chat_put_away(m->chat); /* one floating window at a time */
    /* The attach picker is the way to a split, and a split takes the whole
     * screen: inline, it goes to the alternate screen before it draws, so
     * the person's own screen - their lines above the launch, which no pane
     * holds - is never drawn over and comes back as it was on leaving. */
    if (f == FL_PROCS && m->assumed) mux_go_alt(m);
    m->pick_sel = 0;
    if (f == FL_PROCS) mux_load_picks(m);
    m->fl = f;
    m->dirty = true;
}

/* Dispatch a key following the prefix. Return 1 to detach the last pane.
 * Unrecognized command keys are consumed; repeating the prefix forwards it. */
int mux_command(struct mux *m, unsigned char key) {
    struct pane *f = mux_focused(m);
    m->help = false;
    m->dirty = true;
    if (key == m->prefix) { /* the prefix twice: one is the program's, locked or not */
        mux_to_pane(m, &key, 1);
        return 0;
    }
    if (key == m->detach_key) {
        /* Detach acts on the active pane, as every pane key does: the keys go
         * to its broker, as plain attach sends them. The broker closes the
         * connection - the pane lets go and the rest close up; the last one
         * leaves the terminal - or, when an attach runs nested inside that
         * process (backmux attach typed in a shell), hands the keys inward:
         * the innermost attach, the one on screen, lets go first. */
        if (!f) return 0;
        (void)mux_frame(f->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
        f->detach_ms = m->now ? m->now : 1;
        return 0;
    }
    switch (key) {
    case 'c':
        mux_chat_key_pressed(m);
        return 0;
    case '?':
        m->help = true;
        return 0;
    case 's':
        m->swapping = true;
        m->swap_first = -1;
        return 0;
    case 'z':
        if (f && mux_win_count(m, m->view) > 1) {
            m->full = m->full == f ? NULL : f;
            mux_relayout(m);
        }
        return 0;
    case 'f': /* the bar */
        g_bar = !g_bar;
        mux_relayout(m);
        return 0;
    case 'i': /* Info: the name bars */
        g_info = !g_info;
        mux_relayout(m);
        return 0;
    case 'o': /* the launcher */
        m->pick_cat = PK_TTY;
        mux_open_floater(m, FL_PROCS);
        return 0;
    case 'n':
        mux_prompt_open(m, PROMPT_RENAME);
        return 0;
    case ';':
        mux_prompt_open(m, PROMPT_SAVE);
        return 0;
    case '\\':
        if (f) mux_set_locked(f, !mux_pane_locked(f));
        return 0;
    case 'w':
        mux_arrange(m, ARRANGE_SPREAD);
        return 0;
    case 'a':
        mux_arrange(m, ARRANGE_ALPHA);
        return 0;
    case 'd':
        mux_arrange(m, ARRANGE_DENSE);
        return 0;
    case 'p':
        m->mode = MODE_PANE;
        return 0;
    case 't':
        m->mode = MODE_TAB;
        return 0;
    case 'r':
        m->mode = MODE_RESIZE;
        return 0;
    case 'm':
        m->mode = MODE_MOVE;
        return 0;
    default:
        return 0;
    }
}
