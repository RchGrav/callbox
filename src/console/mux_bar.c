/* console/mux_bar.c - the bar along the bottom: its items, how they shorten
 * and drop on a narrow terminal, and what a click on each does. */
#include "mux.h"

/* ---- the bar -------------------------------------------------------------
 * Every item has a full and a short form and a rank. When the terminal is
 * too narrow, items go to their short forms, least useful first, and then
 * the least useful go altogether - so what is left is always whole. */
struct bar_item {
    char full[176], brief[48];
    int rank;  /* higher stays longer */
    int group; /* 0 left, 1 middle, 2 right */
    const struct comp_style *st;
    enum bar_act act;
    int arg;
    bool use_brief, gone;
};

static int bar_width(const struct bar_item *it, int n) {
    int w = 2, last = -1; /* a margin each end */
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        w += mux_text_cols(it[i].use_brief ? it[i].brief : it[i].full);
        if (last >= 0) w += it[i].group == last ? 1 : 3; /* a space within a group, more between */
        last = it[i].group;
    }
    return w;
}

void mux_draw_bar(struct mux *m) {
    int row = m->trows - 1;
    comp_fill(m->comp, row, 0, m->tcols, &ST_BAR);
    struct bar_item it[MUX_BAR_ITEMS];
    int n = 0;
#define ITEM(g, r, s, a, x) (memset(&it[n], 0, sizeof(it[n])), it[n].group = (g), it[n].rank = (r), it[n].st = (s), \
                             it[n].act = (a), it[n].arg = (x), &it[n++])
    struct bar_item *b;
    /* left: the counters (each opens its list in the launcher), the person's
     * chat, Info */
    b = ITEM(0, 9, &ST_CNT_TTY, BA_TTY, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_tty);
    b = ITEM(0, 7, &ST_CNT_SCROLL, BA_SCROLL, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_scroll);
    b = ITEM(0, 6, &ST_CNT_ENDED, BA_ENDED, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_ended);
    if (m->chat && m->me[0]) {
        bool open = chat_visible(m->chat);
        int unread = open ? 0 : chat_unread(m->chat);
        b = ITEM(0, 10, open ? &ST_BTN_CHAT : unread ? &ST_BTN_UNREAD : &ST_BTN_OFF, BA_CHAT, 0);
        if (unread) {
            snprintf(b->full, sizeof(b->full), " %s \xe2\x97\x8f%d ", m->me, unread);
            snprintf(b->brief, sizeof(b->brief), " %.8s \xe2\x97\x8f%d ", m->me, unread);
        } else {
            snprintf(b->full, sizeof(b->full), " %s ", m->me);
            snprintf(b->brief, sizeof(b->brief), " %.8s ", m->me);
        }
    }
    b = ITEM(0, 8, g_info ? &ST_BTN_INFO : &ST_BTN_OFF, BA_INFO, 0);
    snprintf(b->full, sizeof(b->full), " Info ");
    snprintf(b->brief, sizeof(b->brief), " i ");
    /* middle: what is happening now, then the windows */
    const char *mode = m->mode == MODE_PANE     ? "PANE"
                       : m->mode == MODE_TAB    ? "WINDOW"
                       : m->mode == MODE_RESIZE ? "RESIZE"
                       : m->mode == MODE_MOVE   ? "MOVE"
                       : m->swapping            ? "SWAP"
                                                : NULL;
    if (m->lead) {
        char pk[16];
        mux_key_label(m->prefix, pk);
        b = ITEM(1, 12, &ST_MODE, BA_HELP, 0);
        snprintf(b->full, sizeof(b->full), " %s \xe2\x80\xa6 ? keys ", pk);
        snprintf(b->brief, sizeof(b->brief), " %s ", pk);
    } else if (mode) {
        b = ITEM(1, 12, &ST_MODE, BA_HELP, 0);
        snprintf(b->full, sizeof(b->full), " %s  ? keys  Esc ", mode);
        snprintf(b->brief, sizeof(b->brief), " %s ", mode);
    }
    if (m->now < m->notice_until) {
        b = ITEM(1, 11, &ST_NOTE, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "%s", m->notice);
        snprintf(b->brief, sizeof(b->brief), "%.30s", m->notice);
    }
    int last_used = -1;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++)
        if (mux_win_count(m, w) > 0) last_used = w;
    for (int w = 0; w <= last_used && n < MUX_BAR_ITEMS - 8; w++) {
        if (mux_win_count(m, w) == 0 && w != m->view) continue;
        b = ITEM(1, w == m->view ? 8 : 5, w == m->view ? &ST_WIN_CUR : &ST_WIN, BA_WINDOW, w);
        snprintf(b->full, sizeof(b->full), " %d ", mux_window_key(w));
    }
    /* right: the toggles that act on the focused pane, help, the machine */
    if (mux_win_count(m, m->view) > 1 || m->full) {
        b = ITEM(2, 4, m->full ? &ST_BTN_ON : &ST_BTN_OFF, BA_ZOOM, 0);
        snprintf(b->full, sizeof(b->full), " Zoom ");
        snprintf(b->brief, sizeof(b->brief), " Z ");
    }
    b = ITEM(2, 3, mux_pane_locked(mux_focused(m)) ? &ST_BTN_ON : &ST_BTN_OFF, BA_LOCK, 0);
    snprintf(b->full, sizeof(b->full), " Lock ");
    snprintf(b->brief, sizeof(b->brief), " L ");
    b = ITEM(2, 6, &ST_BTN_OFF, BA_HELP, 0);
    snprintf(b->full, sizeof(b->full), " ? ");
    if (m->sys_cpu >= 0) {
        b = ITEM(2, 5, &ST_BAR, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "CPU %2d%%", m->sys_cpu);
        snprintf(b->brief, sizeof(b->brief), "%d%%", m->sys_cpu);
    }
    if (m->mem_total) {
        char used[16], total[16];
        mux_human_bytes(m->mem_used, used, sizeof(used));
        mux_human_bytes(m->mem_total, total, sizeof(total));
        b = ITEM(2, 5, &ST_BAR, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "MEM %s/%s", used, total);
        snprintf(b->brief, sizeof(b->brief), "%s", used);
    }
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        b = ITEM(2, 2, &ST_BAR_DIM, BA_NONE, 0);
        strftime(b->full, sizeof(b->full), "%H:%M", &tmv);
    }
#undef ITEM
    /* fit: short forms first, least useful first; then drop the least useful */
    for (int rank = 0; rank <= 12 && bar_width(it, n) > m->tcols; rank++)
        for (int i = 0; i < n && bar_width(it, n) > m->tcols; i++)
            if (it[i].rank == rank && it[i].brief[0]) it[i].use_brief = true;
    for (int rank = 0; rank <= 12 && bar_width(it, n) > m->tcols; rank++)
        for (int i = n - 1; i >= 0 && bar_width(it, n) > m->tcols; i--)
            if (it[i].rank == rank) it[i].gone = true;
    /* left from the left edge, right against the right one, the middle
     * centered between them */
    int wl = 0, wm = 0, wr = 0, cl = 0, cm = 0, cr = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        int w = mux_text_cols(it[i].use_brief ? it[i].brief : it[i].full);
        if (it[i].group == 0) wl += w + (cl++ ? 1 : 0);
        else if (it[i].group == 1) wm += w + (cm++ ? 1 : 0);
        else wr += w + (cr++ ? 1 : 0);
    }
    int xl = 1, xr = m->tcols - 1 - wr;
    int xm = xl + wl + ((xr - (xl + wl)) - wm) / 2;
    if (xm < xl + wl + (cl ? 2 : 0)) xm = xl + wl + (cl ? 2 : 0);
    m->nhits = 0;
    int x[3] = {xl, xm, xr};
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        const char *t = it[i].use_brief ? it[i].brief : it[i].full;
        int g = it[i].group, w = mux_text_cols(t);
        mux_put(m, row, x[g], w, t, it[i].st);
        if (it[i].act != BA_NONE && m->nhits < MUX_BAR_ITEMS)
            m->hits[m->nhits++] = (struct bar_hit){x[g], x[g] + w, it[i].act, it[i].arg};
        x[g] += w + 1;
    }
}

/* A button on the bar: what its key does. */
void mux_bar_click(struct mux *m, enum bar_act act, int arg) {
    m->help = false;
    m->dirty = true;
    switch (act) {
    case BA_TTY: case BA_SCROLL: case BA_ENDED:
        m->fl = FL_NONE;
        m->pick_cat = act == BA_TTY ? PK_TTY : act == BA_SCROLL ? PK_SCROLL : PK_ENDED;
        mux_open_floater(m, FL_PROCS);
        return;
    case BA_CHAT: /* the person's name: chat open, or put away */
        m->fl = FL_NONE;
        if (m->chat && chat_visible(m->chat)) chat_put_away(m->chat);
        else mux_chat_key_pressed(m);
        return;
    case BA_INFO: (void)mux_command(m, 'i'); return;
    case BA_WINDOW:
        if (mux_win_count(m, arg) > 0) mux_set_focus(m, arg * 4);
        return;
    case BA_ZOOM: (void)mux_command(m, 'z'); return;
    case BA_LOCK: (void)mux_command(m, '\\'); return;
    case BA_HELP:
        m->fl = FL_NONE;
        m->help = true;
        return;
    default: return;
    }
}
