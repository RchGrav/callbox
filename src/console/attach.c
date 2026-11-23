#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/console.h"
#include "callbox/core.h"
#include "callbox/term.h"
#include "callbox/console_internal.h"

#define CONSOLE_ATTACH_CTRL_P 0x10
#define CONSOLE_ATTACH_CTRL_Q 0x11

static unsigned char g_detach_keys[HOLD_TERM_DETACH_MAX_KEYS] = {
    CONSOLE_ATTACH_CTRL_P,
    CONSOLE_ATTACH_CTRL_Q,
};
static size_t g_detach_key_count = 2;

static char g_enclosing_sock[HOLD_PATH_MAX];
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

/* The broker's words to a client it turns away: "callbox: console already
 * attached (by pid N)". Returns whether text is that refusal; *by the pid. */
static bool parse_refusal(const char *text, size_t n, pid_t *by) {
    static const char lead[] = "callbox: console already attached";
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
    struct hold_term_line line;
};

/* A frame that cannot be sent because the broker has gone (the call ended
 * while input was still coming): not an error - stop, and let what the call
 * did decide the exit status. */
static int sent_or_over(int rc) {
    if (rc == 0) return 0;
    return (errno == EPIPE || errno == ECONNRESET) ? 1 : -1;
}

/* Bytes the keys release go to the broker as DATA frames. */
static int attach_on_input(void *ctx, const unsigned char *bytes, size_t n) {
    struct attach_state *a = ctx;
    if (a->writer < 0) return 0;
    return sent_or_over(write_frame(a->writer, CONSOLE_FRAME_DATA, bytes, (uint16_t)n));
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
        /* callbox attach whose input ran out: it leaves (closing our side is
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
     * and show the output until the program ends - `callbox make test
     * </dev/null` from a script must not detach at once. A launch from a
     * script passes its input's end on (the broker types the terminal's
     * end-of-input key, Ctrl-D, without echoing it), so a program reading
     * stdin finishes, as it would have on /dev/null. callbox attach does
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

/* Our reader went away (`callbox foo | head`): stop showing output. The
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
    const char *out = buf;
    size_t out_len = n;
    if (a->holding) {
        static const char lead[] = "callbox: ";
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

int hold_run_native_console(const char *sock_path) {
    int writer = -1;
    g_refused = g_gone = g_heard = g_reader_gone = false;
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
        fprintf(stderr, "callbox: cannot attach: %s\n", strerror(e));
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
    if (g_line_mode) {
        /* A program on pipes: the terminal keeps its lines and its scrollback
         * (no alternate screen); keys are read one by one so the detach keys
         * and Ctrl-C can be seen, and the line editor does the rest. */
        if (interactive) (void)hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_KEYS, &saved);
    } else if (interactive && hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_RAW, &saved) == 0 && saved.saved &&
               hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
        alt_screen = true;
    }

    struct attach_state a = {.sock = sock, .writer = writer, .holding = true, .line_mode = g_line_mode};
    hold_term_line_init(&a.line, STDOUT_FILENO);
    int rc = 0;
    int nest_writer = -1;
    int nest = interactive ? register_nested(sock_path, &nest_writer) : -1;
    struct hold_term_detach detach;
    hold_term_detach_init(&detach, g_detach_keys, g_composited ? 0 : g_detach_key_count);
    if (g_line_mode) hold_term_detach_set_interrupt(&detach, 0x03); /* Ctrl-C */
    enum hold_client_end end = HOLD_CLIENT_OUTPUT_ENDED;
    if (hold_write_all(writer, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        if (errno == EPIPE || errno == ECONNRESET) g_gone = true; /* ended as we arrived */
        rc = 3;
    } else if (g_stream && !interactive && write_frame(writer, CONSOLE_FRAME_STREAM, NULL, 0) != 0) {
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
            .ctx = &a,
        };
        end = hold_client_run(&c);
        if (hold_term_stop_signal()) end = HOLD_CLIENT_SIGNALED; /* even if it broke a blocked write */
        if (end == HOLD_CLIENT_SIGNALED && g_forward_term && hold_term_stop_signal() == SIGTERM && a.writer >= 0) {
            /* A launch told to stop (`timeout 8 callbox foo`) passes SIGTERM on
             * to its program, as docker run's sig-proxy does: the program was
             * run here, and must not be left behind unseen. (SIGHUP - the
             * terminal went away - detaches: callbox keeps programs running.) */
            unsigned char sig = SIGTERM;
            (void)write_frame(a.writer, CONSOLE_FRAME_INTERRUPT, &sig, 1);
        }
        if (end == HOLD_CLIENT_SIGNALED) rc = 128 + hold_term_stop_signal();
        else if (end == HOLD_CLIENT_ERROR) rc = 3;
        if (a.resize_failed) g_gone = true;
    }

    /* The connection closing just after the detach keys is the detach. */
    if (a.detach_sent_ms && attach_now_ms() - a.detach_sent_ms < 2000) rc = 0;
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
    if (saved.saved && !g_line_mode) {
        /* A program run inside the console may have left terminal-global modes
         * enabled — mouse reporting, bracketed paste, focus reports, application
         * keypad/cursor keys, a cursor style, a hidden cursor — and a callbox
         * shell's display sets them on its behalf (and may be cut off inside a
         * synchronized update). These survive the PTY teardown and would leak
         * back to the user's shell, so reset them before handing the terminal
         * back. */
        static const char reset_modes[] =
            "\033[?2026l\033[0m"
            "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l"
            "\033[?2004l\033[?1004l\033[?1l\033>\033[0 q\033[?25h";
        (void)hold_write_all(STDOUT_FILENO, reset_modes, sizeof(reset_modes) - 1);
    }
    if (alt_screen) {
        (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
    }
    if (saved.saved) {
        if (!g_line_mode) (void)hold_write_all(STDOUT_FILENO, "\033[?25h", 6);
        hold_term_input_restore(&saved);
    }
    g_heard = a.heard;
    if (a.writer >= 0 && a.writer != sock) close(a.writer);
    close(sock);
    if (pipe_ignored) sigaction(SIGPIPE, &old_pipe, NULL);
    if (end == HOLD_CLIENT_SIGNALED) hold_term_reraise_stop();
    return rc;
}
