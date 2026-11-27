/* console/mux.c - the compositor's loop: panes read, input dispatched,
 * frames drawn, until the last pane goes. The rest of the compositor is the
 * mux_*.c files; mux.h says which does what. */
#include "mux.h"

bool g_bar = true;  /* prefix f: the bar along the bottom */
bool g_info = true; /* prefix i, or Info on the bar: every pane's name bar and stats */
char g_locked[MUX_MAX_PANES][ID_STR_LEN];

/* ---- the loop ---------------------------------------------------------- */

/* The connection closing just after the detach keys is the detach (as in
 * plain attach, attach.c). */
#define MUX_DETACH_MS 2000

static void drop_ended(struct mux *m) {
    bool any = false;
    for (int i = 0; i < MUX_MAX_PANES; i++) {
        struct pane *p = m->slot[i];
        if (p && p->ended) {
            bool detached = p->detach_ms && m->now - p->detach_ms < MUX_DETACH_MS;
            if (detached && mux_pane_total(m) == 1) {
                /* the last pane let go: the terminal is left, and says so */
                hold_console_note_detached(NULL);
                hold_console_note_detached(p->name);
            }
            /* A missing endpoint confirms console closure, not process exit.
             * An existing endpoint may belong to a replacement attach. */
            else if (!detached && !hold_path_exists(p->sock)) mux_notice(m, "%s: console closed", p->name);
            mux_pane_free(p);
            m->slot[i] = NULL;
            any = true;
        }
    }
    if (any) mux_settle(m);
}

int hold_mux_run(const struct hold_store *store, const char *chat_name, const char *first_sock,
                 unsigned char cmd, const unsigned char *rest, size_t rest_len, char *handback, size_t hn,
                 bool *handed_back) {
    *handed_back = false;
    struct mux *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    m->store = store;
    snprintf(m->me, sizeof(m->me), "%s", chat_name ? chat_name : "");
    unsigned char keys[8];
    bool two = hold_console_get_detach_keys(keys, sizeof(keys)) == 2;
    m->prefix = two ? keys[0] : KEY_PREFIX;
    m->detach_key = two ? keys[1] : 'q';
    m->comp = comp_new();
    m->swap_first = -1;
    m->now = mux_mono_ms();
    m->resident = hold_console_is_resident();
    m->sys_cpu = -1;
    struct winsize ws;
    if (hold_term_size(&ws) == 0 && ws.ws_row && ws.ws_col) {
        m->trows = ws.ws_row;
        m->tcols = ws.ws_col;
    } else {
        m->trows = 24;
        m->tcols = 80;
    }
    m->arows = m->trows;
    comp_set_size(m->comp, m->trows, m->tcols);
    comp_invalidate(m->comp);
    if (m->me[0]) m->inbox_off = hold_inbox_size(store, m->me);
    if (m->me[0]) {
        struct chat_host host = {
            .screen = NULL, .master = -1, .store = store, .run_id = NULL, .program = true,
            .person = m->me, .app = m, .to_app = mux_chat_to_app, .app_pointer = mux_chat_pointer,
            .lead = m->prefix, .key = 'c', /* what its tips name: Ctrl-Space c */
        };
        m->chat = chat_new(&host);
        if (m->chat) {
            chat_set_on_sent(m->chat, mux_on_sent);
            chat_ready(m->chat, m->trows, m->tcols, m->now);
        }
    }
    int rc = 0;

    /* the process this started on: its record names it */
    struct find_first {
        const char *sock;
        char id[ID_STR_LEN], name[ALIAS_MAX_LEN + 1];
    } ff = {.sock = first_sock};
    struct hold_run_record r;
    char dir_path[HOLD_PATH_MAX];
    (void)dir_path;
    {
        DIR *d = opendir(store->record_dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".json") || l - 5 >= sizeof(ff.id)) continue;
            char id[ID_STR_LEN];
            snprintf(id, sizeof(id), "%.*s", (int)(l - 5), e->d_name);
            char path[HOLD_PATH_MAX];
            if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) continue;
            if (r.has_console && !strcmp(r.console_sock, first_sock)) {
                snprintf(ff.id, sizeof(ff.id), "%s", r.id);
                snprintf(ff.name, sizeof(ff.name), "%s", r.has_name ? r.name : "");
                if (!ff.name[0]) snprintf(ff.name, sizeof(ff.name), "%.12s", r.id);
            }
            hold_free_run_record(&r);
            if (ff.id[0]) break;
        }
        if (d) closedir(d);
    }
    const char *why = NULL;
    struct pane *first = mux_pane_open(ff.id[0] ? ff.id : "", ff.name[0] ? ff.name : "process", first_sock, m->trows, m->tcols, &why);
    if (!first) {
        free(m);
        return 3;
    }
    (void)mux_add_pane(m, first);
    mux_pane_identify(m, first);
    mux_refresh_stats(m);
    mux_relayout(m); /* resident: the bar and the name bar from the first frame */
    if (!first->agent[0]) m->app = first;
    if (m->chat) chat_set_screen(m->chat, first->scr, first->name);
    /* Inline, the terminal shows this pane's screen already (and the
     * person's lines above where the launch began): start from that, so
     * chat paints only its own window instead of wiping the screen. */
    if (hold_console_was_inline()) {
        struct hold_snapshot snap;
        if (hold_screen_snapshot(first->scr, &snap) == 0) {
            comp_assume(m->comp, &snap);
            hold_snapshot_free(&snap);
            m->assumed = true;
        }
    }
    struct hold_term_signals sig;
    hold_term_signals_install(&sig, true);
    if (mux_command(m, cmd) == 1) goto detach;
    /* backmux shell starts with chat up but the keyboard on the shell: it is
     * the person's shell they came to type in */
    if (m->resident && m->chat && chat_has_keyboard(m->chat)) chat_toggle(m->chat);
    if (rest_len && mux_handle_input(m, rest, rest_len) == 1) goto detach;

    for (;;) {
        m->now = mux_mono_ms();
        if (hold_term_stop_signal()) {
            rc = 128 + hold_term_stop_signal();
            break;
        }
        if (hold_term_take_resized() && hold_term_size(&ws) == 0 && ws.ws_row && ws.ws_col) {
            m->trows = ws.ws_row;
            m->tcols = ws.ws_col;
            comp_set_size(m->comp, m->trows, m->tcols);
            mux_relayout(m); /* chat too, clear of the bar */
        }
        (void)hold_term_take_interrupt(); /* Ctrl-C arrives as a key for the focused pane */
        drop_ended(m);
        if (mux_pane_total(m) == 0) break; /* every pane has ended */
        /* One pane left and nothing floating: back to plain attach, the
         * screen exactly as it is without panes - unless the compositor is
         * here for good (backmux shell). */
        if (!m->resident && mux_pane_total(m) == 1 && m->fl == FL_NONE && !m->lead && m->mode == MODE_NONE && !m->help && !m->swapping &&
            m->prompt == PROMPT_NONE && m->now >= m->notice_until && !m->full &&
            !(m->chat && chat_on_screen(m->chat, m->now))) {
            /* the one pane left: windows keep their numbers, so it need not
             * be in the first */
            struct pane *p = NULL;
            for (int i = 0; i < MUX_MAX_PANES && !p; i++) p = m->slot[i];
            /* the last frame, with nothing over the program: inline nothing
             * repaints after this, so nothing of chat may be left on it */
            m->dirty = true;
            mux_render(m);
            snprintf(handback, hn, "%s", p->sock);
            hold_console_set_attached_name(p->name); /* a detach names it */
            /* The mouse reporting this compositor asked for goes off before
             * plain attach takes over: until the program's own repaint
             * arrives, every movement would be typed into it. */
            (void)hold_write_all(STDOUT_FILENO, "\033[?1000l\033[?1002l\033[?1003l\033[?1006l", 32);
            (void)mux_frame(p->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            *handed_back = true;
            break;
        }
        /* backmux swap, from anywhere: a request file names two panes */
        char req_path[HOLD_PATH_MAX], req[256];
        if (hold_checked_snprintf(req_path, sizeof(req_path) - 8, "%s/chat/mux.%d.swap", store->base, (int)getpid()) == 0 &&
            mux_read_small(req_path, req, sizeof(req))) {
            unlink(req_path);
            char a[96] = "", b[96] = "", why2[160] = "bad request";
            int ok = -1;
            /* by name or id only: whoever asks cannot see this screen */
            if (sscanf(req, "%95s %95s", a, b) == 2) {
                int sa = mux_slot_of_name(m, a), sb = mux_slot_of_name(m, b);
                if (sa < 0 || sb < 0) snprintf(why2, sizeof(why2), "%s is not a pane in that terminal", sa < 0 ? a : b);
                else {
                    mux_do_swap(m, sa, sb);
                    ok = 0;
                }
            }
            if (ok != 0) mux_notice(m, "swap: %s", why2);
            /* the answer, for the backmux swap waiting on it */
            char res_path[HOLD_PATH_MAX], res[200];
            int rl = ok == 0 ? snprintf(res, sizeof(res), "ok\n") : snprintf(res, sizeof(res), "error %s\n", why2);
            if (hold_checked_snprintf(res_path, sizeof(res_path) - 8, "%s/chat/mux.%d.result", store->base, (int)getpid()) == 0)
                mux_write_small(res_path, res, (size_t)rl);
        }
        mux_chat_look(m);
        if (mux_decorated(m) && (mux_bar_on(m) || mux_info_on(m))) mux_refresh_stats(m);
        mux_render(m);

        struct pollfd pfds[1 + MUX_MAX_PANES + CHAT_MAX_OUTBOUND];
        int idx[MUX_MAX_PANES];
        nfds_t nf = 0;
        pfds[nf++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        for (int i = 0; i < MUX_MAX_PANES; i++) {
            idx[i] = -1;
            if (m->slot[i]) {
                idx[i] = (int)nf;
                pfds[nf++] = (struct pollfd){.fd = m->slot[i]->rfd, .events = POLLIN};
            }
        }
        /* chat's sends in flight, and its timers (a lone Esc, a hint) */
        int sidx = (int)nf, snf = m->chat ? chat_poll_fds(m->chat, pfds + nf, CHAT_MAX_OUTBOUND) : 0;
        nf += (nfds_t)snf;
        int timeout = m->dirty ? MUX_FRAME_MS : MUX_LOOK_MS;
        if (m->chat) {
            int64_t due = chat_due_in(m->chat, m->now);
            if (due >= 0 && due < timeout) timeout = (int)due;
        }
        int pr = poll(pfds, nf, timeout);
        if (pr < 0 && errno != EINTR) {
            rc = 1;
            break;
        }
        m->now = mux_mono_ms();
        if (m->chat) {
            if (pr > 0 && snf > 0) chat_poll_done(m->chat, pfds + sidx, snf, m->now);
            chat_tick(m->chat, m->now);
            if (chat_wants_frame(m->chat)) m->dirty = true;
        }
        bool probe = m->now >= m->fifo_probe_at;
        if (probe) m->fifo_probe_at = m->now + MUX_FIFO_PROBE_MS;
        if (pr <= 0) m->dirty = true; /* timers: names that expire, notices */
        for (int i = 0; i < MUX_MAX_PANES; i++) {
            if (idx[i] < 0) continue;
            struct pane *p = m->slot[i];
            /* a FIFO pane (separate read and write ends) is read on the probe
             * too: the rfd is non-blocking, so nothing there is EAGAIN */
            bool fifo_probe = probe && p->rfd != p->wfd;
            if (!(pfds[idx[i]].revents & (POLLIN | POLLHUP | POLLERR)) && !fifo_probe) continue;
            char buf[16384];
            ssize_t k = read(p->rfd, buf, sizeof(buf));
            if (k > 0) {
                hold_screen_feed(p->scr, buf, (size_t)k);
                hold_screen_discard_output(p->scr); /* its broker answers queries */
                (void)hold_screen_take_bells(p->scr);
                if (i / 4 == m->view) m->dirty = true;
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                p->ended = true;
            }
        }
        if (pr <= 0) continue;
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buf[4096];
            ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
            if (k > 0) {
                if (mux_handle_input(m, buf, (size_t)k) == 1) goto detach;
            } else if (k == 0) {
                goto detach;
            }
        }
    }
    goto out;
detach:
    /* Detach: every pane lets go; every process keeps running. Which ones
     * is said once the terminal is the person's again. */
    hold_console_note_detached(NULL);
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) {
            (void)mux_frame(m->slot[i]->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            hold_console_note_detached(m->slot[i]->name);
        }
    rc = 0;
out:
    hold_term_signals_restore(&sig);
    for (int i = 0; i < MUX_MAX_PANES; i++) mux_pane_free(m->slot[i]);
    chat_free(m->chat);
    comp_free(m->comp);
    char mux_path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(mux_path, sizeof(mux_path), "%s/chat/mux.%d", store->base, (int)getpid()) == 0)
        unlink(mux_path);
    free(m);
    if (rc > 128 && !*handed_back) {
        hold_console_term_release();
        hold_term_reraise_stop();
    }
    return rc;
}
