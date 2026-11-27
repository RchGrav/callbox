/* console/mux_pane.c - one pane's wire to its broker (open, send, resize,
 * close), what its process is and how busy, and running backmux itself for
 * verbs the compositor borrows (rename, save, starting a process). */
#include "mux.h"

bool mux_pane_locked(const struct pane *p) {
    for (int i = 0; p && p->id[0] && i < MUX_MAX_PANES; i++)
        if (!strcmp(g_locked[i], p->id)) return true;
    return false;
}

void mux_set_locked(const struct pane *p, bool on) {
    if (!p || !p->id[0]) return;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (!strcmp(g_locked[i], p->id)) g_locked[i][0] = '\0';
    for (int i = 0; on && i < MUX_MAX_PANES; i++)
        if (!g_locked[i][0]) {
            snprintf(g_locked[i], sizeof(g_locked[i]), "%s", p->id);
            return;
        }
}

int64_t mux_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void mux_notice(struct mux *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->notice, sizeof(m->notice), fmt, ap);
    va_end(ap);
    m->notice_until = m->now + 3000;
    m->dirty = true;
}

int mux_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char h[CONSOLE_FRAME_HEADER_LEN];
    h[0] = type;
    console_store_be16(h + 1, len);
    if (hold_write_all(fd, h, sizeof(h)) != 0) return -1;
    return len ? hold_write_all(fd, payload, len) : 0;
}

void mux_pane_send(struct pane *p, const void *bytes, size_t n) {
    const unsigned char *b = bytes;
    while (n > 0 && p->wfd >= 0 && !p->ended) {
        uint16_t k = n > 4096 ? 4096 : (uint16_t)n;
        if (mux_frame(p->wfd, CONSOLE_FRAME_DATA, b, k) != 0) {
            p->ended = true;
            return;
        }
        b += k;
        n -= k;
    }
}

void mux_pane_resize(struct pane *p, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    int r0, c0;
    hold_screen_size(p->scr, &r0, &c0);
    if (r0 != rows || c0 != cols) hold_screen_resize(p->scr, rows, cols);
    unsigned char payload[4];
    console_store_be16(payload, (uint16_t)rows);
    console_store_be16(payload + 2, (uint16_t)cols);
    if (p->wfd >= 0 && mux_frame(p->wfd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload)) != 0) p->ended = true;
}

void mux_pane_free(struct pane *p) {
    if (!p) return;
    if (p->wfd >= 0 && p->wfd != p->rfd) close(p->wfd);
    if (p->rfd >= 0) close(p->rfd);
    hold_screen_free(p->scr);
    free(p);
}

/* Attach to a process's console as a pane, retrying connection-refused
 * errors briefly. */
struct pane *mux_pane_open(const char *id, const char *name, const char *sock, int rows, int cols, const char **why) {
    *why = NULL;
    for (int tries = 0; tries < 20; tries++) {
        int w = -1;
        int fd = hold_open_channel(sock, 'A', &w);
        if (fd < 0) {
            if (errno == EBUSY) {
                struct timespec sl = {0, 50 * 1000000L};
                nanosleep(&sl, NULL);
                continue;
            }
            *why = errno == ENOENT || errno == ECONNREFUSED ? "console unavailable" : strerror(errno);
            return NULL;
        }
        struct pane *p = calloc(1, sizeof(*p));
        if (!p || !(p->scr = hold_screen_new(rows > 0 ? rows : 1, cols > 0 ? cols : 1))) {
            free(p);
            if (w != fd) close(w);
            close(fd);
            *why = "out of memory";
            return NULL;
        }
        p->rfd = fd;
        p->wfd = w;
        snprintf(p->id, sizeof(p->id), "%s", id);
        snprintf(p->name, sizeof(p->name), "%s", name);
        snprintf(p->sock, sizeof(p->sock), "%s", sock);
        /* NO_TERMINAL: the broker's own screen answers the program's
         * queries; this side only mirrors. */
        if (hold_write_all(w, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0 ||
            mux_frame(w, CONSOLE_FRAME_NO_TERMINAL, NULL, 0) != 0) {
            mux_pane_free(p);
            *why = "console handshake failed";
            return NULL;
        }
        mux_pane_resize(p, rows, cols);
        /* A refusal comes as one line of text before the broker hangs up. */
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 150) > 0) {
            ssize_t n = read(fd, p->held, sizeof(p->held));
            if (n > 0) p->held_len = (size_t)n;
            static const char lead[] = "backmux: console already attached";
            if (n > 0 && (size_t)n >= sizeof(lead) - 1 && !memcmp(p->held, lead, sizeof(lead) - 1)) {
                mux_pane_free(p);
                struct timespec sl = {0, 50 * 1000000L};
                nanosleep(&sl, NULL);
                *why = "console already attached";
                continue;
            }
            if (n == 0) {
                mux_pane_free(p);
                *why = "console connection closed";
                return NULL;
            }
            if (n > 0) hold_screen_feed(p->scr, p->held, (size_t)n);
            hold_screen_discard_output(p->scr);
            p->held_len = 0;
            p->heard = n > 0;
        }
        (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        return p;
    }
    if (!*why) *why = "console busy";
    return NULL;
}

/* A pane's process, from its record: the pid its load is read from, and
 * what it runs - the harness (chat talks only to agents) or the program. */
void mux_pane_identify(struct mux *m, struct pane *p) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (!p->id[0] || hold_load_record_by_id(m->store->record_dir, p->id, &r, path, sizeof(path)) != 0) return;
    p->pid = r.pid;
    p->agent[0] = '\0';
    (void)hold_agent_of_record(&r, p->agent, sizeof(p->agent));
    if (r.recipe.argc > 0 && r.recipe.argv && r.recipe.argv[0]) {
        const char *b = strrchr(r.recipe.argv[0], '/');
        snprintf(p->program, sizeof(p->program), "%s", b ? b + 1 : r.recipe.argv[0]);
    }
    if (r.has_name && r.name[0]) snprintf(p->name, sizeof(p->name), "%s", r.name); /* renamed elsewhere */
    hold_free_run_record(&r);
}

/* A pane's load: its terminal's foreground job when it has one (claude run
 * from a shell is what is busy, not the shell), else its program. */
void mux_pane_load(struct pane *p, int64_t now) {
    pid_t at = p->pid, fg = 0;
    if (at <= 0) return;
    if (hold_proc_foreground_group(at, &fg) == 0 && fg > 0) at = fg;
    uint64_t ticks = 0, rss = 0;
    if (hold_proc_read_cpu_rss(at, &ticks, &rss) != 0) {
        p->cpu_pct = -1;
        p->rss = 0;
        return;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (p->cpu_at && now > p->cpu_at && ticks >= p->cpu_prev && hz > 0)
        p->cpu_pct = (int)((ticks - p->cpu_prev) * 100000 / (uint64_t)hz / (uint64_t)(now - p->cpu_at));
    else p->cpu_pct = -1;
    p->cpu_prev = ticks;
    p->cpu_at = now;
    p->rss = rss;
}

/* ---- running backmux itself --------------------------------------------
 * Rename and save are backmux's own verbs, with their checks and records;
 * this layer never reaches into the store's writers, so it runs them. */
/* out (when given) gets the first line it printed: a launch's process id. */
int mux_run_self(char *const args[], char *err, size_t en, char *out, size_t on) {
    return mux_run_self_in(NULL, args, err, en, out, on);
}

/* As mux_run_self, started in cwd (NULL: here). */
int mux_run_self_in(const char *cwd, char *const args[], char *err, size_t en, char *out, size_t on) {
    if (err && en) err[0] = '\0';
    if (out && on) out[0] = '\0';
    char self[HOLD_PATH_MAX];
    if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) return -1;
    /* Files, not pipes: nothing to drain while waiting, and no broker is
     * started that could hold a pipe open. */
    FILE *fe = tmpfile(), *fo = out ? tmpfile() : NULL;
    if (!fe || (out && !fo)) {
        if (fe) fclose(fe);
        if (fo) fclose(fo);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fclose(fe);
        if (fo) fclose(fo);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
        }
        if (fo) dup2(fileno(fo), STDOUT_FILENO);
        dup2(fileno(fe), STDERR_FILENO);
        if (cwd && chdir(cwd) != 0) { /* gone since: start where we are */
        }
        int argc = 0;
        while (args[argc]) argc++;
        char **argv = calloc((size_t)argc + 2, sizeof(char *));
        if (!argv) _exit(126);
        argv[0] = "backmux";
        for (int i = 0; i < argc; i++) argv[i + 1] = args[i];
        execv(self, argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
    if (err && en) {
        rewind(fe);
        size_t got = fread(err, 1, en - 1, fe);
        err[got] = '\0';
        err[strcspn(err, "\n")] = '\0';
        const char *strip = "backmux: error: ";
        if (!strncmp(err, strip, strlen(strip))) memmove(err, err + strlen(strip), strlen(err + strlen(strip)) + 1);
    }
    fclose(fe);
    if (fo) {
        rewind(fo);
        size_t got = fread(out, 1, on - 1, fo);
        out[got] = '\0';
        out[strcspn(out, "\n")] = '\0';
        fclose(fo);
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
