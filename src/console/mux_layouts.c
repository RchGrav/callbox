/* console/mux_layouts.c - layouts: the base layout kept in a file by name,
 * and the compositor's requests from the command line.
 *
 * A layout is ~/.config/backmux/layouts/NAME, plain text a person can read
 * and edit: where the view is, each window's split lines, and each pane's
 * place with its process id (the name beside it is only for reading - a
 * rename does not break a layout). Why only the base: views are computed
 * from it, so saving with one on keeps what is on screen by making it the
 * base first. Why ids and not commands: every process's record already
 * says how it was started (its program, mode, user, store), so a layout only
 * says where things go.
 *
 * Loading never ends anything: panes not in the layout are let go (their
 * processes run on), panes already here just move, running members are
 * attached (taken from another terminal, as attach does), and ended ones
 * are started again from their records first. */
#include "mux.h"

static int layouts_dir(char *out, size_t n) {
    const char *home = hold_console_home() ? hold_console_home() : getenv("HOME");
    char cfg[HOLD_PATH_MAX];
    if (!home || hold_settings_path(home, cfg, sizeof(cfg)) != 0) return -1;
    char *slash = strrchr(cfg, '/');
    if (!slash) return -1;
    *slash = '\0';
    return hold_checked_snprintf(out, n, "%s/layouts", cfg);
}

static int layout_path(const char *name, char *out, size_t n) {
    char dir[HOLD_PATH_MAX];
    if (!hold_valid_alias(name) || layouts_dir(dir, sizeof(dir)) != 0) return -1;
    return hold_checked_snprintf(out, n, "%s/%s", dir, name);
}

/* Save the base as NAME (a view on is made the base first). */
bool mux_layout_save(struct mux *m, const char *name, const char **why) {
    char path[HOLD_PATH_MAX], dir[HOLD_PATH_MAX];
    if (!hold_valid_alias(name)) {
        *why = "a layout's name is letters, digits, - and _";
        return false;
    }
    if (layout_path(name, path, sizeof(path)) != 0 || layouts_dir(dir, sizeof(dir)) != 0 ||
        hold_mkdir_p_mode(dir, 0700) != 0) {
        *why = "cannot reach ~/.config/backmux/layouts";
        return false;
    }
    if (m->sort || m->arr != ARR_BASE) { /* what is on screen becomes the base */
        memcpy(m->base, m->shown, sizeof(m->base));
        m->sort = false;
        m->arr = ARR_BASE;
    }
    char buf[8192];
    size_t k = (size_t)snprintf(buf, sizeof(buf),
                                "# backmux layout: places in the base, by process id (the name is for reading)\n"
                                "view %d %d\n", m->vx, m->vy);
    for (int w = 0; w < MUX_MAX_WINDOWS && k < sizeof(buf); w++)
        if (m->colpct[w] || m->lpct[w] || m->rpct[w])
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "lines %d %d %d %d\n", w + 1, m->colpct[w], m->lpct[w], m->rpct[w]);
    for (int i = 0; i < MUX_SLOTS && k < sizeof(buf); i++)
        if (m->base[i] && m->base[i]->id[0])
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "pane %d%c %s %s\n", i / MUX_POS + 1, "ABCDE"[i % MUX_POS],
                                  m->base[i]->id, m->base[i]->name);
    if (k >= sizeof(buf)) {
        *why = "the layout is too large";
        return false;
    }
    mux_write_small(path, buf, k);
    snprintf(m->layout_name, sizeof(m->layout_name), "%s", name);
    m->layout_dirty = false;
    mux_relayout(m);
    return true;
}

/* Load NAME into the base. keep (may be NULL) is not let go: backmux shell's
 * own shell, which joins its default layout. */
bool mux_layout_load(struct mux *m, const char *name, struct pane *keep, const char **why) {
    static char msg[160];
    char path[HOLD_PATH_MAX];
    char *text = NULL;
    size_t tlen = 0;
    if (layout_path(name, path, sizeof(path)) != 0 || hold_read_owned_file_capped(path, 65536, &text, &tlen) != 0) {
        snprintf(msg, sizeof(msg), "no layout %s", name);
        *why = msg;
        return false;
    }
    struct pane *placed[MUX_SLOTS] = {0};
    int vx = 0, vy = 0, lines[MUX_MAX_WINDOWS][3] = {{0}}, got = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char place[8], id[ID_STR_LEN + 2], pname[ALIAS_MAX_LEN + 2];
        int w, a, b, c;
        if (sscanf(line, "view %d %d", &vx, &vy) == 2) continue;
        if (sscanf(line, "lines %d %d %d %d", &w, &a, &b, &c) == 4 && w >= 1 && w <= MUX_MAX_WINDOWS) {
            lines[w - 1][0] = a, lines[w - 1][1] = b, lines[w - 1][2] = c;
            continue;
        }
        if (sscanf(line, "pane %7s %65s %64s", place, id, pname) < 2) continue;
        int slot = mux_parse_position(place);
        if (slot < 0 || placed[slot] || !hold_valid_id(id)) continue;
        struct pane *p = NULL;
        for (int i = 0; i < MUX_SLOTS && !p; i++)
            if (m->base[i] && !strcmp(m->base[i]->id, id)) p = m->base[i];
        if (!p) { /* not here: attach it, starting it again if it ended */
            struct hold_run_record r;
            char rpath[HOLD_PATH_MAX];
            if (hold_load_record_by_id(m->store->record_dir, id, &r, rpath, sizeof(rpath)) != 0) continue;
            bool ended = r.has_exit_code || r.has_ended_at || (r.has_state && strcmp(r.state, "running") != 0) ||
                         !r.has_console || !hold_console_record_alive(&r);
            bool tty = r.recipe.mode_tty, pipe = r.has_console && !tty, shell = r.recipe.mode_shell;
            char rname[ALIAS_MAX_LEN + 1];
            snprintf(rname, sizeof(rname), "%.*s", ALIAS_MAX_LEN, r.has_name ? r.name : pname);
            hold_free_run_record(&r);
            if (!tty) continue; /* a pane is a terminal */
            if (ended && !mux_start_again(m, id, rname, tty, pipe, shell)) continue;
            p = mux_open_id(m, id, rname);
            if (!p) continue;
            mux_pane_identify(m, p);
        }
        placed[slot] = p;
        got++;
    }
    free(text);
    if (!got) {
        snprintf(msg, sizeof(msg), "nothing in layout %s could be shown", name);
        *why = msg;
        return false;
    }
    /* what is here and not in the layout lets go (its process runs on) */
    for (int i = 0; i < MUX_SLOTS; i++) {
        struct pane *p = m->base[i];
        bool in = false;
        for (int k = 0; k < MUX_SLOTS && p && !in; k++) in = placed[k] == p;
        if (!p || in || p == keep) continue;
        (void)mux_frame(p->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
        if (m->active == p) m->active = NULL;
        if (m->full == p) m->full = NULL;
        if (m->app == p) m->app = NULL;
        if (m->prompt_pane == p) m->prompt = PROMPT_NONE, m->prompt_pane = NULL;
        mux_pane_free(p);
    }
    bool keep_placed = false;
    for (int k = 0; k < MUX_SLOTS && keep; k++) keep_placed = keep_placed || placed[k] == keep;
    memcpy(m->base, placed, sizeof(m->base));
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) { /* a hand-edited file may name no shape */
        if (!mux_shape_valid(mux_window_mask(m->base, w))) {
            struct pane *out[MUX_POS];
            if (mux_reshape(m->base + w * MUX_POS, NULL, NULL, -1, out)) memcpy(m->base + w * MUX_POS, out, sizeof(out));
        }
        m->colpct[w] = lines[w][0], m->lpct[w] = lines[w][1], m->rpct[w] = lines[w][2];
    }
    m->sort = false;
    m->arr = ARR_BASE;
    m->vx = vx >= 0 && vx <= 4 ? vx : 0;
    m->vy = vy >= 0 && vy <= 4 ? vy : 0;
    if (keep && !keep_placed) (void)mux_base_add(m, keep); /* the shell joins the layout */
    snprintf(m->layout_name, sizeof(m->layout_name), "%s", name);
    m->layout_dirty = false;
    mux_settle(m);
    return true;
}

/* ---- the panel (prefix L, or the layout's name on the bar) ---------------- */

static int by_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void load_names(struct mux *m) {
    char dir[HOLD_PATH_MAX];
    m->nlay = 0;
    const char *home = hold_console_home() ? hold_console_home() : getenv("HOME");
    if (!home || hold_settings_get(home, "layout", m->lay_default, sizeof(m->lay_default)) != 0) m->lay_default[0] = '\0';
    if (layouts_dir(dir, sizeof(dir)) != 0) return;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && m->nlay < MUX_LAYOUT_MAX)
        if (hold_valid_alias(e->d_name)) snprintf(m->lay_names[m->nlay++], sizeof(m->lay_names[0]), "%.*s", ALIAS_MAX_LEN, e->d_name);
    if (d) closedir(d);
    qsort(m->lay_names, (size_t)m->nlay, sizeof(m->lay_names[0]), by_name);
    if (m->lay_sel >= m->nlay) m->lay_sel = m->nlay ? m->nlay - 1 : 0;
}

void mux_layouts_open(struct mux *m) {
    m->lay_sel = 0;
    m->lay_naming = false;
    load_names(m);
    for (int i = 0; i < m->nlay; i++)
        if (!strcmp(m->lay_names[i], m->layout_name)) m->lay_sel = i;
}

void mux_draw_layouts(struct mux *m) {
    int w = m->tcols - 6 < 60 ? m->tcols - 6 : 60, h = m->nlay + 7 < m->arows - 2 ? m->nlay + 7 : m->arows - 2;
    if (h < 8) h = 8 < m->arows ? 8 : m->arows;
    if (w < 30 || h < 6) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2;
    mux_draw_frame(m, y, x, h, w, "Layouts", m->lay_naming ? "Enter: save  Esc: cancel"
                                                           : "Enter load  s save  n new  d default  Esc");
    char line[160];
    snprintf(line, sizeof(line), "Now: %s%s", m->layout_name[0] ? m->layout_name : "(not saved)", m->layout_dirty ? " - changed since" : "");
    mux_put(m, y + 1, x + 3, w - 6, line, &ST_HEAD);
    if (m->lay_naming) {
        snprintf(line, sizeof(line), "Save as: %s", m->in);
        int used = mux_put(m, y + 3, x + 3, w - 6, line, &ST_NB_EDIT);
        (void)used;
        return;
    }
    if (m->nlay == 0) mux_put(m, y + 3, x + 3, w - 6, "No layouts yet: n saves this one.", &ST_BOX_DIM);
    for (int i = 0; i < m->nlay && 3 + i < h - 1; i++) {
        bool sel = i == m->lay_sel;
        const struct comp_style *st = sel ? &ST_SEL : &ST_BOX;
        comp_fill(m->comp, y + 3 + i, x + 1, w - 2, st);
        snprintf(line, sizeof(line), "%s %-24s %s%s", sel ? "\xe2\x96\xb8" : " ", m->lay_names[i],
                 !strcmp(m->lay_names[i], m->layout_name) ? "loaded " : "", !strcmp(m->lay_names[i], m->lay_default) ? "default" : "");
        mux_put(m, y + 3 + i, x + 2, w - 4, line, st);
    }
}

void mux_layouts_key(struct mux *m, const unsigned char *k, size_t n) {
    const char *why = NULL;
    m->dirty = true;
    if (m->lay_naming) { /* typing a name for "save as" */
        if (n == 1 && k[0] == 0x1b) m->lay_naming = false;
        else if (n == 1 && (k[0] == '\r' || k[0] == '\n')) {
            m->lay_naming = false;
            if (mux_layout_save(m, m->in, &why)) mux_notice(m, "saved layout %s", m->in), m->fl = FL_NONE;
            else mux_notice(m, "not saved: %s", why);
        } else if (n == 1 && (k[0] == 0x7f || k[0] == 0x08)) {
            if (m->in_len) m->in[--m->in_len] = '\0';
        } else if (n == 1 && (isalnum(k[0]) || k[0] == '-' || k[0] == '_') && m->in_len < ALIAS_MAX_LEN) {
            m->in[m->in_len++] = (char)k[0];
            m->in[m->in_len] = '\0';
        }
        return;
    }
    bool up = n == 3 && k[0] == 0x1b && k[2] == 'A', down = n == 3 && k[0] == 0x1b && k[2] == 'B';
    const char *sel = m->nlay ? m->lay_names[m->lay_sel] : NULL;
    if (n == 1 && k[0] == 0x1b) m->fl = FL_NONE;
    else if (up && m->lay_sel > 0) m->lay_sel--;
    else if (down && m->lay_sel + 1 < m->nlay) m->lay_sel++;
    else if (n == 1 && (k[0] == '\r' || k[0] == '\n') && sel) {
        char name[ALIAS_MAX_LEN + 1];
        snprintf(name, sizeof(name), "%s", sel);
        m->fl = FL_NONE;
        if (mux_layout_load(m, name, NULL, &why)) mux_notice(m, "layout %s", name);
        else mux_notice(m, "%s", why);
    } else if (n == 1 && k[0] == 's') { /* save over the loaded one (or the one chosen) */
        char name[ALIAS_MAX_LEN + 1];
        snprintf(name, sizeof(name), "%s", m->layout_name[0] ? m->layout_name : sel ? sel : "");
        if (!name[0]) {
            m->lay_naming = true, m->in_len = 0, m->in[0] = '\0';
            return;
        }
        if (mux_layout_save(m, name, &why)) mux_notice(m, "saved layout %s", name), m->fl = FL_NONE;
        else mux_notice(m, "not saved: %s", why);
    } else if (n == 1 && k[0] == 'n') {
        m->lay_naming = true;
        m->in_len = 0;
        m->in[0] = '\0';
    } else if (n == 1 && k[0] == 'd' && sel) { /* the one backmux shell opens with */
        const char *home = hold_console_home() ? hold_console_home() : getenv("HOME");
        if (home && hold_settings_put(home, "layout", sel) == 0) mux_notice(m, "%s opens with backmux shell", sel);
        load_names(m);
    }
}

/* ---- requests from the command line (backmux swap, backmux layout) ------- */

/* chat/mux.<pid>.ask holds one request - "swap A B", "layout save NAME",
 * "layout load NAME" - and the answer goes in chat/mux.<pid>.result. */
void mux_serve_requests(struct mux *m) {
    char req_path[HOLD_PATH_MAX], req[256];
    if (hold_checked_snprintf(req_path, sizeof(req_path) - 8, "%s/chat/mux.%d.ask", m->store->base, (int)getpid()) != 0 ||
        !mux_read_small(req_path, req, sizeof(req)))
        return;
    unlink(req_path);
    char verb[16] = "", a[96] = "", b[96] = "", why2[200] = "bad request";
    const char *why = NULL;
    int ok = -1;
    if (sscanf(req, "%15s %95s %95s", verb, a, b) >= 2) {
        if (!strcmp(verb, "swap") && b[0]) {
            /* a place, a name or an id (mux_find.c); the second may be free */
            int sa = -1, sb = -1;
            const char *wa = NULL, *wb = NULL, *wsend = NULL;
            struct pane *pa = mux_find(m, a, &sa, &wa);
            (void)mux_find(m, b, &sb, &wb);
            if (!pa) snprintf(why2, sizeof(why2), "%s", wa ? wa : "no pane there");
            else if (sb < 0) snprintf(why2, sizeof(why2), "%s", wb ? wb : "no such place");
            else if ((wsend = mux_base_send(m, pa, sb)) != NULL) snprintf(why2, sizeof(why2), "%s", wsend);
            else {
                mux_relayout(m);
                mux_focus_in_view(m);
                ok = 0;
            }
        } else if (!strcmp(verb, "layout") && b[0] && (!strcmp(a, "save") || !strcmp(a, "load"))) {
            bool done = !strcmp(a, "save") ? mux_layout_save(m, b, &why) : mux_layout_load(m, b, NULL, &why);
            if (done) ok = 0;
            else snprintf(why2, sizeof(why2), "%s", why ? why : "failed");
        }
    }
    if (ok != 0) mux_notice(m, "%s: %s", verb[0] ? verb : "request", why2);
    char res_path[HOLD_PATH_MAX], res[220];
    int rl = ok == 0 ? snprintf(res, sizeof(res), "ok\n") : snprintf(res, sizeof(res), "error %s\n", why2);
    if (hold_checked_snprintf(res_path, sizeof(res_path) - 8, "%s/chat/mux.%d.result", m->store->base, (int)getpid()) == 0)
        mux_write_small(res_path, res, (size_t)rl);
}
