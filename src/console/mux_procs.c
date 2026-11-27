/* console/mux_procs.c - the process launcher (running terminals, consoles
 * that scroll, ended processes) and the bar's counters and load, from one
 * walk of the records. */
#include "mux.h"

/* Whether a record's process runs now. A console's broker and program must
 * both be there (hold_console_record_alive); a process without a console is
 * taken at its pid's word. Ended: it says so, or it is gone. */
static bool record_running(const struct hold_run_record *r) {
    if (r->has_exit_code || r->has_ended_at || (r->has_state && strcmp(r->state, "running") != 0)) return false;
    if (r->has_console) return hold_path_exists(r->console_sock) && hold_console_record_alive(r);
    return r->pid > 1 && (kill(r->pid, 0) == 0 || errno == EPERM);
}

/* Which list of the launcher a record belongs on, -1 none. Terminals can be
 * panes (a pane shows a screen); a console that scrolls - a program on pipes,
 * or a server's log - is opened in a viewer pane; an ended process can be
 * started again. */
static int record_cat(const struct hold_run_record *r) {
    if (!record_running(r)) return PK_ENDED;
    if (r->has_console && r->recipe.mode_tty) return PK_TTY;
    return PK_SCROLL;
}

struct pick_walk {
    struct mux *m;
    bool fill; /* fill m->picks for m->pick_cat, besides counting */
};

static int pick_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct pick_walk *wk = ctx;
    struct mux *m = wk->m;
    if (!r) return 0;
    int cat = record_cat(r);
    /* the process this terminal itself runs in: shown inside itself, it would
     * draw itself forever */
    const char *here = getenv("BACKMUX_ID");
    if (here && !strcmp(here, id)) return 0;
    if (cat == PK_TTY) m->n_tty++;
    else if (cat == PK_SCROLL) m->n_scroll++;
    else m->n_ended++;
    if (!wk->fill || cat != (int)m->pick_cat || m->npicks >= MUX_PICK_MAX) return 0;
    if (cat == PK_TTY && mux_has_pane(m, id)) return 0; /* already on this terminal */
    struct pick *p = &m->picks[m->npicks++];
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    if (r->has_name && r->name[0]) snprintf(p->name, sizeof(p->name), "%s", r->name);
    else snprintf(p->name, sizeof(p->name), "%.12s", id);
    if (r->has_console) snprintf(p->sock, sizeof(p->sock), "%s", r->console_sock);
    if (r->recipe.argc > 0 && r->recipe.argv) snprintf(p->program, sizeof(p->program), "%s", r->recipe.argv[0]);
    if (r->recipe.argc > 1) (void)hold_format_argv_human(p->args, sizeof(p->args), r->recipe.argc - 1, r->recipe.argv + 1);
    snprintf(p->working, sizeof(p->working), "%s",
             r->recipe.has_working ? r->recipe.working : (r->has_observed ? r->observed_cwd : ""));
    if (r->has_started_at) snprintf(p->started, sizeof(p->started), "%s", r->started_at);
    (void)hold_agent_of_record(r, p->agent, sizeof(p->agent));
    p->tty = r->recipe.mode_tty;
    p->pipe = r->has_console && !r->recipe.mode_tty;
    p->shell = r->recipe.mode_shell;
    p->exit_code = r->has_exit_code ? r->exit_code : -1;
    return 0;
}

static int pick_order(const void *a, const void *b) {
    return strcasecmp(((const struct pick *)a)->name, ((const struct pick *)b)->name);
}

/* One walk of the records: the bar's counts, and (fill) the launcher's list. */
static void scan_records(struct mux *m, bool fill) {
    m->n_tty = m->n_scroll = m->n_ended = 0;
    if (fill) m->npicks = 0;
    struct pick_walk w = {m, fill};
    (void)hold_for_each_record(m->store->record_dir, pick_cb, &w);
    if (fill) {
        qsort(m->picks, (size_t)m->npicks, sizeof(m->picks[0]), pick_order);
        if (m->pick_sel >= m->npicks) m->pick_sel = m->npicks > 0 ? m->npicks - 1 : 0;
    }
}

void mux_load_picks(struct mux *m) {
    m->pick_sel = 0;
    scan_records(m, true);
}

/* Once a second while the bar or the name bars are up: the counts, the
 * machine's load, every pane's. */
void mux_refresh_stats(struct mux *m) {
    if (m->now < m->next_stats) return;
    m->next_stats = m->now + MUX_STATS_MS;
    scan_records(m, m->fl == FL_PROCS);
    uint64_t busy = 0, total = 0;
    if (hold_system_cpu_times(&busy, &total) == 0) {
        m->sys_cpu = m->sys_total && total > m->sys_total ? (int)((busy - m->sys_busy) * 100 / (total - m->sys_total)) : -1;
        m->sys_busy = busy;
        m->sys_total = total;
    } else {
        m->sys_cpu = -1;
    }
    uint64_t mt = 0, ma = 0;
    if (hold_system_memory(&mt, &ma) == 0) {
        m->mem_total = mt;
        m->mem_used = mt > ma ? mt - ma : 0;
    }
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->base[i]) {
            mux_pane_identify(m, m->base[i]);
            mux_pane_load(m->base[i], m->now);
        }
    m->dirty = true;
}

static const char *const cat_name[PK_CATS] = {"Terminals", "Consoles", "Ended"};

/* The launcher: three lists - running terminals (attach one as a pane),
 * consoles that scroll (open one in a viewer pane), ended processes (start
 * one again) - and the chosen entry's details. */
void mux_draw_procs(struct mux *m) {
    int w = m->tcols - 6 < 84 ? m->tcols - 6 : 84, h = m->arows - 2 < 22 ? m->arows - 2 : 22;
    if (w < 30 || h < 10) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2;
    static const char *const verb[PK_CATS] = {"attach as a pane", "open in a pane", "start again"};
    char hint[96];
    snprintf(hint, sizeof(hint), "Tab: list  Up/Down: select  Enter: %s  Esc: close",
             verb[m->pick_cat]);
    mux_draw_frame(m, y, x, h, w, "Processes", hint);
    /* the three lists, as tabs: their counts in the bar's colors */
    const int counts[PK_CATS] = {m->n_tty, m->n_scroll, m->n_ended};
    const struct comp_style *cst[PK_CATS] = {&ST_CNT_TTY, &ST_CNT_SCROLL, &ST_CNT_ENDED};
    int tx = x + 2;
    m->pick_tabs_row = y + 1;
    for (int k = 0; k < PK_CATS; k++) {
        char tab[48];
        snprintf(tab, sizeof(tab), " %s %d ", cat_name[k], counts[k]);
        m->pick_tab_x[k] = tx;
        struct comp_style on = *cst[k];
        on.bg = 238;
        tx += mux_put(m, y + 1, tx, x + w - 2 - tx, tab, k == (int)m->pick_cat ? &on : &ST_BOX_DIM);
        tx += mux_put(m, y + 1, tx, x + w - 2 - tx, "  ", &ST_BOX);
    }
    m->pick_tab_x[PK_CATS] = tx;
    for (int c = x + 1; c < x + w - 1; c++) comp_put(m->comp, y + 2, c, 0x2500, &ST_FRAME);
    int list_h = (h - 4) / 2;
    if (list_h < 3) list_h = 3;
    if (m->npicks == 0) {
        static const char *const none[PK_CATS] = {"No other terminal processes are running.",
                                                  "No consoles or servers are running.", "No ended processes."};
        mux_put(m, y + 3, x + 3, w - 6, none[m->pick_cat], &ST_BOX_DIM);
    }
    int first = m->pick_sel >= list_h ? m->pick_sel - list_h + 1 : 0;
    m->pick_x = x, m->pick_w = w, m->pick_top = y + 3, m->pick_first = first, m->pick_rows = list_h;
    for (int i = first; i < m->npicks && i - first < list_h; i++) {
        const struct pick *p = &m->picks[i];
        bool sel = i == m->pick_sel;
        const struct comp_style *st = sel ? &ST_SEL : &ST_BOX;
        int row = y + 3 + i - first;
        comp_fill(m->comp, row, x + 1, w - 2, st);
        int u = mux_put(m, row, x + 2, w - 4, sel ? "\xe2\x96\xb8 " : "  ", st);
        u += mux_put(m, row, x + 2 + u, 26, p->name, st);
        int col2 = x + 2 + 2 + 26 + 1;
        const char *what = p->agent[0] ? p->agent : (strrchr(p->program, '/') ? strrchr(p->program, '/') + 1 : p->program);
        int v = mux_put(m, row, col2, x + w - 2 - col2, what, sel ? st : (p->agent[0] ? &ST_KEY : &ST_BOX_DIM));
        char tail[48] = "";
        if (m->pick_cat == PK_ENDED) {
            if (p->exit_code >= 0) snprintf(tail, sizeof(tail), "exit %d", p->exit_code);
            else snprintf(tail, sizeof(tail), "ended");
        } else if (m->pick_cat == PK_SCROLL) {
            snprintf(tail, sizeof(tail), "%s", p->pipe ? "console" : "log");
        }
        if (tail[0]) {
            int tl = mux_text_cols(tail);
            if (col2 + v + 2 < x + w - 3 - tl) mux_put(m, row, x + w - 3 - tl, tl, tail, sel ? st : &ST_BOX_DIM);
        }
    }
    int iy = y + 3 + list_h;
    for (int c = x + 1; c < x + w - 1; c++) comp_put(m->comp, iy, c, 0x2500, &ST_FRAME);
    if (m->npicks > 0 && iy + 4 < y + h - 1) {
        const struct pick *p = &m->picks[m->pick_sel];
        static const char *const label[] = {"Program", "Arguments", "Working directory", "Started"};
        const char *val[] = {p->program, p->args[0] ? p->args : "-", p->working[0] ? p->working : "-",
                             p->started[0] ? p->started : "-"};
        for (int k = 0; k < 4 && iy + 1 + k < y + h - 1; k++) {
            mux_put(m, iy + 1 + k, x + 3, 19, label[k], &ST_HEAD);
            mux_put(m, iy + 1 + k, x + 3 + 19, w - 6 - 19, val[k], &ST_BOX);
        }
    }
}

/* A process as a pane, by its record's console - not placed yet. NULL
 * (and a notice saying why) when it has no record or no console. */
struct pane *mux_open_id(struct mux *m, const char *id, const char *fallback_name) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(m->store->record_dir, id, &r, path, sizeof(path)) != 0) {
        mux_notice(m, "%s has no record", fallback_name);
        return NULL;
    }
    char name[ALIAS_MAX_LEN + 1], sock[HOLD_PATH_MAX];
    snprintf(name, sizeof(name), "%s", r.has_name && r.name[0] ? r.name : fallback_name);
    snprintf(sock, sizeof(sock), "%s", r.has_console ? r.console_sock : "");
    hold_free_run_record(&r);
    if (!sock[0]) {
        mux_notice(m, "%s has no console", name);
        return NULL;
    }
    const char *why = NULL;
    struct pane *np = mux_pane_open(id, name, sock, m->arows, m->tcols, &why);
    if (!np) mux_notice(m, "cannot attach %s: %s", name, why ? why : "console unavailable");
    return np;
}

/* An ended process started again in the background, as it first ran (on a
 * PTY, on pipes, or plain). A shell comes back only attached (backmux NAME):
 * false, saying so. */
bool mux_start_again(struct mux *m, const char *id, const char *name, bool tty, bool pipe, bool shell) {
    char err[200], out[ID_STR_LEN + 8];
    if (shell) {
        mux_notice(m, "a shell comes back attached: backmux %s", name);
        return false;
    }
    char *args[] = {tty ? "-dt" : pipe ? "-di" : "-d", (char *)id, NULL};
    if (mux_run_self(args, err, sizeof(err), out, sizeof(out)) != 0) {
        mux_notice(m, "cannot start %s: %s", name, err[0] ? err : "backmux failed");
        return false;
    }
    return true;
}

/* A process as a pane, placed: beside the active pane (split), or where the
 * view puts the next one. */
static bool open_pane_id(struct mux *m, const char *id, const char *fallback_name, char split) {
    struct pane *np = mux_open_id(m, id, fallback_name);
    if (!np) return false;
    if (split) {
        /* the split was checked before the terminal started; the pane is
         * placed beside the active one, and made active */
        if (!mux_base_split(m, np, split)) {
            mux_pane_free(np);
            mux_notice(m, "that split no longer fits");
            return false;
        }
        mux_relayout(m);
        mux_set_focus(m, np);
        mux_relayout(m);
        mux_pane_identify(m, np);
        return true;
    }
    if (mux_add_pane(m, np) != 0) {
        mux_pane_free(np);
        mux_notice(m, "every window is full");
        return false;
    }
    mux_pane_identify(m, np);
    return true;
}

/* What Enter does in the launcher. A terminal is attached as a pane. A
 * console that scrolls is opened in a pane of its own: a terminal process
 * that runs backmux attach (a program on pipes: its input too) or backmux
 * logs -f (a server), with --rm so it leaves no record once closed. An ended
 * process is started again in the background, and shown as a pane when it
 * has a terminal. */
void mux_launch_pick(struct mux *m, const struct pick *p) {
    if (mux_pane_total(m) >= MUX_MAX_PANES && (m->pick_cat != PK_ENDED || p->tty)) {
        mux_notice(m, "pane limit reached (%d)", MUX_MAX_PANES);
        return;
    }
    char err[200], id[ID_STR_LEN + 8];
    if (m->pick_cat == PK_TTY) {
        const char *why = NULL;
        struct pane *np = mux_pane_open(p->id, p->name, p->sock, m->arows, m->tcols, &why);
        if (!np) mux_notice(m, "cannot attach %s: %s", p->name, why ? why : "console unavailable");
        else if (mux_add_pane(m, np) != 0) mux_pane_free(np);
        else mux_pane_identify(m, np);
        return;
    }
    if (m->pick_cat == PK_SCROLL) {
        char self[HOLD_PATH_MAX];
        if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) {
            mux_notice(m, "cannot find backmux itself");
            return;
        }
        /* named for what it shows (srv-log, pipey-view), so its name bar and
         * the list say so; a name already taken gets a generated one */
        char vname[ALIAS_MAX_LEN + 1];
        snprintf(vname, sizeof(vname), "%.*s-%s", ALIAS_MAX_LEN - 5, p->name, p->pipe ? "view" : "log");
        char *args[] = {"-dt", "--rm", "--name", vname, "--", self, p->pipe ? "attach" : "logs", (char *)p->id,
                        p->pipe ? NULL : "-f", NULL};
        char *anon[] = {"-dt", "--rm", "--", self, p->pipe ? "attach" : "logs", (char *)p->id, p->pipe ? NULL : "-f", NULL};
        if ((mux_run_self(args, err, sizeof(err), id, sizeof(id)) != 0 || !id[0]) &&
            (mux_run_self(anon, err, sizeof(err), id, sizeof(id)) != 0 || !id[0])) {
            mux_notice(m, "cannot open %s: %s", p->name, err[0] ? err : "the viewer did not start");
            return;
        }
        (void)open_pane_id(m, id, p->name, 0);
        return;
    }
    if (!mux_start_again(m, p->id, p->name, p->tty, p->pipe, p->shell)) return;
    if (p->tty) (void)open_pane_id(m, p->id, p->name, 0);
    else mux_notice(m, "started %s", p->name);
}

/* A new terminal (prefix Enter, or a split: prefix | to the right, prefix -
 * below): the person's shell, started in the background in the active
 * pane's working directory, then attached where the view puts the next pane
 * - or, split, beside the active one. Why the active pane's directory: the
 * pane you are in is almost always where the next one is wanted (Zellij
 * does the same; tmux uses the session's). */
void mux_new_terminal(struct mux *m, char split) {
    if (mux_pane_total(m) >= MUX_MAX_PANES) {
        mux_notice(m, "pane limit reached (%d)", MUX_MAX_PANES);
        return;
    }
    if (split && !mux_can_split(m, split)) {
        mux_notice(m, "window full"); /* short: it shows in the name bar's blank space */
        return;
    }
    const char *shell = getenv("SHELL");
    if (!shell || shell[0] != '/' || access(shell, X_OK) != 0) shell = "/bin/sh";
    char cwd[HOLD_PATH_MAX] = "";
    if (m->active && m->active->pid > 0) {
        pid_t fg = 0, at = m->active->pid;
        if (hold_proc_foreground_group(at, &fg) == 0 && fg > 0) at = fg;
        if (hold_proc_cwd(at, cwd, sizeof(cwd)) != 0 && hold_proc_cwd(m->active->pid, cwd, sizeof(cwd)) != 0) cwd[0] = '\0';
    }
    char err[200], id[ID_STR_LEN + 8];
    char *args[] = {"-dt", "--", (char *)shell, NULL};
    if (mux_run_self_in(cwd[0] ? cwd : NULL, args, err, sizeof(err), id, sizeof(id)) != 0 || !id[0]) {
        mux_notice(m, "cannot start a terminal: %s", err[0] ? err : "backmux failed");
        return;
    }
    (void)open_pane_id(m, id, "terminal", split);
}
