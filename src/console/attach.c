#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/console.h"
#include "backmux/core.h"
#include "backmux/term.h"
#include "backmux/console_internal.h"
#include "backmux/screen.h"

#define CONSOLE_ATTACH_CTRL_P 0x10
#define CONSOLE_ATTACH_CTRL_Q 0x11

static unsigned char g_detach_keys[HOLD_TERM_DETACH_MAX_KEYS] = {
    CONSOLE_ATTACH_CTRL_P,
    CONSOLE_ATTACH_CTRL_Q,
};
static size_t g_detach_key_count = 2;

static char g_enclosing_sock[HOLD_PATH_MAX];

/* The person's terminal, owned across a hand-off: plain attach and the
 * compositor (mux.c) take turns at it without leaving the alternate screen
 * in between, so going from one call to panes and back never flashes the
 * shell underneath. Restored once, by whoever has it last. */
struct hold_console_term g_console_term = {.guard_fd = -1};

/* What gives the person's terminal back: every mode a program inside (or
 * the compositor) may have set, then off the alternate screen. */
static const char g_reset_modes[] =
    "\033[?2026l\033[0m"
    "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l"
    "\033[?2004l\033[?1004l\033[?1l\033>\033[0 q\033[?25h";

/* A guard for the terminal: a tiny child that holds nothing but the
 * terminal and a pipe, and waits. If the client goes without saying so -
 * kill -9, a crash - the pipe closes and the guard gives the terminal back:
 * its settings, mouse reporting off, off the alternate screen. Why: nothing
 * in a process can catch kill -9, and a terminal left raw with every mouse
 * movement typed into the shell is unusable. Why a child, not a parent that
 * supervises: the backmux the person ran keeps its pid and its exit status,
 * which scripts rely on. It ignores SIGTTOU: once the client is gone the
 * shell has the foreground, and the guard restores from the background. */
static void start_guard(struct hold_console_term *t) {
    int p[2];
    if (pipe(p) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return;
    }
    if (pid == 0) {
        for (int fd = 3; fd < 1024; fd++)
            if (fd != p[0]) close(fd); /* never hold a console connection open */
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTERM, SIG_IGN); /* a TERM to the client is handled there */
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        char c = 0;
        ssize_t n;
        do n = read(p[0], &c, 1);
        while (n < 0 && errno == EINTR);
        if (n == 1) _exit(0); /* the client let go itself */
        (void)tcsetattr(t->saved.fd, TCSANOW, &t->saved.t);
        (void)hold_write_all(STDOUT_FILENO, g_reset_modes, sizeof(g_reset_modes) - 1);
        if (t->alt_screen) (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
        _exit(0);
    }
    close(p[0]);
    (void)fcntl(p[1], F_SETFD, FD_CLOEXEC);
    t->guard_fd = p[1];
    t->guard_pid = pid;
}

static void stop_guard(struct hold_console_term *t) {
    if (t->guard_fd < 0) return;
    (void)hold_write_all(t->guard_fd, "q", 1);
    close(t->guard_fd);
    if (t->guard_pid > 0) (void)waitpid(t->guard_pid, NULL, 0);
    t->guard_fd = -1;
    t->guard_pid = 0;
}

void hold_console_term_release(void) {
    struct hold_console_term *t = &g_console_term;
    if (!t->owned) return;
    stop_guard(t);
    if (t->saved.saved) {
        /* A program run inside the console may have left terminal-global modes
         * enabled - mouse reporting, bracketed paste, focus reports, application
         * keypad/cursor keys, a cursor style, a hidden cursor - and the
         * compositor sets them on its behalf (and may be cut off inside a
         * synchronized update). These survive the PTY teardown and would leak
         * back to the user's shell, so reset them before handing the terminal
         * back. */
        (void)hold_write_all(STDOUT_FILENO, g_reset_modes, sizeof(g_reset_modes) - 1);
    }
    if (t->alt_screen) (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
    if (t->saved.saved) {
        (void)hold_write_all(STDOUT_FILENO, "\033[?25h", 6);
        hold_term_input_restore(&t->saved);
    }
    memset(t, 0, sizeof(*t));
    t->guard_fd = -1;
}

static bool g_inline, g_was_inline;
/* Inline: whether the program has switched the person's terminal to its own
 * alternate screen (vim, htop do), so letting go can switch it back - else
 * their prompt would land inside the program's screen. */
static bool g_prog_alt;

void hold_console_set_inline(bool on) {
    g_inline = on;
}

/* The compositor split an inline screen: from here the screen is all
 * backmux's, so it goes to the alternate screen, and letting go gives the
 * person back their own screen exactly as the launch left it - not panes,
 * split lines and stale prompts. A hand-back to one pane is then an ordinary
 * attach on the alternate screen: repainted, not a stream already replayed. */
void hold_console_leave_inline(void) {
    struct hold_console_term *t = &g_console_term;
    if (!g_was_inline || !t->owned) return;
    if (!t->alt_screen) (void)hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15);
    t->alt_screen = true;
    g_was_inline = false;
    g_prog_alt = false;
}

bool hold_console_was_inline(void) {
    return g_was_inline;
}

/* The program's own alternate-screen switches, seen in its output. */
static void track_alt(const char *buf, size_t n) {
    static const struct { const char *seq; bool on; } sw[] = {
        {"\033[?1049h", true}, {"\033[?1049l", false}, {"\033[?1047h", true},
        {"\033[?1047l", false}, {"\033[?47h", true},    {"\033[?47l", false},
    };
    for (size_t i = 0; i + 5 <= n; i++) {
        if (buf[i] != '\033') continue;
        for (size_t k = 0; k < sizeof(sw) / sizeof(sw[0]); k++) {
            size_t l = strlen(sw[k].seq);
            if (i + l <= n && !memcmp(buf + i, sw[k].seq, l)) g_prog_alt = sw[k].on;
        }
    }
}

static const struct hold_store *g_mux_store;
static char g_chat_name[ALIAS_MAX_LEN + 1];
static unsigned char g_mux_cmd; /* the command key that ended the last attach, 0 none */

void hold_console_set_mux(const struct hold_store *store, const char *chat_name) {
    g_mux_store = store;
    snprintf(g_chat_name, sizeof(g_chat_name), "%s", chat_name ? chat_name : "");
}
/* How the last attach ended (see console.h). */
static bool g_refused, g_gone, g_composited, g_line_mode, g_stream;
static pid_t g_refused_by;

bool hold_console_was_refused(pid_t *by) {
    if (by) *by = g_refused_by;
    return g_refused;
}

bool hold_console_was_gone(void) {
    return g_gone;
}

/* The person detached (the detach keys, or Ctrl-P Ctrl-Q with panes up):
 * the names let go, comma separated; "" when it was this process alone. A
 * newer attach taking the process sets nothing - that is not a detach. */
static bool g_detached;
static char g_detached_names[512];
/* The process a compositor handed back to plain attach: a detach from then
 * on is from it, not from the one the attach began with. */
static char g_attached_name[128];

void hold_console_set_attached_name(const char *name) {
    snprintf(g_attached_name, sizeof(g_attached_name), "%s", name ? name : "");
}

void hold_console_note_detached(const char *name) {
    if (!name) {
        g_detached = true;
        g_detached_names[0] = '\0';
        return;
    }
    size_t used = strlen(g_detached_names);
    snprintf(g_detached_names + used, sizeof(g_detached_names) - used, "%s%s", used ? ", " : "", name);
}

bool hold_console_was_detached(const char **names) {
    if (names) *names = g_detached_names;
    return g_detached;
}

static bool g_heard, g_reader_gone;

bool hold_console_reader_gone(void) {
    return g_reader_gone;
}

bool hold_console_heard_output(void) {
    return g_heard;
}

size_t hold_console_get_detach_keys(unsigned char *out, size_t cap) {
    memcpy(out, g_detach_keys, g_detach_key_count < cap ? g_detach_key_count : cap);
    return g_detach_key_count;
}

void hold_console_set_composited(bool on) {
    g_composited = on;
}

void hold_console_set_line_mode(bool on) {
    g_line_mode = on;
}

void hold_console_set_stream(bool on) {
    g_stream = on;
}

static bool g_forward_term;

void hold_console_set_forward_term(bool on) {
    g_forward_term = on;
}

/* The broker's words to a client it turns away: "backmux: console already
 * attached (by pid N)". Returns whether text is that refusal; *by the pid. */
static bool parse_refusal(const char *text, size_t n, pid_t *by) {
    static const char lead[] = "backmux: console already attached";
    *by = 0;
    if (n < sizeof(lead) - 1 || memcmp(text, lead, sizeof(lead) - 1) != 0) return false;
    const char *p = memchr(text, '(', n);
    if (p && (size_t)(p - text) + 8 < n && !memcmp(p, "(by pid ", 8)) *by = (pid_t)strtol(p + 8, NULL, 10);
    return true;
}

int hold_console_set_detach_keys(const unsigned char *keys, size_t len) {
    if (!keys || len == 0 || len > HOLD_TERM_DETACH_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }
    memcpy(g_detach_keys, keys, len);
    g_detach_key_count = len;
    return 0;
}

void hold_console_set_enclosing(const char *sock_path) {
    snprintf(g_enclosing_sock, sizeof(g_enclosing_sock), "%s", sock_path ? sock_path : "");
}

/* We are an attach client in another call's terminal: say so to that call's
 * broker, with our detach keys, over a control connection held open until we
 * exit (its closing is how that broker learns we have gone). The person's
 * detach keys reach the outermost client first; its broker hands them on to
 * us instead of detaching, so they let go of the innermost attach - the one
 * being looked at. Returns the connection's read end (*writer its write
 * end), or -1 when there is no enclosing console or it does not answer. */
static int register_nested(const char *own_sock, int *writer) {
    *writer = -1;
    if (!g_enclosing_sock[0] || !strcmp(g_enclosing_sock, own_sock)) return -1;
    int out = -1;
    int fd = hold_open_channel(g_enclosing_sock, 'C', &out);
    if (fd < 0) return -1;
    char req[32 + 2 * HOLD_TERM_DETACH_MAX_KEYS];
    int n = snprintf(req, sizeof(req), "nested keys=");
    for (size_t i = 0; i < g_detach_key_count; i++) n += snprintf(req + n, sizeof(req) - (size_t)n, "%02x", g_detach_keys[i]);
    unsigned char hdr[CONSOLE_CONTROL_MAGIC_LEN + 4];
    memcpy(hdr, CONSOLE_CONTROL_MAGIC, CONSOLE_CONTROL_MAGIC_LEN);
    console_store_be32(hdr + CONSOLE_CONTROL_MAGIC_LEN, (uint32_t)n);
    /* over FIFOs the introduction already said "control": no magic */
    size_t skip = fd == out ? 0 : CONSOLE_CONTROL_MAGIC_LEN;
    if (hold_write_all(out, hdr + skip, sizeof(hdr) - skip) != 0 || hold_write_all(out, req, (size_t)n) != 0) {
        if (out != fd) close(out);
        close(fd);
        return -1;
    }
    *writer = out;
    return fd;
}

static int64_t attach_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* hold_write_all, but a pending SIGTERM or SIGHUP ends the wait: a client
 * blocked on a broker that is not reading (its program's input is full) must
 * still restore the terminal and go when told to. */
static int write_all_stoppable(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR && !hold_term_stop_signal()) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Client-side wire: 3-byte type + be16-length frames after the attach magic. */
static int write_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char header[CONSOLE_FRAME_HEADER_LEN];
    header[0] = type;
    console_store_be16(header + 1, len);
    if (write_all_stoppable(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && write_all_stoppable(fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int send_resize(int fd, const struct winsize *ws) {
    if (ws->ws_row == 0 || ws->ws_col == 0) {
        return 0;
    }
    unsigned char payload[4];
    console_store_be16(payload, ws->ws_row);
    console_store_be16(payload + 2, ws->ws_col);
    return write_frame(fd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload));
}

/* One attach, as the client loop sees it. */
struct attach_state {
    int sock, writer;
    bool heard;              /* anything from the broker yet */
    int64_t detach_sent_ms;  /* when the detach keys were last sent on */
    char held[160];          /* text that may be the broker turning us away */
    size_t held_len;
    bool holding;
    bool resize_failed;
    bool line_mode;           /* the program is on pipes: keep the terminal's lines */
    bool hold_keys;           /* Ctrl-Space held hands over to the compositor */
    bool nul_pending;         /* a Ctrl-Space waiting to see whether it repeats */
    int64_t nul_first;
    struct hold_term_line line;
};

/* A frame that cannot be sent because the broker has gone (the call ended
 * while input was still coming): not an error - stop, and let what the call
 * did decide the exit status. */
static int sent_or_over(int rc) {
    if (rc == 0) return 0;
    return (errno == EPIPE || errno == ECONNRESET) ? 1 : -1;
}

/* The lead key then a pane or floating-window key: let go of this console
 * (the broker closes on DETACH) and hand the terminal to the compositor. */
static int attach_on_command(void *ctx, unsigned char key) {
    struct attach_state *a = ctx;
    if (a->writer >= 0) (void)write_frame(a->writer, CONSOLE_FRAME_DETACH, NULL, 0);
    a->detach_sent_ms = attach_now_ms();
    g_mux_cmd = key;
    return 1;
}

/* Ctrl-Space (NUL): a second one within the key-repeat delay is a hold -
 * the compositor takes over to show the keys and names; a lone one goes
 * to the program once it is clear no repeat follows. */
#define ATTACH_TAP_MS 700

/* Bytes the keys release go to the broker as DATA frames. */
static int attach_on_input(void *ctx, const unsigned char *bytes, size_t n) {
    struct attach_state *a = ctx;
    if (a->writer < 0) return 0;
    if (a->hold_keys) {
        size_t from = 0;
        for (size_t i = 0; i < n; i++) {
            if (bytes[i] != 0x00) continue;
            if (i > from) {
                int rc = sent_or_over(write_frame(a->writer, CONSOLE_FRAME_DATA, bytes + from, (uint16_t)(i - from)));
                if (rc != 0) return rc;
            }
            from = i + 1;
            int64_t now = attach_now_ms();
            if (a->nul_pending && now - a->nul_first <= ATTACH_TAP_MS) {
                a->nul_pending = false;
                (void)write_frame(a->writer, CONSOLE_FRAME_DETACH, NULL, 0);
                a->detach_sent_ms = now;
                g_mux_cmd = 0x80; /* held: the compositor shows the keys */
                return 1;
            }
            a->nul_pending = true;
            a->nul_first = now;
        }
        if (from >= n) return 0;
        bytes += from;
        n -= from;
    }
    return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_DATA, bytes, (uint16_t)n));
}

static int64_t attach_due_in(void *ctx) {
    struct attach_state *a = ctx;
    if (!a->nul_pending) return -1;
    int64_t left = a->nul_first + ATTACH_TAP_MS - attach_now_ms();
    return left < 0 ? 0 : left;
}

static int attach_on_timer(void *ctx) {
    struct attach_state *a = ctx;
    if (!a->nul_pending || attach_now_ms() - a->nul_first <= ATTACH_TAP_MS) return 0;
    a->nul_pending = false;
    static const unsigned char nul = 0; /* a tap: the program's */
    return a->writer >= 0 ? sent_or_over(write_frame(a->writer, CONSOLE_FRAME_DATA, &nul, 1)) : 0;
}

/* A program on pipes: keys go through the line editor, and only whole lines
 * (or what Ctrl-D sends) reach it. */
static int attach_line_sink(void *ctx, const unsigned char *bytes, size_t n) {
    return attach_on_input(ctx, bytes, n) == 0 ? 0 : -1;
}

static int attach_on_line_input(void *ctx, const unsigned char *bytes, size_t n) {
    struct attach_state *a = ctx;
    for (size_t i = 0; i < n; i++) {
        bool eof = false;
        if (hold_term_line_feed(&a->line, bytes[i], attach_line_sink, a, &eof) != 0) return sent_or_over(-1);
        if (eof) {
            int rc = sent_or_over(write_frame(a->writer, CONSOLE_FRAME_EOF, NULL, 0));
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* Ctrl-C for a program on pipes: no PTY turns the key into a signal, so the
 * broker sends SIGINT; the half-typed line is dropped, as a terminal does. */
static int attach_on_interrupt(void *ctx) {
    struct attach_state *a = ctx;
    hold_term_line_discard(&a->line);
    (void)hold_write_all(STDOUT_FILENO, "^C\r\n", 4);
    return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_INTERRUPT, NULL, 0));
}

/* SIGINT sent to the client (Ctrl-C with its output piped, or a kill from a
 * script): it is the program's, as in docker run. On a PTY it goes in as the
 * key, so the program's own terminal signals it. */
static int attach_on_pty_interrupt(void *ctx) {
    struct attach_state *a = ctx;
    if (a->writer < 0) return 0;
    return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_DATA, "\x03", 1));
}

/* The detach keys: the broker answers by closing the connection - or, when
 * an attach nested in this terminal is what the person is looking at, by
 * handing the keys to that one and keeping us. */
static int attach_on_detach(void *ctx) {
    struct attach_state *a = ctx;
    if (write_frame(a->writer, CONSOLE_FRAME_DETACH, NULL, 0) != 0) return -1;
    a->detach_sent_ms = attach_now_ms();
    return 0;
}

static int attach_on_input_end(void *ctx) {
    struct attach_state *a = ctx;
    if (!g_stream) {
        /* backmux attach whose input ran out: it leaves (closing our side is
         * how the broker learns), as it always has - the program's input
         * stays open for the next attach, and a piped attach returns. */
        if (a->writer == a->sock) {
            shutdown(a->sock, SHUT_WR);
        } else if (a->writer >= 0) {
            close(a->writer);
            a->writer = -1;
        }
        return 0;
    }
    if (a->line_mode) {
        /* A launch whose piped input ran out: the program reads EOF, and its
         * output goes on until it ends. */
        return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_EOF, NULL, 0));
    }
    /* On a PTY, closing our side would read to the broker as leaving: stay,
     * and show the output until the program ends - `backmux make test
     * </dev/null` from a script must not detach at once. A launch from a
     * script passes its input's end on (the broker types the terminal's
     * end-of-input key, Ctrl-D, without echoing it), so a program reading
     * stdin finishes, as it would have on /dev/null. backmux attach does
     * not: that would end an attached shell. */
    if (g_stream && a->writer >= 0) return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_EOF, NULL, 0));
    return 0;
}

static int attach_on_resize(void *ctx, const struct winsize *ws) {
    struct attach_state *a = ctx;
    if (a->writer < 0 || send_resize(a->writer, ws) == 0) return 0;
    if (!a->heard && (errno == EPIPE || errno == ECONNRESET)) a->resize_failed = true;
    return -1;
}

/* Our reader went away (`backmux foo | head`): stop showing output. The
 * call itself is not ours to end - it keeps running - and the caller says
 * so, then ends as a writer to a closed pipe does. */
static int stdout_failed(void) {
    if (errno != EPIPE) return -1;
    g_reader_gone = true;
    return 1;
}

/* A broker that turns us away says so in a line of text and hangs up. Text
 * that starts like that line is held back until it is clear which it is:
 * written to the terminal it would be wiped with the alternate screen, and
 * the person would see an attach that silently returns. */
static int attach_on_output(void *ctx, const char *buf, size_t n) {
    struct attach_state *a = ctx;
    a->heard = true;
    if (g_was_inline) track_alt(buf, n);
    const char *out = buf;
    size_t out_len = n;
    if (a->holding) {
        static const char lead[] = "backmux: ";
        size_t room = sizeof(a->held) - a->held_len, take = out_len < room ? out_len : room;
        memcpy(a->held + a->held_len, out, take);
        a->held_len += take;
        size_t cmp = a->held_len < sizeof(lead) - 1 ? a->held_len : sizeof(lead) - 1;
        if (take == out_len && memcmp(a->held, lead, cmp) == 0) return 0; /* may be a refusal: wait */
        /* a screen after all: what was held, then the rest */
        a->holding = false;
        if (hold_write_all(STDOUT_FILENO, a->held, a->held_len) != 0) return stdout_failed();
        out += take;
        out_len -= take;
    }
    if (out_len > 0 && hold_write_all(STDOUT_FILENO, out, out_len) != 0) return stdout_failed();
    return 0;
}

/* A broker that turns us away writes its reason and hangs up, and may do so
 * before our opening bytes reach it - then our write breaks first, with the
 * reason still unread in the connection. Read what is there (briefly: a
 * broker that went away gives its end at once) before calling the console
 * gone. Why: under load a refused attach said "has closed its terminal and
 * runs on" about a call that was running and attached elsewhere. */
static void read_parting_words(int sock, struct attach_state *a) {
    while (a->held_len < sizeof(a->held)) {
        struct pollfd p = {.fd = sock, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) return;
        char buf[sizeof(a->held)];
        ssize_t n = read(sock, buf, sizeof(a->held) - a->held_len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        memcpy(a->held + a->held_len, buf, (size_t)n);
        a->held_len += (size_t)n;
    }
}

static int attach_once(const char *sock_path) {
    int writer = -1;
    g_mux_cmd = 0;
    g_refused = g_gone = g_heard = g_reader_gone = false;
    g_detached = false;
    g_detached_names[0] = '\0';
    g_refused_by = 0;
    int sock = hold_open_channel(sock_path, 'A', &writer);
    if (sock < 0) {
        int e = errno;
        if (e == EBUSY) { /* over FIFOs the console says so before the connection exists */
            g_refused = true;
            g_refused_by = hold_fifo_busy_holder();
            return 3;
        }
        /* The console has closed: the call's output is over. A broker that
         * exits mid-handshake (a command that ended at once) shows up as a
         * refused, reset, or broken connection rather than a missing one. */
        if (e == ENOENT || e == ECONNREFUSED || e == ECONNRESET || e == EPIPE || e == ENXIO) {
            g_gone = true;
            return 3;
        }
        fprintf(stderr, "backmux: cannot attach: %s\n", strerror(e));
        return e == ENOTSOCK || e == ENAMETOOLONG ? 5 : 3;
    }

    /* A broker that hung up makes our writes fail (EPIPE), never end us:
     * the opening writes come before the client loop ignores SIGPIPE, and a
     * refused attach died of it (exit 141) instead of saying why. */
    struct sigaction ign_pipe = {.sa_handler = SIG_IGN}, old_pipe;
    sigemptyset(&ign_pipe.sa_mask);
    bool pipe_ignored = sigaction(SIGPIPE, &ign_pipe, &old_pipe) == 0;

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    struct hold_term_saved saved;
    memset(&saved, 0, sizeof(saved));
    bool alt_screen = false;
    struct hold_console_term *term = &g_console_term;
    if (g_line_mode) {
        /* A program on pipes: the terminal keeps its lines and its scrollback
         * (no alternate screen); keys are read one by one so the detach keys
         * and Ctrl-C can be seen, and the line editor does the rest. */
        if (interactive) (void)hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_KEYS, &saved);
    } else if (term->owned && g_was_inline) {
        /* handed back by the compositor, inline: what is on the screen is
         * the program's screen already - nothing is cleared or repainted */
    } else if (term->owned) {
        /* handed back by the compositor: raw, on the alternate screen; the
         * broker's repaint redraws it all */
        (void)hold_write_all(STDOUT_FILENO, "\033[0m\033[H\033[2J", 10);
    } else if (interactive && g_inline && hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_RAW, &saved) == 0 &&
               saved.saved) {
        /* A fresh launch, inline: no alternate screen and nothing cleared.
         * The program's output arrives as it was written (the broker replays
         * it from the start, then streams it), from where the cursor is;
         * the broker's model started on this row, so the screen below it is
         * exactly the model, and what is above stays as the person left it. */
        term->owned = true;
        term->saved = saved;
        term->alt_screen = false;
        g_was_inline = true;
        g_prog_alt = false;
        start_guard(term);
    } else if (interactive && hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_RAW, &saved) == 0 && saved.saved &&
               hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
        alt_screen = true;
        term->owned = true;
        term->saved = saved;
        term->alt_screen = true;
        start_guard(term);
    }

    struct attach_state a = {.sock = sock, .writer = writer, .holding = true, .line_mode = g_line_mode};
    hold_term_line_init(&a.line, STDOUT_FILENO);
    int rc = 0;
    int nest_writer = -1;
    int nest = interactive ? register_nested(sock_path, &nest_writer) : -1;
    struct hold_term_detach detach;
    hold_term_detach_init(&detach, g_detach_keys, g_composited ? 0 : g_detach_key_count);
    if (g_line_mode) hold_term_detach_set_interrupt(&detach, 0x03); /* Ctrl-C */
    /* Ctrl-P then A (attach another call beside this one), T (chat), H
     * (help), N (names), W (next window): the compositor's. Only with the
     * default two-key lead form and a terminal to draw on. */
    bool mux_keys = interactive && !g_line_mode && !g_composited && g_mux_store && g_detach_key_count == 2 &&
                    term->owned;
    a.hold_keys = mux_keys;
    if (mux_keys) {
        /* A attach, T chat, S swap, G go to window, M move, F full, J jump,
         * W next window */
        static const unsigned char cmds[] = {0x01, 0x14, 0x13, 0x07, 0x0d, 0x06, 0x0a, 0x17};
        hold_term_detach_set_commands(&detach, cmds, sizeof(cmds));
    }
    enum hold_client_end end = HOLD_CLIENT_OUTPUT_ENDED;
    if (hold_write_all(writer, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        if (errno == EPIPE || errno == ECONNRESET) g_gone = true; /* ended as we arrived */
        rc = 3;
    } else if (((g_stream && !interactive) || g_was_inline) && write_frame(writer, CONSOLE_FRAME_STREAM, NULL, 0) != 0) {
        /* Not a terminal: ask for the output as a stream, not a screen. */
        if (errno == EPIPE || errno == ECONNRESET) g_gone = true;
        rc = 3;
    } else if (!interactive && write_frame(writer, CONSOLE_FRAME_NO_TERMINAL, NULL, 0) != 0) {
        /* Nobody here answers the program's terminal queries: the screen will. */
        if (errno == EPIPE || errno == ECONNRESET) g_gone = true;
        rc = 3;
    } else {
        struct hold_client c = {
            .out_fd = sock,
            .on_output = attach_on_output,
            /* macOS poll() never reports end-of-file on a FIFO, so over the
             * FIFO console (a separate write fd) a quiet pass probes. */
#if defined(__APPLE__)
            .probe_fifo = writer != sock,
#endif
            .in_fd = STDIN_FILENO,
            .keys = interactive ? &detach : NULL,
            .on_input = g_line_mode && interactive ? attach_on_line_input : attach_on_input,
            .on_detach = attach_on_detach,
            .on_interrupt = g_line_mode ? attach_on_interrupt : attach_on_pty_interrupt,
            .want_sigint = true,
            .on_input_end = attach_on_input_end,
            .on_resize = attach_on_resize,
            .on_command = mux_keys ? attach_on_command : NULL,
            .due_in = mux_keys ? attach_due_in : NULL,
            .on_timer = mux_keys ? attach_on_timer : NULL,
            .ctx = &a,
        };
        end = hold_client_run(&c);
        if (hold_term_stop_signal()) end = HOLD_CLIENT_SIGNALED; /* even if it broke a blocked write */
        if (end == HOLD_CLIENT_SIGNALED && g_forward_term && hold_term_stop_signal() == SIGTERM && a.writer >= 0) {
            /* A launch told to stop (`timeout 8 backmux foo`) passes SIGTERM on
             * to its program, as docker run's sig-proxy does: the program was
             * run here, and must not be left behind unseen. (SIGHUP - the
             * terminal went away - detaches: backmux keeps programs running.) */
            unsigned char sig = SIGTERM;
            (void)write_frame(a.writer, CONSOLE_FRAME_INTERRUPT, &sig, 1);
        }
        if (end == HOLD_CLIENT_SIGNALED) rc = 128 + hold_term_stop_signal();
        else if (end == HOLD_CLIENT_ERROR) rc = 3;
        if (a.resize_failed) g_gone = true;
    }

    /* The connection closing just after the detach keys is the detach. */
    if (a.detach_sent_ms && attach_now_ms() - a.detach_sent_ms < 2000) rc = 0;
    if (a.detach_sent_ms && !g_mux_cmd) {
        hold_console_note_detached(NULL);
        if (g_attached_name[0]) hold_console_note_detached(g_attached_name);
    }
    /* Inline and letting go: the program's own full screen (vim) is switched
     * back, or, if there is none, the prompt that follows starts on a line
     * of its own instead of after the program's. A program that ended has
     * already left its cursor on a fresh line. */
    if (g_was_inline && !g_mux_cmd && term->owned) {
        if (g_prog_alt) (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
        else if (a.detach_sent_ms) (void)hold_write_all(STDOUT_FILENO, "\r\n", 2);
        g_prog_alt = false;
    }
    if (g_gone && !a.heard && a.holding) {
        /* Only a refusal is taken from what is read after the break. A broker
         * whose program ended as we arrived may have left a screen repaint
         * (sent before our stream request reached it); printing that would
         * put a terminal redraw where the run's output belongs, which the
         * caller prints from the log for a console that is gone. */
        size_t before = a.held_len;
        read_parting_words(sock, &a);
        if (!parse_refusal(a.held, a.held_len, &g_refused_by)) a.held_len = before;
    }
    if (a.holding && a.held_len > 0) {
        if (parse_refusal(a.held, a.held_len, &g_refused_by)) {
            g_refused = true;
            g_gone = false; /* turned away, not over */
            rc = 3;
        } else {
            (void)hold_write_all(STDOUT_FILENO, a.held, a.held_len); /* a program's own (short) output */
        }
    }
    if (nest >= 0) {
        if (nest_writer != nest) close(nest_writer);
        close(nest);
    }
    (void)alt_screen;
    if (g_line_mode) {
        if (saved.saved) hold_term_input_restore(&saved);
    } else if (!g_mux_cmd || end == HOLD_CLIENT_SIGNALED) {
        hold_console_term_release(); /* (kept for the compositor on a hand-off) */
    }
    g_heard = a.heard;
    if (a.writer >= 0 && a.writer != sock) close(a.writer);
    close(sock);
    if (pipe_ignored) sigaction(SIGPIPE, &old_pipe, NULL);
    if (end == HOLD_CLIENT_SIGNALED) {
        g_mux_cmd = 0;
        hold_term_reraise_stop();
    }
    return rc;
}

int hold_run_native_console(const char *sock_path) {
    char cur[HOLD_PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", sock_path);
    g_attached_name[0] = '\0';
    for (;;) {
        int rc = attach_once(cur);
        if (!g_mux_cmd) return rc;
        /* Panes or a floating window were asked for: the compositor takes the
         * terminal. With one pane left and nothing floating it hands back the
         * call to show plainly, and this loop attaches to it as before. */
        char back[HOLD_PATH_MAX] = "";
        bool handed_back = false;
        rc = hold_mux_run(g_mux_store, g_chat_name, cur, g_mux_cmd, back, sizeof(back), &handed_back);
        if (!handed_back) {
            hold_console_term_release();
            return rc;
        }
        snprintf(cur, sizeof(cur), "%s", back);
    }
}
