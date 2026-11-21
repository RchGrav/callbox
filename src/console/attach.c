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
static bool g_refused, g_gone, g_composited;
static pid_t g_refused_by;

bool hold_console_was_refused(pid_t *by) {
    if (by) *by = g_refused_by;
    return g_refused;
}

bool hold_console_was_gone(void) {
    return g_gone;
}

size_t hold_console_get_detach_keys(unsigned char *out, size_t cap) {
    memcpy(out, g_detach_keys, g_detach_key_count < cap ? g_detach_key_count : cap);
    return g_detach_key_count;
}

void hold_console_set_composited(bool on) {
    g_composited = on;
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

static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int signo) {
    (void)signo;
    g_resized = 1;
}

/* SIGTERM or SIGHUP: leave the loop, give the terminal back as it was, then
 * die of the signal as the default action would have. */
static volatile sig_atomic_t g_stop_sig = 0;

static void handle_stop(int signo) {
    g_stop_sig = signo;
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

/* Client-side wire: 3-byte type + be16-length frames after the attach magic. */
static int write_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char header[CONSOLE_FRAME_HEADER_LEN];
    header[0] = type;
    console_store_be16(header + 1, len);
    if (hold_write_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && hold_write_all(fd, payload, len) != 0) {
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

static int terminal_size(struct winsize *ws) {
    memset(ws, 0, sizeof(*ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    return -1;
}

static bool install_handler(int sig, void (*fn)(int), struct sigaction *old) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, old) == 0;
}

/* Bytes the detach FSM releases go to the broker as DATA frames. */
static int attach_data_sink(void *ctx, const unsigned char *bytes, size_t n) {
    return write_frame(*(const int *)ctx, CONSOLE_FRAME_DATA, bytes, (uint16_t)n);
}

int hold_run_native_console(const char *sock_path) {
    int writer = -1;
    g_refused = g_gone = false;
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

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool terminal_saved = false;
    bool alt_screen = false;
    struct termios old_termios;
    if (interactive && tcgetattr(STDIN_FILENO, &old_termios) == 0) {
        struct termios raw = old_termios;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            terminal_saved = true;
            if (hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
                alt_screen = true;
            }
        }
    }

    struct sigaction old_winch, old_pipe;
    bool have_old_winch = install_handler(SIGWINCH, handle_sigwinch, &old_winch);
    bool have_old_pipe = install_handler(SIGPIPE, SIG_IGN, &old_pipe);
    struct sigaction old_term, old_hup;
    g_stop_sig = 0;
    bool have_old_term = install_handler(SIGTERM, handle_stop, &old_term);
    bool have_old_hup = install_handler(SIGHUP, handle_stop, &old_hup);
    /* A signal the caller ignores (nohup) stays ignored. */
    if (have_old_term && old_term.sa_handler == SIG_IGN) sigaction(SIGTERM, &old_term, NULL);
    if (have_old_hup && old_hup.sa_handler == SIG_IGN) sigaction(SIGHUP, &old_hup, NULL);
    g_resized = 1;

    int rc = 0;
    bool stdin_open = true;
    /* When the detach keys were last sent on. The broker answers by closing
     * the connection - or, when an attach nested in this terminal is what the
     * person is looking at, by handing the keys to that one and keeping us. */
    int64_t detach_sent_ms = 0;
    /* A broker that turns us away says so in a line of text and hangs up.
     * Text that starts like that line is held back until it is clear which
     * it is: written to the terminal it would be wiped with the alternate
     * screen, and the person would see an attach that silently returns. */
    char held[160];
    size_t held_len = 0;
    bool holding = true;
    int nest_writer = -1;
    int nest = interactive ? register_nested(sock_path, &nest_writer) : -1;
    /* macOS poll() never reports end-of-file on a FIFO, so over the FIFO
     * console (a separate write fd) a quiet pass probes with a non-blocking
     * read: 0 means the broker has gone. */
#if defined(__APPLE__)
    bool probe_fifo = writer != sock;
#else
    bool probe_fifo = false;
#endif
    struct hold_term_detach detach;
    hold_term_detach_init(&detach, g_detach_keys, g_composited ? 0 : g_detach_key_count);
    if (hold_write_all(writer, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        if (errno == EPIPE || errno == ECONNRESET) g_gone = true; /* ended as we arrived */
        rc = 3;
        goto out;
    }
    bool heard = false; /* anything from the broker yet */

    while (1) {
        if (g_stop_sig) {
            rc = 128 + g_stop_sig;
            break;
        }
        if (g_resized) {
            struct winsize ws;
            g_resized = 0;
            if (writer >= 0 && terminal_size(&ws) == 0 && send_resize(writer, &ws) != 0) {
                if (!heard && (errno == EPIPE || errno == ECONNRESET)) g_gone = true;
                rc = 3;
                break;
            }
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = sock, .events = POLLIN};
        int stdin_idx = -1;
        if (stdin_open) {
            stdin_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        }

        int sr = poll(pfds, nfds, probe_fifo ? 250 : -1);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 3;
            break;
        }

        /* POLLNVAL too: macOS poll() rejects character devices such as
         * /dev/null; reading one then reports its end, instead of spinning. */
        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (interactive) {
                    for (ssize_t i = 0; i < n; i++) {
                        int completed = 0;
                        if (hold_term_detach_feed(&detach, buf[i], attach_data_sink, &writer, &completed) != 0) {
                            rc = 3;
                            break;
                        }
                        if (completed == 1) {
                            if (write_frame(writer, CONSOLE_FRAME_DETACH, NULL, 0) != 0) {
                                rc = 3;
                                break;
                            }
                            detach_sent_ms = attach_now_ms();
                        }
                    }
                    if (rc != 0) {
                        break;
                    }
                } else if (write_frame(writer, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0 || errno == EBADF) { /* end of input, or none at all (stdin closed) */
                if (hold_term_detach_flush(&detach, attach_data_sink, &writer) != 0) {
                    rc = 3;
                    break;
                }
                stdin_open = false;
                if (writer == sock) shutdown(sock, SHUT_WR);
                else {
                    close(writer);
                    writer = -1;
                }
            } else if (errno != EINTR) {
                rc = 3;
                break;
            }
        }

        bool probe = probe_fifo && !(pfds[0].revents & (POLLIN | POLLHUP | POLLERR));
        if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) || probe) {
            char buf[4096];
            int flags = probe ? fcntl(sock, F_GETFL) : -1;
            if (flags >= 0) (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
            ssize_t n = read(sock, buf, sizeof(buf));
            int read_errno = errno;
            if (flags >= 0) (void)fcntl(sock, F_SETFL, flags);
            errno = read_errno;
            if (n > 0) {
                heard = true;
                const char *out = buf;
                size_t out_len = (size_t)n;
                if (holding) {
                    static const char lead[] = "callbox: ";
                    size_t room = sizeof(held) - held_len, take = out_len < room ? out_len : room;
                    memcpy(held + held_len, out, take);
                    held_len += take;
                    size_t cmp = held_len < sizeof(lead) - 1 ? held_len : sizeof(lead) - 1;
                    if (take == out_len && memcmp(held, lead, cmp) == 0) continue; /* may be a refusal: wait */
                    /* a screen after all: what was held, then the rest */
                    holding = false;
                    if (hold_write_all(STDOUT_FILENO, held, held_len) != 0) {
                        rc = 3;
                        break;
                    }
                    out += take;
                    out_len -= take;
                }
                if (out_len > 0 && hold_write_all(STDOUT_FILENO, out, out_len) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                rc = 3;
                break;
            }
        }
    }

out:
    /* The connection closing just after the detach keys is the detach. */
    if (detach_sent_ms && attach_now_ms() - detach_sent_ms < 2000) rc = 0;
    if (holding && held_len > 0) {
        if (parse_refusal(held, held_len, &g_refused_by)) {
            g_refused = true;
            rc = 3;
        } else {
            (void)hold_write_all(STDOUT_FILENO, held, held_len); /* a program's own (short) output */
        }
    }
    if (nest >= 0) {
        if (nest_writer != nest) close(nest_writer);
        close(nest);
    }
    if (have_old_winch) {
        sigaction(SIGWINCH, &old_winch, NULL);
    }
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    if (terminal_saved) {
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
    if (terminal_saved) {
        (void)hold_write_all(STDOUT_FILENO, "\033[?25h", 6);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    if (writer >= 0 && writer != sock) close(writer);
    close(sock);
    if (have_old_term) sigaction(SIGTERM, &old_term, NULL);
    if (have_old_hup) sigaction(SIGHUP, &old_hup, NULL);
    if (g_stop_sig) {
        /* The terminal is restored; now end as the signal meant us to. */
        int sig = g_stop_sig;
        signal(sig, SIG_DFL);
        raise(sig);
    }
    return rc;
}
