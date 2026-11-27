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
    /* the windows in view: one, two side by side or stacked, or four */
    bool in_view[MUX_MAX_WINDOWS] = {false};
    for (int q = 0; q < 4; q++) in_view[((m->vy + q / 2) / 2) * 3 + (m->vx + q % 2) / 2] = true;
    /* where the view is on the 3x3 grid: a dot for each window in view, in
     * two braille cells (columns 1-2, then 3) */
    unsigned dots[2] = {0, 0};
    static const unsigned bit[2][3] = {{0x01, 0x02, 0x04}, {0x08, 0x10, 0x20}};
    for (int w = 0; w < MUX_MAX_WINDOWS; w++)
        if (in_view[w]) dots[(w % 3) / 2] |= bit[(w % 3) % 2][w / 3];
    b = ITEM(1, 7, &ST_WIN, BA_NONE, 0);
    char *o = b->full;
    for (int k = 0; k < 2; k++) {
        uint32_t cp = 0x2800 + dots[k];
        o += hold_utf8_encode(cp, o, 4);
    }
    *o = '\0';
    for (int w = 0; w < MUX_MAX_WINDOWS && n < MUX_BAR_ITEMS - 10; w++) {
        if (mux_window_count(m->shown, w) == 0 && !in_view[w]) continue;
        b = ITEM(1, in_view[w] ? 8 : 5, in_view[w] ? &ST_WIN_CUR : &ST_WIN, BA_WINDOW, w);
        snprintf(b->full, sizeof(b->full), " %d ", mux_window_key(w));
    }
    /* the view: sorted by name, spread or packed (each on until clicked off) */
    b = ITEM(1, 4, m->sort ? &ST_BTN_ON : &ST_BTN_OFF, BA_SORT, 0);
    snprintf(b->full, sizeof(b->full), " Sort ");
    snprintf(b->brief, sizeof(b->brief), " S ");
    b = ITEM(1, 4, m->arr == ARR_SPREAD ? &ST_BTN_ON : &ST_BTN_OFF, BA_ARR, 'w');
    snprintf(b->full, sizeof(b->full), " Spread ");
    snprintf(b->brief, sizeof(b->brief), " W ");
    b = ITEM(1, 4, m->arr == ARR_PACK ? &ST_BTN_ON : &ST_BTN_OFF, BA_ARR, 'd');
    snprintf(b->full, sizeof(b->full), " Pack ");
    snprintf(b->brief, sizeof(b->brief), " P ");
    /* right: the toggles that act on the focused pane, help, the machine */
    int on_screen = 0; /* panes in view, each once */
    for (int c = 0; c < 4; c++) {
        bool seen = false;
        for (int d = 0; d < c; d++) seen = seen || m->cell[d] == m->cell[c];
        on_screen += m->cell[c] && !seen;
    }
    if (on_screen > 1 || m->full) {
        b = ITEM(2, 4, m->full ? &ST_BTN_ON : &ST_BTN_OFF, BA_ZOOM, 0);
        snprintf(b->full, sizeof(b->full), " Zoom ");
        snprintf(b->brief, sizeof(b->brief), " Z ");
    }
    b = ITEM(2, 3, mux_pane_locked(m->active) ? &ST_BTN_ON : &ST_BTN_OFF, BA_LOCK, 0);
    snprintf(b->full, sizeof(b->full), " Lock ");
    snprintf(b->brief, sizeof(b->brief), " L ");
    b = ITEM(2, 1, g_slide ? &ST_BTN_ON : &ST_BTN_OFF, BA_SLIDE, 0);
    snprintf(b->full, sizeof(b->full), " Slide ");
    snprintf(b->brief, sizeof(b->brief), " ~ ");
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
        if (mux_window_count(m->shown, arg) > 0) mux_align(m, arg);
        if (m->cell[0] && !(m->active && m->active->visible)) mux_set_focus(m, m->cell[0]);
        return;
    case BA_SORT: (void)mux_command(m, 'a'); return;
    case BA_ARR: /* Spread or Pack: on, or off (the base again) */
        (void)mux_command(m, (unsigned char)arg);
        return;
    case BA_ZOOM: (void)mux_command(m, 'z'); return;
    case BA_LOCK: (void)mux_command(m, '\\'); return;
    case BA_SLIDE: /* the slide on or off, kept in the config for next time */
        g_slide = !g_slide;
        if (hold_console_home()) (void)hold_settings_put(hold_console_home(), "slide", g_slide ? "on" : "off");
        mux_notice(m, "slide %s", g_slide ? "on" : "off");
        return;
    case BA_HELP:
        m->fl = FL_NONE;
        m->help = true;
        return;
    default: return;
    }
}
