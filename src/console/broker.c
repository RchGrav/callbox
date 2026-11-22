#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/console.h"
#include "callbox/core.h"
#include "callbox/store.h"
#include "callbox/platform.h"
#include "callbox/term.h"
#include "callbox/screen.h"
#include "callbox/console_internal.h"
#include "overlay.h"

#include <regex.h>

/* The console broker: owns a call's PTY master, captures every output byte to
 * the indexed log, and keeps a headless terminal (the screen layer) in step
 * with it. Clients reach it over the call's private Unix socket:
 *
 *   - one attach client at a time (callbox attach / callbox -it): framed or raw
 *     terminal bridge, repainted from the live screen when it connects;
 *   - any number of control clients (callbox peek / send / wait / resize): one
 *     length-prefixed request, one length-prefixed reply, then close.
 *
 * When the target exits, the final screen is written beside the log so the
 * last frame stays inspectable after the socket is gone.
 *
 * A callbox shell (options->chat) differs in two ways, both confined to the
 * `b->chat` branches below: its attach client is sent composed frames (the
 * screen with the chat window over it, see overlay.h) instead of the PTY's
 * raw bytes, and a message for it goes to the chat inbox instead of being
 * typed. Every other call runs the paths it always did. */

#define BROKER_MAX_CONTROL 64
#define BROKER_WRITE_TIMEOUT_MS 5000
/* A connection silent this long is a read-only raw attach (e.g. socat -u). */
#define BROKER_SNIFF_MS 250
/* A control request that has sent nothing for this long is a stalled
 * client, and its slot is freed (a large one still arriving is not). */
#define BROKER_READ_MS 10000
#define BROKER_MAX_MSGS 16
/* callbox msg loop guard: at most this many deliveries per receiver per minute. */
#define BROKER_MSG_RATE 20
/* A sender that messages the instant it starts may beat its call's record. */
#define BROKER_MSG_VERIFY_GRACE_MS 2000
/* A callbox shell's display is redrawn at most this often: output that arrives
 * faster is coalesced into the next frame. */
#define BROKER_FRAME_MS 12

/* The child-mode broker's forked target group, set right after the pid is
 * reported and cleared at every reap site. The SIGTERM forwarder reads it so a
 * TERM aimed at the broker (e.g. a raw kill of the broker pid) reaches the held
 * group instead of orphaning it. */
static volatile pid_t g_broker_forward_target = 0;

static void broker_forward_term(int signo) {
    (void)signo;
    pid_t t = g_broker_forward_target;
    if (t > 1) {
        kill(-t, SIGTERM);
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- broker context: every fd/path the broker owns, one cleanup path ---- */

struct broker {
    const struct hold_store *store;
    const char *run_id;
    const char *log_path;
    const char *sock_path; /* NULL/"" = nothing to unlink */
    int parent_pipe;
    int listener;
    int fifo_writer;
    /* Where the program is: on a PTY, in and out are the master; a program on
     * pipes has its stdin's write end and its output's read end. */
    struct program_endpoint {
        int in_fd;  /* the program's input; -1 once closed (end of input) */
        int out_fd; /* its output, pumped to the log, the screen, and the client */
        bool pty;
    } ep;
    int logfd;
    pid_t target; /* >0: our held child (waited, SIGKILLed on cleanup) */
    pid_t program_pid; /* the program's pid, kept after it is reaped: whose exit to stamp */
    bool eof_echo;       /* a typed end of input's echo is still to be taken out */
    size_t eof_echo_got; /* how much of it the output has matched so far */
    uid_t owner_uid;
    bool have_allowed_peer_uid;
    uid_t allowed_peer_uid;
    struct hold_screen *screen;
    uint64_t output_seq;
    int64_t last_output_ms;
    bool size_by_agent;
    bool have_mouse;
    uint16_t mouse_row, mouse_col;
    bool target_done;
    int target_status;
    unsigned char *last_alt; /* HSNAP1 of the last full-screen frame left */
    size_t last_alt_len;
    uint64_t next_gen;
    struct pending_msg {
        bool used;
        char *text; /* the body; the header is built at delivery */
        size_t len;
        bool submit;
        bool verified;
        bool has_claim;
        char claim[ALIAS_MAX_LEN + ID_STR_LEN + 1]; /* the sender's claimed call */
        pid_t peer;                                 /* the sending process */
        int64_t start_ms;
        struct control_client *waiter; /* reply here if still the same client */
        uint64_t waiter_gen;
    } msgs[BROKER_MAX_MSGS];
    int64_t sent_ms[BROKER_MSG_RATE];
    int sent_idx;
    uint32_t msg_count; /* feeds each message's id */
    struct chat *chat;  /* a callbox shell's chat and compositor; NULL for every other call */
    off_t log_start;    /* where this run's output starts in the log */
    /* A program on pipes: input waiting for room in its stdin pipe. The
     * broker never blocks on that pipe - its one loop also drains the
     * program's output, and a program that prints more than it reads (or
     * never reads) would otherwise lock both up. */
    unsigned char *inq;
    size_t inq_len, inq_cap;
    bool inq_eof; /* end of input asked for: close stdin once the queue is out */
    bool replayed;      /* a program on pipes: its first client has had the replay */
    int64_t frame_ms;   /* when its attach client was last sent a frame */
    struct control_client *ctl; /* the control slots (BROKER_MAX_CONTROL of them) */
};

static void broker_cleanup_and_exit(struct broker *b, int exit_code) {
    if (b->target > 0) {
        int st = 0;
        kill(b->target, SIGKILL);
        while (waitpid(b->target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (b->parent_pipe >= 0) close(b->parent_pipe);
    if (b->fifo_writer >= 0) close(b->fifo_writer);
    if (b->listener >= 0) close(b->listener);
    if (b->ep.in_fd >= 0) close(b->ep.in_fd);
    if (b->ep.out_fd >= 0 && b->ep.out_fd != b->ep.in_fd) close(b->ep.out_fd);
    free(b->inq);
    if (b->logfd >= 0) close(b->logfd);
    if (b->sock_path && *b->sock_path) {
        unlink(b->sock_path);
        hold_remove_fifo_exchanges(b->sock_path); /* clients that died mid-handshake */
    }
    chat_free(b->chat);
    hold_screen_free(b->screen);
    free(b->last_alt);
    _exit(exit_code);
}

static void broker_fail_errno(struct broker *b, int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)hold_write_all(b->parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(b, 127);
}

static int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

/* Client sockets are nonblocking; a reader that falls behind gets a bounded
 * wait rather than an instant drop. */
static int write_all_wait(int fd, const void *buf, size_t n) {
    const char *p = buf;
    int64_t deadline = now_ms() + BROKER_WRITE_TIMEOUT_MS;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int64_t left = deadline - now_ms();
            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            (void)poll(&pfd, 1, (int)left);
            continue;
        }
        if (w == 0) errno = EIO;
        return -1;
    }
    return 0;
}

/* Who may use the console: root, the call's owner, or the user a sudo
 * launch names. The socket learns the uid from peer credentials, the FIFO
 * console from who owns the client's private FIFOs. */
static bool uid_allowed(const struct broker *b, uid_t uid) {
    return uid == 0 || uid == b->owner_uid || (b->have_allowed_peer_uid && uid == b->allowed_peer_uid);
}

static bool authorize_client(const struct broker *b, int client) {
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(client, &peer_uid) == 0 && uid_allowed(b, peer_uid)) {
        return true;
    }
    static const char msg[] = "callbox: console attach denied\n";
    (void)hold_write_all(client, msg, sizeof(msg) - 1);
    return false;
}

/* The program's input has ended (EOF asked for, or it closed its stdin):
 * nothing more goes in. */
static void close_program_input(struct broker *b) {
    if (b->ep.pty || b->ep.in_fd < 0) return;
    close(b->ep.in_fd);
    b->ep.in_fd = -1;
    b->inq_len = 0;
    b->inq_eof = false;
}

/* Push queued input into a program's stdin pipe, as far as it will take. */
static void flush_program_input(struct broker *b) {
    while (b->inq_len > 0 && b->ep.in_fd >= 0) {
        ssize_t w = write(b->ep.in_fd, b->inq, b->inq_len);
        if (w > 0) {
            memmove(b->inq, b->inq + w, b->inq_len - (size_t)w);
            b->inq_len -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) return;
        /* EPIPE (it closed its stdin, or ended) and anything else: its input
         * is over - which is the program's business, not the client's. */
        close_program_input(b);
        return;
    }
    if (b->inq_len == 0 && b->inq_eof) close_program_input(b);
}

/* THE way input reaches the program. On a PTY, written as it always was. On
 * pipes, queued and pushed without ever blocking the broker. */
static int program_write(struct broker *b, const void *buf, size_t n) {
    if (b->ep.in_fd < 0 || n == 0) return 0;
    if (b->ep.pty) return hold_write_all(b->ep.in_fd, buf, n);
    if (b->inq_len + n > b->inq_cap) {
        size_t cap = b->inq_cap ? b->inq_cap : 16384;
        while (cap < b->inq_len + n) cap *= 2;
        unsigned char *p = realloc(b->inq, cap);
        if (!p) return 0; /* out of memory: this input is lost, the call goes on */
        b->inq = p;
        b->inq_cap = cap;
    }
    memcpy(b->inq + b->inq_len, buf, n);
    b->inq_len += n;
    flush_program_input(b);
    return 0;
}

/* End of input for a program on a PTY: the terminal's own end-of-input
 * character, so a program reading stdin finishes as it would on /dev/null.
 * macOS echoes it into the program's output as "^D" and two backspaces
 * (Linux never echoes VEOF); a stream's reader should get the program's
 * output only, so that echo is taken out of the next output (see
 * strip_eof_echo). Not by turning ECHO off around the byte: on macOS,
 * changing the terminal's modes from the master while input waits loses
 * that input (cat then read only the end). */
static const char EOF_ECHO[] = "^D\b\b";

static void program_send_eof(struct broker *b) {
    if (b->ep.in_fd < 0) return;
    struct termios t;
    bool have = tcgetattr(b->ep.in_fd, &t) == 0;
    unsigned char eof = have && t.c_cc[VEOF] != _POSIX_VDISABLE ? t.c_cc[VEOF] : 0x04;
#ifdef __APPLE__
    /* BSD echoes as it takes the byte, before the write returns: the echo
     * is in the output queue, after whatever the program wrote before. */
    if (have && eof == 0x04 && (t.c_lflag & ICANON) && (t.c_lflag & ECHO) && (t.c_lflag & ECHOCTL)) {
        b->eof_echo = true;
        b->eof_echo_got = 0;
    }
#endif
    (void)hold_write_all(b->ep.in_fd, &eof, 1);
}

/* Take the first "^D\b\b" out of the program's output after an end of input
 * was typed (it may arrive split across reads); out has room for n + 4. */
static size_t strip_eof_echo(void *ctx, const char *in, size_t n, char *out) {
    struct broker *b = ctx;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (!b->eof_echo) {
            out[o++] = in[i];
            continue;
        }
        if (in[i] == EOF_ECHO[b->eof_echo_got]) {
            if (++b->eof_echo_got == sizeof(EOF_ECHO) - 1) b->eof_echo = false;
            continue;
        }
        /* Not the echo after all: give back what was held, look again here. */
        memcpy(out + o, EOF_ECHO, b->eof_echo_got);
        o += b->eof_echo_got;
        b->eof_echo_got = 0;
        if (in[i] == EOF_ECHO[0]) b->eof_echo_got = 1;
        else out[o++] = in[i];
    }
    return o;
}

/* While this much input waits for a program that is not reading, the
 * attached client is not read either: its sender waits, the broker never. */
#define BROKER_INQ_HIGH (64 * 1024)

/* Everything the terminal wants sent to the program (query replies, encoded
 * input) goes to the master. While a human terminal is attached it answers
 * the program's queries itself, so the screen's replies are dropped then. */
static void flush_screen_output(struct broker *b) {
    char buf[4096];
    size_t n;
    while ((n = hold_screen_take_output(b->screen, buf, sizeof(buf))) > 0) {
        /* A key's Enter is a carriage return, which a PTY turns into a
         * newline; a program on pipes needs the newline itself. */
        if (!b->ep.pty)
            for (size_t i = 0; i < n; i++)
                if (buf[i] == '\r') buf[i] = '\n';
        (void)program_write(b, buf, n);
    }
}

static void apply_size(struct broker *b, int rows, int cols, bool by_agent) {
    if (!hold_term_size_valid(cols, rows)) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    /* The kernel delivers SIGWINCH to the PTY's foreground group itself. A
     * program on pipes has no window: only the headless screen changes. */
    if (b->ep.pty) (void)ioctl(b->ep.in_fd, TIOCSWINSZ, &ws);
    hold_screen_resize(b->screen, rows, cols);
    b->size_by_agent = by_agent;
    if (b->chat) {
        /* A size from the attached terminal is that terminal's window; a
         * callbox resize changes only the shell's PTY, shown clipped or padded. */
        if (!by_agent) chat_terminal_resized(b->chat, rows, cols);
        chat_screen_changed(b->chat);
    }
    /* The last mouse event's cell may not exist at the new size (or now
     * shows something else): stop reporting it. */
    b->have_mouse = false;
}

static int exit_code_of(int st) {
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
}

static int take_snapshot(const struct broker *b, struct hold_snapshot *snap) {
    if (hold_screen_snapshot(b->screen, snap) != 0) return -1;
    snap->meta.output_seq = b->output_seq;
    int64_t idle = now_ms() - b->last_output_ms;
    snap->meta.idle_ms = idle < 0 ? 0 : idle > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)idle;
    snap->meta.running = !b->target_done;
    snap->meta.size_by_agent = b->size_by_agent;
    snap->meta.has_mouse = b->have_mouse;
    snap->meta.mouse_row = b->mouse_row;
    snap->meta.mouse_col = b->mouse_col;
    if (b->target_done) {
        snap->meta.has_exit_code = true;
        snap->meta.exit_code = exit_code_of(b->target_status);
    }
    return 0;
}

/* ---- the attach client: magic sniff, then 3-byte frames or raw ---------- */

struct attach_client {
    int output;
    int fd;            /* -1 = no client attached */
    bool input_closed; /* its input ended; it may still be reading (printf ... | callbox attach) */
    pid_t peer_pid;    /* 0 = unknown */
    bool decided;
    bool framed;
    bool needs_repaint;
    bool stream;       /* not a terminal: output as a stream, never a repaint */
    bool no_terminal;  /* nothing on the client's side answers terminal queries */
    unsigned char pending[16384];
    size_t pending_len;
};

static bool forward_detach(struct broker *b);

static void drop_attach(struct broker *b, struct attach_client *c) {
    if (c->fd >= 0) {
        if (c->output != c->fd) close(c->output);
        close(c->fd);
        if (b->chat) chat_detached(b->chat);
        /* Nobody is looking at this terminal any more: an attach client
         * running in it would hold its own call's console unseen, with no
         * way for its keys to reach it. It lets go too. */
        (void)forward_detach(b);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/* What the attached terminal sent: the program's input - except in a callbox
 * shell, where chat decides whether it is the shell's or its own. */
static int attach_input(struct broker *b, const void *buf, size_t n) {
    if (b->chat) {
        chat_input(b->chat, buf, n, now_ms());
        /* The lead key, then the detach key: as a detach frame from a client
         * that reads its own keys - the innermost attach lets go first. */
        return chat_take_detach(b->chat) && !forward_detach(b) ? 1 : 0;
    }
    return program_write(b, buf, n);
}

static int process_framed(struct broker *b, struct attach_client *c) {
    while (c->pending_len >= CONSOLE_FRAME_HEADER_LEN) {
        unsigned char type = c->pending[0];
        uint16_t len = console_load_be16(c->pending + 1);
        size_t frame_len = CONSOLE_FRAME_HEADER_LEN + (size_t)len;
        if (c->pending_len < frame_len) {
            return 0;
        }
        const unsigned char *payload = c->pending + CONSOLE_FRAME_HEADER_LEN;
        if (type == CONSOLE_FRAME_DATA) {
            if (len > 0 && attach_input(b, payload, len) != 0) {
                return -1;
            }
        } else if (type == CONSOLE_FRAME_RESIZE) {
            if (len == 4) apply_size(b, console_load_be16(payload), console_load_be16(payload + 2), false);
        } else if (type == CONSOLE_FRAME_STREAM) {
            c->stream = true;
        } else if (type == CONSOLE_FRAME_NO_TERMINAL) {
            c->no_terminal = true;
        } else if (type == CONSOLE_FRAME_EOF) {
            if (b->ep.pty) {
                program_send_eof(b);
            } else if (b->ep.in_fd >= 0) {
                b->inq_eof = true; /* once what is queued has gone in */
                flush_program_input(b);
            }
        } else if (type == CONSOLE_FRAME_INTERRUPT) {
            /* Empty: Ctrl-C for a program on pipes. One byte: a signal a
             * launch passes on (SIGTERM), for either kind of program. */
            int sig = len == 1 ? payload[0] : SIGINT;
            bool ok = len == 1 ? (sig == SIGTERM || sig == SIGINT) : !b->ep.pty;
            if (ok && !b->target_done && b->target > 0) (void)kill(-b->target, sig);
        } else if (type == CONSOLE_FRAME_DETACH) {
            /* The detach keys let go of the innermost attach first: with an
             * attach client at the front of this terminal they are its, and
             * ours stays. */
            if (!forward_detach(b)) return 1;
        }
        memmove(c->pending, c->pending + frame_len, c->pending_len - frame_len);
        c->pending_len -= frame_len;
    }
    return 0;
}

/* Until CONSOLE_ATTACH_MAGIC_LEN bytes decide the mode, input is buffered; a
 * magic prefix selects framed mode, anything else replays as raw passthrough.
 * Returns nonzero to drop the client (1 = clean detach frame). */
static int process_attach_input(struct broker *b, struct attach_client *c, const unsigned char *buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (c->decided && !c->framed) {
        return attach_input(b, buf, n);
    }
    if (c->pending_len + n > sizeof(c->pending)) {
        if (c->decided) {
            errno = EOVERFLOW;
            return -1;
        }
        c->decided = true;
        if (c->pending_len > 0 && attach_input(b, c->pending, c->pending_len) != 0) {
            return -1;
        }
        c->pending_len = 0;
        return attach_input(b, buf, n);
    }
    memcpy(c->pending + c->pending_len, buf, n);
    c->pending_len += n;
    if (!c->decided) {
        size_t cmp = c->pending_len < CONSOLE_ATTACH_MAGIC_LEN ? c->pending_len : CONSOLE_ATTACH_MAGIC_LEN;
        if (memcmp(c->pending, CONSOLE_ATTACH_MAGIC, cmp) != 0) {
            c->decided = true;
            if (attach_input(b, c->pending, c->pending_len) != 0) {
                return -1;
            }
            c->pending_len = 0;
            return 0;
        }
        if (c->pending_len < CONSOLE_ATTACH_MAGIC_LEN) {
            return 0;
        }
        c->decided = true;
        c->framed = true;
        c->pending_len -= CONSOLE_ATTACH_MAGIC_LEN;
        memmove(c->pending, c->pending + CONSOLE_ATTACH_MAGIC_LEN, c->pending_len);
    }
    return process_framed(b, c);
}

/* A callbox shell's display: one composed frame (what changed since the last)
 * to the attach client. Only a failed write is an error; a frame that could
 * not be built is simply tried again. */
static int send_frame(struct broker *b, struct attach_client *c, int64_t now) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc = 0;
    b->frame_ms = now;
    if (chat_render(b->chat, now, &buf, &len, &cap) == 0 && len > 0) rc = write_all_wait(c->output, buf, len);
    free(buf);
    return rc;
}

/* A new attach client sees the live screen, not a byte replay. */
/* A program on pipes has no screen to repaint for a client that keeps its
 * terminal in line mode: the first client (the launcher, attaching as its
 * program starts) gets this run's output so far from the log, so nothing
 * printed before it connected is lost; a client attaching later gets what
 * comes next, as docker attach does. */
static int replay_attach(struct broker *b, struct attach_client *c) {
    if (b->replayed) return 0;
    b->replayed = true;
    int fd = open(b->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    if (lseek(fd, b->log_start, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }
    char buf[4096];
    ssize_t n;
    int rc = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write_all_wait(c->output, buf, (size_t)n) != 0) {
            rc = -1;
            break;
        }
    close(fd);
    return rc;
}

static int repaint_attach(struct broker *b, struct attach_client *c) {
    if (!b->ep.pty || c->stream) return replay_attach(b, c);
    if (b->chat) {
        int rows = 0, cols = 0;
        int64_t now = now_ms();
        hold_screen_size(b->screen, &rows, &cols);
        chat_attached(b->chat, rows, cols, now);
        return send_frame(b, c, now);
    }
    struct hold_snapshot snap;
    if (take_snapshot(b, &snap) != 0) return -1;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc = hold_snapshot_repaint(&snap, &buf, &len, &cap);
    hold_snapshot_free(&snap);
    if (rc == 0) rc = write_all_wait(c->output, buf, len);
    free(buf);
    return rc;
}

/* ---- control clients: one request, one reply ---------------------------- */

enum control_phase { CTL_SNIFF = 0, CTL_READING, CTL_WAITING, CTL_MSG, CTL_NESTED };

struct wait_request {
    int64_t start_ms;
    int64_t deadline_ms; /* 0 = no timeout */
    uint32_t idle_ms;    /* 0 = no idle condition */
    bool has_since;
    uint64_t since;
    bool want_exit;
    bool has_match;
    bool match_regex;
    char *match;         /* literal text, or the regex source */
    regex_t re;
    bool re_ok;
};

struct control_client {
    int output;
    int fd; /* -1 = free slot */
    enum control_phase phase;
    int64_t accepted_ms;
    int64_t active_ms; /* CTL_READING: when its last bytes arrived */
    uint64_t gen;   /* distinguishes reuses of this slot */
    pid_t peer_pid; /* 0 = unknown */
    unsigned char *buf;
    size_t len, cap;
    struct wait_request wait;
    unsigned char nest_keys[HOLD_TERM_DETACH_MAX_KEYS]; /* CTL_NESTED: that client's detach keys */
    size_t nest_nkeys;
};

static void drop_control(struct control_client *c) {
    if (c->fd >= 0) {
        if (c->output != c->fd) close(c->output);
        close(c->fd);
    }
    free(c->buf);
    free(c->wait.match);
    if (c->wait.re_ok) regfree(&c->wait.re);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/* Reply = be32 length + (header text [+ binary body]); the client is then closed. */
static void control_reply(struct control_client *c, const char *header, const void *body, size_t body_len) {
    size_t hl = strlen(header);
    unsigned char len[4];
    console_store_be32(len, (uint32_t)(hl + body_len));
    if (write_all_wait(c->output, len, 4) == 0 && write_all_wait(c->output, header, hl) == 0 && body_len > 0)
        (void)write_all_wait(c->output, body, body_len);
    drop_control(c);
}

static void control_replyf(struct control_client *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    control_reply(c, msg, NULL, 0);
}

static int hex_value(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Decode hex in place; returns the decoded length or -1. */
static ssize_t hex_decode(char *s) {
    size_t n = strlen(s);
    if (n % 2) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_value((unsigned char)s[i]), lo = hex_value((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        s[i / 2] = (char)(hi << 4 | lo);
    }
    s[n / 2] = '\0';
    return (ssize_t)(n / 2);
}

/* "key=value" lookup in a space-separated argument line. */
static const char *kv_get(const char *line, const char *key) {
    size_t kl = strlen(key);
    for (const char *tok = line; tok && *tok;) {
        const char *sp = strchr(tok, ' ');
        if (!strncmp(tok, key, kl) && tok[kl] == '=') {
            return tok + kl + 1;
        }
        tok = sp ? sp + 1 : NULL;
    }
    return NULL;
}

/* "nested keys=<hex>": an attach client running in this call's terminal (it
 * must be in our program's session) is attached to some other call. The
 * connection is kept, unanswered, until that client goes. */
static void handle_nested(struct broker *b, struct control_client *c, const char *args) {
    const char *v = kv_get(args, "keys");
    size_t n = 0;
    for (; v && n < sizeof(c->nest_keys) && hex_value((unsigned char)v[2 * n]) >= 0 &&
           hex_value((unsigned char)v[2 * n + 1]) >= 0; n++)
        c->nest_keys[n] = (unsigned char)(hex_value((unsigned char)v[2 * n]) << 4 | hex_value((unsigned char)v[2 * n + 1]));
    pid_t ours = b->target > 0 ? getsid(b->target) : -1;
    if (n == 0 || c->peer_pid <= 0 || ours <= 0 || getsid(c->peer_pid) != ours) {
        drop_control(c);
        return;
    }
    c->nest_nkeys = n;
    c->phase = CTL_NESTED;
}

/* An attach client nested in this terminal, and at the front of it (in the
 * PTY's foreground process group, so what is typed is its to read): type
 * its detach keys, and it detaches from the call it is attached to. False
 * when there is none. */
static bool forward_detach(struct broker *b) {
    /* Nesting needs a terminal to be nested in: only a PTY has a front. */
    if (!b->ctl || b->target_done || !b->ep.pty || b->ep.in_fd < 0) return false;
    pid_t front = tcgetpgrp(b->ep.in_fd);
    struct control_client *pick = NULL;
    for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
        struct control_client *c = &b->ctl[i];
        if (c->fd < 0 || c->phase != CTL_NESTED) continue;
        /* (where the PTY will not say who is in front, the newest one is) */
        if (front > 0 ? getpgid(c->peer_pid) == front : (!pick || c->gen > pick->gen)) pick = c;
    }
    if (!pick) return false;
    (void)hold_write_all(b->ep.in_fd, pick->nest_keys, pick->nest_nkeys);
    return true;
}

static void handle_peek(struct broker *b, struct control_client *c, const char *args) {
    const char *alt = strstr(args, "alt=1");
    if (alt) {
        if (!b->last_alt) {
            control_replyf(c, "error the program has not left a full-screen (alternate) screen yet\n");
            return;
        }
        control_reply(c, "ok\n", b->last_alt, b->last_alt_len);
        return;
    }
    struct hold_snapshot snap;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    if (take_snapshot(b, &snap) != 0) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    int rc = hold_snapshot_serialize(&snap, &blob, &blob_len);
    hold_snapshot_free(&snap);
    if (rc != 0) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    control_reply(c, "ok\n", blob, blob_len);
    free(blob);
}

static void handle_send(struct broker *b, struct control_client *c, char *ops) {
    uint64_t seq = b->output_seq;
    bool mouse_off = false, mouse_clamped = false;
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    for (char *line = ops; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = '\0';
        if (!strcmp(line, "text") || !strcmp(line, "paste")) {
            ssize_t n = arg ? hex_decode(arg) : -1;
            if (n < 0) {
                control_replyf(c, "error malformed %s\n", line);
                return;
            }
            if (line[0] == 'p') {
                hold_screen_send_paste(b->screen, arg, (size_t)n);
                flush_screen_output(b);
            } else if (n > 0 && b->ep.in_fd >= 0) {
                (void)program_write(b, arg, (size_t)n);
            }
        } else if (!strcmp(line, "key")) {
            if (!arg || hold_screen_send_key(b->screen, arg) != 0) {
                control_replyf(c, "error unknown key '%s'\n", arg ? arg : "");
                return;
            }
            flush_screen_output(b);
        } else if (!strcmp(line, "mouse")) {
            char action[32];
            int button = 1, row = 0, col = 0;
            if (!arg || sscanf(arg, "%31s %d %d %d", action, &button, &row, &col) != 4) {
                control_replyf(c, "error malformed mouse\n");
                return;
            }
            int mr = hold_screen_send_mouse(b->screen, action, button, row, col);
            if (mr < 0) {
                control_replyf(c, "error unknown mouse action '%s'\n", action);
                return;
            }
            if (mr == 1) mouse_off = true;
            if (mr == 2) mouse_clamped = true;
            b->have_mouse = true;
            b->mouse_row = (uint16_t)(row < 0 ? 0 : row);
            b->mouse_col = (uint16_t)(col < 0 ? 0 : col);
            flush_screen_output(b);
        } else if (*line) {
            control_replyf(c, "error unknown send op '%s'\n", line);
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
    control_replyf(c, "ok seq=%" PRIu64 "%s%s\n", seq, mouse_off ? " mouse=off" : "",
                   mouse_clamped ? " mouse=clamped" : "");
}

static void handle_resize(struct broker *b, struct control_client *c, const char *args) {
    const char *r = kv_get(args, "rows"), *k = kv_get(args, "cols");
    int rows = r ? atoi(r) : 0, cols = k ? atoi(k) : 0;
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    if (!hold_term_size_valid(cols, rows)) {
        control_replyf(c, "error size must be 1-%d columns and rows\n", HOLD_TERM_MAX_DIM);
        return;
    }
    apply_size(b, rows, cols, true);
    control_replyf(c, "ok rows=%d cols=%d\n", rows, cols);
}

/* "signal sig=N": the chain of command for a shell that cannot see our
 * program's pid (another PID namespace) - callbox end/kill ask us, and we signal
 * the program's process group, which we can always see. */
static void handle_signal(struct broker *b, struct control_client *c, const char *args) {
    const char *v = kv_get(args, "sig");
    int sig = v ? atoi(v) : 0;
    if (sig != SIGTERM && sig != SIGKILL && sig != SIGINT && sig != SIGHUP) {
        control_replyf(c, "error unsupported signal\n");
        return;
    }
    if (b->target_done || b->target <= 0 || (kill(-b->target, sig) != 0 && errno == ESRCH)) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    control_replyf(c, "ok\n");
}

static bool screen_matches(struct broker *b, struct wait_request *w) {
    struct hold_snapshot snap;
    if (hold_screen_snapshot(b->screen, &snap) != 0) return false;
    size_t cap = (size_t)snap.rows * ((size_t)snap.cols * 4 * HOLD_CELL_CHARS + 1) + 1;
    char *text = malloc(cap);
    bool hit = false;
    if (text) {
        size_t len = 0;
        text[0] = '\0';
        for (int r = 0; r < snap.rows; r++) {
            hold_snapshot_row_text(&snap, r, text + len, cap - len);
            len += strlen(text + len);
            /* A row that wrapped into the next is one line, not two: text
             * cut by the right edge still matches (`wait --match` for a
             * message whose header pushed it past the edge). */
            bool wraps = r + 1 < snap.rows && hold_screen_row_continues(b->screen, r + 1);
            if (!wraps && len + 1 < cap) text[len++] = '\n';
            text[len] = '\0';
        }
        hit = w->match_regex ? (w->re_ok && regexec(&w->re, text, 0, NULL, 0) == 0) : strstr(text, w->match) != NULL;
        free(text);
    }
    hold_snapshot_free(&snap);
    return hit;
}

static void handle_wait(struct control_client *c, const char *args) {
    struct wait_request *w = &c->wait;
    memset(w, 0, sizeof(*w));
    w->start_ms = now_ms();
    const char *v;
    if ((v = kv_get(args, "idle"))) w->idle_ms = (uint32_t)strtoul(v, NULL, 10);
    if ((v = kv_get(args, "timeout"))) {
        long t = strtol(v, NULL, 10);
        if (t > 0) w->deadline_ms = w->start_ms + t;
    }
    if ((v = kv_get(args, "since"))) {
        w->has_since = true;
        w->since = strtoull(v, NULL, 10);
    }
    if ((v = kv_get(args, "exit"))) w->want_exit = *v == '1';
    if ((v = kv_get(args, "regex"))) w->match_regex = *v == '1';
    if ((v = kv_get(args, "match"))) {
        size_t vl = strcspn(v, " \n");
        w->match = strndup(v, vl);
        if (!w->match || hex_decode(w->match) < 0) {
            control_replyf(c, "error malformed match\n");
            return;
        }
        w->has_match = true;
        if (w->match_regex) {
            int rc = regcomp(&w->re, w->match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE);
            if (rc != 0) {
                char err[128];
                regerror(rc, &w->re, err, sizeof(err));
                control_replyf(c, "error bad regex: %s\n", err);
                return;
            }
            w->re_ok = true;
        }
    }
    c->phase = CTL_WAITING;
}

static int64_t quiet_since(const struct broker *b, const struct wait_request *w) {
    /* Without --since, silence counts only from when the wait began, so a
     * reply to input sent just before the wait is still awaited. */
    if (w->has_since || b->last_output_ms > w->start_ms) return b->last_output_ms;
    return w->start_ms;
}

/* Check one waiter; replies (and frees the slot) when a condition holds. */
static void check_wait(struct broker *b, struct control_client *c, int64_t now) {
    struct wait_request *w = &c->wait;
    /* With --since, a match counts only on a screen changed after SEQ:
     * text that was already there is not an answer to what was just sent. */
    bool match_fresh = !w->has_since || b->output_seq > w->since;
    if (w->has_match && match_fresh && screen_matches(b, w)) {
        control_replyf(c, "ok reason=match seq=%" PRIu64 "\n", b->output_seq);
        return;
    }
    if (b->target_done) {
        bool asked = w->want_exit || (!w->has_match && !w->idle_ms);
        control_replyf(c, "%s reason=exit seq=%" PRIu64 " exit_code=%d\n", asked ? "ok" : "ended", b->output_seq,
                       exit_code_of(b->target_status));
        return;
    }
    if (w->idle_ms) {
        /* Settled means drawn, then quiet: silence before the program's first
         * output (or, with --since, before fresh output) never counts. */
        bool fresh = w->has_since ? b->output_seq > w->since : b->output_seq > 0;
        if (fresh && now - quiet_since(b, w) >= (int64_t)w->idle_ms) {
            control_replyf(c, "ok reason=idle seq=%" PRIu64 "\n", b->output_seq);
            return;
        }
    }
    if (w->deadline_ms && now >= w->deadline_ms) {
        control_replyf(c, "timeout seq=%" PRIu64 "\n", b->output_seq);
    }
}

/* Milliseconds until this waiter could next change state by time alone. */
static int64_t wait_due_in(const struct broker *b, const struct control_client *c, int64_t now) {
    const struct wait_request *w = &c->wait;
    int64_t due = INT64_MAX;
    bool fresh = w->has_since ? b->output_seq > w->since : b->output_seq > 0;
    if (w->idle_ms && fresh) due = quiet_since(b, w) + w->idle_ms - now;
    if (w->deadline_ms && w->deadline_ms - now < due) due = w->deadline_ms - now;
    return due < 0 ? 0 : due;
}

struct name_lookup {
    const char *name;
    char id[ID_STR_LEN];
    int matches;
};

static int match_record_name(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct name_lookup *nl = ctx;
    if (!r) return 0; /* a corrupt record */
    if (r->has_name && !strcmp(r->name, nl->name)) {
        snprintf(nl->id, sizeof(nl->id), "%s", id);
        nl->matches++;
    }
    return 0;
}

/* Who is speaking: the claimed call (the sender's CALLBOX_ID, or a name)
 * is verified when the connecting process belongs to that call's session or
 * descends from its leader. */
/* 1 verified, 0 not verified, -1 the claimed call has no record (yet: a
 * call that messages the moment it starts can beat its own record write). */
static int verify_sender(const struct broker *b, pid_t peer, const char *claim, char *label, size_t n,
                         char id[ID_STR_LEN]) {
    snprintf(label, n, "%s", claim);
    id[0] = '\0';
    if (!b->store || peer <= 0) return 0;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_resolve_record_id(b->store->record_dir, claim, id, ID_STR_LEN) != 0) {
        struct name_lookup nl = {.name = claim};
        (void)hold_for_each_record(b->store->record_dir, match_record_name, &nl);
        if (nl.matches != 1) return nl.matches == 0 ? -1 : 0;
        memcpy(id, nl.id, ID_STR_LEN);
    }
    if (hold_load_record_by_id(b->store->record_dir, id, &r, path, sizeof(path)) != 0) return -1;
    if (r.has_name && r.name[0]) snprintf(label, n, "%s", r.name);
    /* Same session as the call, or a descendant of its leader: agent
     * harnesses (Claude Code among them) run each tool command in a new
     * session, but it still descends from the harness the call launched. Or
     * the call's own broker - a session leader, with only its helpers in its
     * session - speaking for it: a callbox shell's chat sends that way. */
    pid_t peer_sid = getsid(peer);
    bool ok = (peer_sid > 0 && r.sid > 0 && peer_sid == r.sid) || hold_pid_has_ancestor(peer, r.pid) ||
              (peer_sid > 1 && r.has_broker && peer_sid == r.broker_pid);
    hold_free_run_record(&r);
    return ok ? 1 : 0;
}

/* A message is text from a sender callbox does not trust: keep newlines and
 * tabs, drop every other control - C0 bytes, DEL, and C1 (U+0080-U+009F) -
 * so it can never type Escape into the receiver, submit early with a carriage
 * return, end a bracketed paste, or drive the terminal (titles, clears). */
static size_t strip_controls(char *t, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)t[i];
        if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f) continue;
        if (ch == 0xc2 && i + 1 < n && (unsigned char)t[i + 1] >= 0x80 && (unsigned char)t[i + 1] <= 0x9f) {
            i++;
            continue;
        }
        t[o++] = t[i];
    }
    return o;
}

static void handle_msg(struct broker *b, struct control_client *c, const char *args, char *body) {
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    int64_t now = now_ms();
    int recent = 0;
    for (int i = 0; i < BROKER_MSG_RATE; i++)
        if (b->sent_ms[i] && now - b->sent_ms[i] < 60000) recent++;
    if (recent >= BROKER_MSG_RATE) {
        control_replyf(c, "error rate limit: %d messages a minute to one call\n", BROKER_MSG_RATE);
        return;
    }
    int slot = -1;
    for (int i = 0; i < BROKER_MAX_MSGS && slot < 0; i++)
        if (!b->msgs[i].used) slot = i;
    if (slot < 0) {
        control_replyf(c, "error too many undelivered messages\n");
        return;
    }
    char *nl = strchr(body, '\n');
    if (nl) *nl = '\0';
    ssize_t blen = hex_decode(body);
    if (blen < 0) {
        control_replyf(c, "error malformed message\n");
        return;
    }
    struct pending_msg *m = &b->msgs[slot];
    memset(m, 0, sizeof(*m));
    const char *v = kv_get(args, "from");
    if (v) {
        size_t vl = strcspn(v, " ");
        if (vl > 0 && vl < sizeof(m->claim)) {
            memcpy(m->claim, v, vl);
            m->claim[vl] = '\0';
            /* Only an id or a call name counts as a claim: anything else
             * (spaces, newlines, control bytes) would end up in the .msgs
             * log and the typed header, where it could forge a line. */
            ssize_t cl = hex_decode(m->claim);
            m->has_claim = cl > 0 && (size_t)cl == strlen(m->claim) &&
                           (hold_valid_id_prefix(m->claim) || hold_valid_alias(m->claim));
            if (!m->has_claim) m->claim[0] = '\0';
        }
    }
    m->peer = c->peer_pid;
    m->text = malloc((size_t)blen + 1);
    if (!m->text) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    memcpy(m->text, body, (size_t)blen);
    m->len = strip_controls(m->text, (size_t)blen);
    m->text[m->len] = '\0';
    m->used = true;
    m->submit = !((v = kv_get(args, "submit")) && *v == '0');
    m->start_ms = now;
    m->waiter = c;
    m->waiter_gen = c->gen;
    b->sent_ms[b->sent_idx] = now;
    b->sent_idx = (b->sent_idx + 1) % BROKER_MSG_RATE;
    c->phase = CTL_MSG;
}

/* A short id per delivered message: the first 6 hex digits of a hash of the
 * call, the time, a counter, and the text (unique within one call's log). */
static void make_msg_id(struct broker *b, const struct pending_msg *m, char out[7]) {
    struct sha256_ctx h;
    unsigned char d[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t n = ++b->msg_count;
    hold_sha256_init(&h);
    if (b->run_id) hold_sha256_update(&h, b->run_id, strlen(b->run_id));
    hold_sha256_update(&h, &ts, sizeof(ts));
    hold_sha256_update(&h, &n, sizeof(n));
    hold_sha256_update(&h, m->text, m->len);
    hold_sha256_final(&h, d);
    snprintf(out, 7, "%02x%02x%02x", d[0], d[1], d[2]);
}

/* A root broker serving a call in a user's store (sudo from that user's
 * home) gives the files it creates beside the log the log's owner, so the
 * user can still read them (callbox peek, callbox msg --received). */
static void own_like_log(const struct broker *b, int fd) {
    struct stat st;
    if (geteuid() == 0 && b->logfd >= 0 && fstat(b->logfd, &st) == 0 && st.st_uid != 0)
        if (fchown(fd, st.st_uid, st.st_gid) != 0) { /* best effort: the file stays root's */ }
}

/* <id>.msgs beside <id>.log: one line per delivered message, "ID UNIXTIME
 * VERIFIED SENDER HEXTEXT", so the receiver can tell a message callbox
 * delivered (callbox msg --received ID) from text that only looks like one. */
static void record_received(const struct broker *b, const char *id, bool verified, const char *label,
                            const char *text, size_t len) {
    size_t ll = b->log_path ? strlen(b->log_path) : 0;
    if (ll < 5 || strcmp(b->log_path + ll - 4, ".log") != 0) return;
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%.*s.msgs", (int)(ll - 4), b->log_path) != 0) return;
    char head[ALIAS_MAX_LEN + ID_STR_LEN + 64];
    int hn = snprintf(head, sizeof(head), "%s %lld %d %s ", id, (long long)time(NULL), verified ? 1 : 0,
                      label && *label ? label : "-");
    if (hn < 0 || (size_t)hn >= sizeof(head)) return;
    char *line = malloc((size_t)hn + 2 * len + 2);
    if (!line) return;
    memcpy(line, head, (size_t)hn);
    static const char hx[] = "0123456789abcdef";
    size_t o = (size_t)hn;
    for (size_t i = 0; i < len; i++) {
        line[o++] = hx[(unsigned char)text[i] >> 4];
        line[o++] = hx[(unsigned char)text[i] & 15];
    }
    line[o++] = '\n';
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        own_like_log(b, fd);
        (void)hold_write_all(fd, line, o); /* one O_APPEND write per message */
        close(fd);
    }
    free(line);
}

/* A queued message has been handed over: its sender's callbox msg returns, and
 * the queue slot is free. */
static void msg_done(struct pending_msg *m, const char *id) {
    struct control_client *w = m->waiter;
    if (w && w->fd >= 0 && w->gen == m->waiter_gen && w->phase == CTL_MSG)
        control_replyf(w, "ok delivered verified=%d id=%s\n", m->verified ? 1 : 0, id);
    free(m->text);
    memset(m, 0, sizeof(*m));
}

/* Type a queued message: bracketed paste when the program asked for it, else
 * newlines flattened so it cannot submit early; then Enter. The header names
 * the sender, how to reply, and how to check it was really delivered. */
static void deliver_msg(struct broker *b, struct pending_msg *m, int verdict, const char *label,
                        const char *sender_id) {
    char id[7], header[400];
    make_msg_id(b, m, id);
    m->verified = verdict == 1;
    if (b->chat) {
        /* A callbox shell: the message goes to the chat inbox, filed under who
         * callbox verified it came from. It is shown, never typed: nothing a
         * sender writes reaches the shell's input. */
        record_received(b, id, m->verified, m->has_claim ? label : "", m->text, m->len);
        chat_inbound(b->chat, m->has_claim, verdict, sender_id, label, m->text, m->len);
        msg_done(m, id);
        return;
    }
    if (m->has_claim && verdict == 1)
        snprintf(header, sizeof(header),
                 "[msg %s from %.100s; reply: callbox msg %.100s \"...\"; verify: callbox msg --received %s] ", id, label,
                 label, id);
    else if (m->has_claim)
        snprintf(header, sizeof(header), "[msg %s from %.100s (unverified); verify: callbox msg --received %s] ", id,
                 label, id);
    else
        snprintf(header, sizeof(header), "[msg %s via callbox; verify: callbox msg --received %s] ", id, id);
    size_t hl = strlen(header);
    char *full = malloc(hl + m->len + 1);
    if (!full) {
        /* not typed: say so and free the slot, rather than retry it forever */
        struct control_client *w = m->waiter;
        if (w && w->fd >= 0 && w->gen == m->waiter_gen && w->phase == CTL_MSG)
            control_replyf(w, "error out of memory\n");
        free(m->text);
        memset(m, 0, sizeof(*m));
        return;
    }
    record_received(b, id, m->verified, m->has_claim ? label : "", m->text, m->len);
    memcpy(full, header, hl);
    memcpy(full + hl, m->text, m->len);
    size_t flen = hl + m->len;
    struct hold_screen_modes modes;
    hold_screen_modes(b->screen, &modes);
    if (modes.bracketed_paste) {
        hold_screen_send_paste(b->screen, full, flen);
        flush_screen_output(b);
    } else {
        for (size_t i = 0; i < flen; i++)
            if (full[i] == '\n' || full[i] == '\r') full[i] = ' ';
        (void)program_write(b, full, flen);
    }
    free(full);
    /* Enter: a carriage return on a PTY (its terminal makes it a newline), the
     * newline itself for a program on pipes. */
    if (m->submit) (void)program_write(b, b->ep.pty ? "\r" : "\n", 1);
    msg_done(m, id);
}

/* The oldest queued message goes in at once - unless its claimed sender's
 * record is not written yet, which gets the verify grace below. One per
 * pass, so messages arrive whole and in order. */
static void pump_msgs(struct broker *b, int64_t now) {
    struct pending_msg *oldest = NULL;
    for (int i = 0; i < BROKER_MAX_MSGS; i++)
        if (b->msgs[i].used && (!oldest || b->msgs[i].start_ms < oldest->start_ms)) oldest = &b->msgs[i];
    if (!oldest || b->target_done) return;
    /* Verify at delivery: by now a just-launched sender's record exists. If
     * the claimed call still has none, give it up to 2 s before deciding. */
    char label[ALIAS_MAX_LEN + ID_STR_LEN + 1] = "", sender_id[ID_STR_LEN] = "";
    int verdict = oldest->has_claim ? verify_sender(b, oldest->peer, oldest->claim, label, sizeof(label), sender_id) : 0;
    if (verdict == -1 && now - oldest->start_ms < BROKER_MSG_VERIFY_GRACE_MS) return;
    deliver_msg(b, oldest, verdict, label, sender_id);
}

static int64_t msgs_due_in(const struct broker *b) {
    int64_t due = INT64_MAX;
    for (int i = 0; i < BROKER_MAX_MSGS; i++) {
        const struct pending_msg *m = &b->msgs[i];
        if (!m->used) continue;
        /* Delivered at once; one still here waits on its sender's record. */
        int64_t d = m->has_claim ? 50 : 0;
        if (d < due) due = d;
    }
    return due < 0 ? 0 : due;
}

static void dispatch_control(struct broker *b, struct control_client *c) {
    char *req = (char *)c->buf + 4;
    char *nl = strchr(req, '\n');
    char *rest = nl ? nl + 1 : req + strlen(req);
    if (nl) *nl = '\0';
    char *args = strchr(req, ' ');
    if (args) *args++ = '\0';
    else args = req + strlen(req);
    if (!strcmp(req, "peek")) handle_peek(b, c, args);
    else if (!strcmp(req, "send")) handle_send(b, c, rest);
    else if (!strcmp(req, "resize")) handle_resize(b, c, args);
    else if (!strcmp(req, "signal")) handle_signal(b, c, args);
    else if (!strcmp(req, "msg")) handle_msg(b, c, args, rest);
    else if (!strcmp(req, "nested")) handle_nested(b, c, args);
    else if (!strcmp(req, "wait")) {
        handle_wait(c, args);
        if (c->fd >= 0) check_wait(b, c, now_ms());
    } else control_replyf(c, "error unknown request '%s'\n", req);
}

/* Read what is available; dispatch once the length-prefixed request is whole. */
static void read_control(struct broker *b, struct control_client *c) {
    if (c->phase == CTL_WAITING || c->phase == CTL_MSG || c->phase == CTL_NESTED) {
        /* A waiter that hangs up is done (a queued msg is still delivered);
         * a nested attach client that hangs up has detached or died.
         * Anything else it sends is ignored. */
        char sink[256];
        ssize_t n = read(c->fd, sink, sizeof(sink));
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) drop_control(c);
        return;
    }
    /* Drain what is there, not one read per poll: macOS does not always
     * report a FIFO readable again after a partial read while the writer is
     * still mid-write, so a large request would crawl a chunk per wakeup. */
    for (;;) {
        if (c->cap - c->len < 4096) {
            size_t cap = c->cap ? c->cap * 2 : 8192;
            unsigned char *p = realloc(c->buf, cap + 1);
            if (!p) {
                drop_control(c);
                return;
            }
            c->buf = p;
            c->cap = cap;
        }
        ssize_t n = read(c->fd, c->buf + c->len, c->cap - c->len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {
            drop_control(c);
            return;
        }
        c->len += (size_t)n;
        c->active_ms = now_ms();
        if (c->len < 4) continue;
        uint32_t want = console_load_be32(c->buf);
        if (want > HOLD_CONSOLE_MAX_REQUEST) {
            control_replyf(c, "error request too large\n");
            return;
        }
        if (c->len < 4 + (size_t)want) continue;
        c->buf[4 + want] = '\0';
        dispatch_control(b, c);
        return;
    }
}

/* ---- the last full-screen frame -------------------------------------------- */

/* Full-screen programs (vim, less, htop) draw on the alternate screen and
 * switch back when they exit, which leaves the normal screen showing nothing
 * of what they displayed. Before the switch-back takes effect, keep that
 * frame: feed up to the leave sequence, snapshot, feed the rest. Best effort
 * (a leave sequence split across reads is fed whole). */
static const char *find_alt_leave(const char *buf, size_t n) {
    static const char *const seqs[] = {"\033[?1049l", "\033[?1047l", "\033[?47l"};
    const char *first = NULL;
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        const char *hit = memmem(buf, n, seqs[i], strlen(seqs[i]));
        if (hit && (!first || hit < first)) first = hit;
    }
    return first;
}

static void keep_alt_frame(struct broker *b) {
    struct hold_snapshot snap;
    unsigned char *blob = NULL;
    size_t len = 0;
    if (take_snapshot(b, &snap) != 0) return;
    int rc = hold_snapshot_serialize(&snap, &blob, &len);
    hold_snapshot_free(&snap);
    if (rc != 0) return;
    free(b->last_alt);
    b->last_alt = blob;
    b->last_alt_len = len;
}

static void feed_screen_raw(struct broker *b, const char *buf, size_t n);

/* A program on pipes writes bare "\n": a PTY would have turned it into
 * "\r\n" (onlcr), so the headless screen gets the same - its lines then read
 * as they do on a terminal, and peek and wait work for it too. */
static void feed_screen(struct broker *b, const char *buf, size_t n) {
    if (b->ep.pty) {
        feed_screen_raw(b, buf, n);
        return;
    }
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != '\n') continue;
        if (i > from) feed_screen_raw(b, buf + from, i - from);
        feed_screen_raw(b, "\r\n", 2);
        from = i + 1;
    }
    if (from < n) feed_screen_raw(b, buf + from, n - from);
}

static void feed_screen_raw(struct broker *b, const char *buf, size_t n) {
    struct hold_screen_modes modes;
    hold_screen_modes(b->screen, &modes);
    const char *leave = modes.alt_screen ? find_alt_leave(buf, n) : NULL;
    if (!leave) {
        hold_screen_feed(b->screen, buf, n);
        return;
    }
    size_t head = (size_t)(leave - buf);
    hold_screen_feed(b->screen, buf, head);
    hold_screen_modes(b->screen, &modes);
    if (modes.alt_screen) keep_alt_frame(b);
    hold_screen_feed(b->screen, buf + head, n - head);
}

/* ---- the final frame ----------------------------------------------------- */

static void write_beside_log(const struct broker *b, const char *suffix, const unsigned char *blob, size_t len) {
    size_t ll = b->log_path ? strlen(b->log_path) : 0;
    if (ll < 5 || strcmp(b->log_path + ll - 4, ".log") != 0) return;
    char path[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%.*s%s", (int)(ll - 4), b->log_path, suffix) != 0 ||
        hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0)
        return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        own_like_log(b, fd);
        bool ok = hold_write_all(fd, blob, len) == 0;
        if (close(fd) != 0) ok = false;
        if (!ok || rename(tmp, path) != 0) unlink(tmp);
    }
}

/* <id>.screen beside <id>.log: the last screen, for callbox peek after exit;
 * <id>.altscreen: the last full-screen frame, for callbox peek --last-alt. */
static void write_final_screen(struct broker *b) {
    struct hold_snapshot snap;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    if (take_snapshot(b, &snap) != 0) return;
    int rc = hold_snapshot_serialize(&snap, &blob, &blob_len);
    hold_snapshot_free(&snap);
    if (rc == 0) write_beside_log(b, ".screen", blob, blob_len);
    free(blob);
    if (b->last_alt) write_beside_log(b, ".altscreen", b->last_alt, b->last_alt_len);
}

/* ---- the one serve loop ------------------------------------------------- */

static void reap_target(struct broker *b, bool block) {
    if (b->target_done || b->target <= 0) return;
    int st = 0;
    pid_t got;
    do {
        got = waitpid(b->target, &st, block ? 0 : WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == b->target) {
        b->target_done = true;
        b->target_status = st;
    }
    if (got == b->target || (got < 0 && errno == ECHILD)) {
        b->target = -1;
        g_broker_forward_target = 0;
    }
}

/* Another client wants the console. Before it is turned away: is the one
 * that has it still there? A client whose process no longer exists is not,
 * whatever its connection has (not yet) reported - a FIFO on macOS never
 * reports its end, and is only probed when the broker is otherwise idle. */
static bool console_taken(struct broker *b, struct attach_client *cl) {
    if (cl->fd < 0) return false;
    if (cl->peer_pid > 0 && kill(cl->peer_pid, 0) != 0 && errno == ESRCH) {
        drop_attach(b, cl);
        return false;
    }
    return true;
}

/* Turn a second attach away, naming the process that has the console (so
 * the person can be told where it is attached, not just that it is). */
static void refuse_attach(struct control_client *c, const struct attach_client *cl) {
    char msg[80];
    int n = cl->peer_pid > 0
                ? snprintf(msg, sizeof(msg), "callbox: console already attached (by pid %ld)\n", (long)cl->peer_pid)
                : snprintf(msg, sizeof(msg), "callbox: console already attached\n");
    (void)hold_write_all(c->fd, msg, (size_t)n);
    drop_control(c);
}

/* Route a freshly accepted connection once its first bytes are readable:
 * control magic -> control request; anything else -> the attach client. */
static void sniff_connection(struct broker *b, struct control_client *c, struct attach_client *cl) {
    unsigned char head[CONSOLE_CONTROL_MAGIC_LEN];
    ssize_t n = recv(c->fd, head, sizeof(head), MSG_PEEK);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_control(c);
        return;
    }
    if (n <= 0) return;
    size_t cmp = (size_t)n < CONSOLE_CONTROL_MAGIC_LEN ? (size_t)n : CONSOLE_CONTROL_MAGIC_LEN;
    if (memcmp(head, CONSOLE_CONTROL_MAGIC, cmp) == 0) {
        if ((size_t)n < CONSOLE_CONTROL_MAGIC_LEN) return; /* the rest is coming */
        if (read(c->fd, head, sizeof(head)) != (ssize_t)sizeof(head)) {
            drop_control(c);
            return;
        }
        c->phase = CTL_READING;
        read_control(b, c);
        return;
    }
    if (console_taken(b, cl)) {
        refuse_attach(c, cl);
        return;
    }
    memset(cl, 0, sizeof(*cl));
    cl->fd = c->fd;
    cl->output = c->output;
    cl->peer_pid = c->peer_pid;
    cl->needs_repaint = true;
    c->fd = -1;
    drop_control(c);
}

/* A connection that never spoke within BROKER_SNIFF_MS becomes a raw,
 * read-only attach so output-only clients still see the screen. */
static void sniff_timeout(struct broker *b, struct control_client *c, struct attach_client *cl, int64_t now) {
    if (c->phase != CTL_SNIFF || now - c->accepted_ms < BROKER_SNIFF_MS) return;
    if (console_taken(b, cl)) {
        refuse_attach(c, cl);
        return;
    }
    memset(cl, 0, sizeof(*cl));
    cl->fd = c->fd;
    cl->output = c->output;
    cl->peer_pid = c->peer_pid;
    cl->decided = true; /* raw */
    cl->needs_repaint = true;
    c->fd = -1;
    drop_control(c);
}

/* b->target is our child: waitpid lifecycle, killed on cleanup. Never returns. */
static void broker_serve(struct broker *b) {
    /* The serve loop always ends in _exit, so no handler save/restore. */
    signal(SIGPIPE, SIG_IGN);

    struct attach_client cl;
    memset(&cl, 0, sizeof(cl));
    cl.fd = -1;
    struct control_client ctl[BROKER_MAX_CONTROL];
    memset(ctl, 0, sizeof(ctl));
    for (int i = 0; i < BROKER_MAX_CONTROL; i++) ctl[i].fd = -1;
    b->ctl = ctl;
    bool target_marked = false;
    b->last_output_ms = now_ms();
    while (1) {
        if (!b->target_done) {
            reap_target(b, false);
            if (b->target_done && b->store && b->run_id && *b->run_id) {
                /* Best-effort while serving: an instantly-exiting target
                 * can beat the parent's record write, so a failure here
                 * is retried patiently after the serve loop ends. */
                target_marked = hold_mark_run_finished(b->store, b->run_id, b->target_status, b->program_pid) == 0;
            }
        }

        struct pollfd pfds[4 + BROKER_MAX_CONTROL + CHAT_MAX_OUTBOUND]; /* program, listener, client, input queue */
        int ctl_idx[BROKER_MAX_CONTROL];
        nfds_t nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = b->ep.out_fd, .events = POLLIN};
        nfds_t listener_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = b->listener, .events = POLLIN};
        int client_idx = -1;
        if (cl.fd >= 0) {
            /* A client whose input has ended is still watched, for its going
             * away (no events asked for: hangup and error are always
             * reported). Otherwise a client that vanished - its terminal
             * closed, the process killed - would be noticed only when the
             * program next printed something, and with a quiet program,
             * never: the console would stay "already attached". */
            client_idx = (int)nfds;
            /* (not read while its earlier input still waits for the program) */
            bool hold_client = b->inq_len >= BROKER_INQ_HIGH;
            pfds[nfds++] = cl.input_closed ? (struct pollfd){.fd = cl.output, .events = 0}
                                           : (struct pollfd){.fd = cl.fd, .events = hold_client ? 0 : POLLIN};
        }
        int inq_idx = -1;
        if (b->inq_len > 0 && b->ep.in_fd >= 0) {
            inq_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = b->ep.in_fd, .events = POLLOUT};
        }
        int64_t now = now_ms();
        int64_t timeout = 1000;
        int64_t msg_due = msgs_due_in(b);
        if (msg_due < timeout) timeout = msg_due;
        for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
            ctl_idx[i] = -1;
            if (ctl[i].fd < 0) continue;
            ctl_idx[i] = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = ctl[i].fd, .events = POLLIN};
            if (ctl[i].phase == CTL_WAITING) {
                int64_t due = wait_due_in(b, &ctl[i], now);
                if (due < timeout) timeout = due;
            } else if (ctl[i].phase == CTL_SNIFF || ctl[i].phase == CTL_READING) {
                int64_t due = ctl[i].phase == CTL_SNIFF ? ctl[i].accepted_ms + BROKER_SNIFF_MS - now
                                                        : ctl[i].active_ms + BROKER_READ_MS - now;
#if defined(__APPLE__)
                /* macOS does not report a FIFO readable again while its writer
                 * is blocked mid-write: a request partly read is probed soon
                 * (the quiet pass below), not once a second. */
                if (b->fifo_writer >= 0 && ctl[i].phase == CTL_READING && ctl[i].len > 0 && due > 5) due = 5;
#endif
                if (due < timeout) timeout = due < 0 ? 0 : due;
            }
        }

        /* A callbox shell: its sends in flight, its timers, and the next frame
         * when one is owed sooner than the frame rate allows. */
        nfds_t chat_idx = nfds;
        int chat_nfds = 0;
        if (b->chat) {
            chat_nfds = chat_poll_fds(b->chat, pfds + nfds, CHAT_MAX_OUTBOUND);
            nfds += (nfds_t)chat_nfds;
            int64_t due = chat_due_in(b->chat, now);
            if (due < timeout) timeout = due;
            if (cl.fd >= 0 && chat_wants_frame(b->chat)) {
                due = b->frame_ms + BROKER_FRAME_MS - now;
                if (due < timeout) timeout = due < 0 ? 0 : due;
            }
        }

        int pr = poll(pfds, nfds, (int)timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        /* Nothing happened: the exit check below uses this. (A callbox shell's
         * own timers do not count as the program stirring.) */
        bool quiet = pr == 0;
        if (b->chat) {
            if (pr > 0) chat_poll_done(b->chat, pfds + chat_idx, chat_nfds, now_ms());
            chat_tick(b->chat, now_ms());
        }
#if defined(__APPLE__)
        /* macOS poll() never reports end-of-file on a FIFO: on a quiet pass,
         * let the FIFO clients' (non-blocking) reads find out who hung up. */
        if (pr == 0 && b->fifo_writer >= 0) {
            for (int i = 0; i < BROKER_MAX_CONTROL; i++)
                if (ctl_idx[i] >= 0) pfds[ctl_idx[i]].revents = POLLIN;
            if (client_idx >= 0) pfds[client_idx].revents = POLLIN;
            pr = 1;
        }
#endif

        if (pr > 0 && (pfds[listener_idx].revents & POLLIN)) {
            bool fifo = b->fifo_writer >= 0;
            int output = -1;
            char kind = 0;
            uid_t fifo_uid = (uid_t)-1;
            pid_t fifo_pid = 0;
            int next = fifo ? hold_fifo_accept(b->listener, b->sock_path, &kind, &output, &fifo_uid, &fifo_pid)
                            : accept(b->listener, NULL, NULL);
            if (!fifo) output = next;
            if (next >= 0 && fifo && !uid_allowed(b, fifo_uid)) {
                (void)!write(output, "D", 1);
                close(output);
                close(next);
                next = -1;
            }
            if (next >= 0) {
                int slot = -1;
                for (int i = 0; i < BROKER_MAX_CONTROL && slot < 0; i++)
                    if (ctl[i].fd < 0) slot = i;
                if (fifo && kind == 'A') {
                    bool available = !console_taken(b, &cl);
                    char ack[16];
                    int ack_len = available ? snprintf(ack, sizeof(ack), "R")
                                            : snprintf(ack, sizeof(ack), "B%010ld", (long)(cl.peer_pid > 0 ? cl.peer_pid : 0));
                    if (write(output, ack, (size_t)ack_len) == ack_len && available) {
                        memset(&cl, 0, sizeof(cl));
                        cl.fd = next;
                        cl.output = output;
                        cl.peer_pid = fifo_pid;
                        cl.needs_repaint = true;
                    } else {
                        close(output);
                        close(next);
                    }
                } else if (!fifo && (set_fd_nonblocking(next) != 0 || !authorize_client(b, next))) {
                    close(next);
                } else if (slot < 0) {
                    /* Full: say so in protocol, so the client knows nothing
                     * was done and can retry, instead of seeing a bare reset. */
                    static const char busy[] = "error busy\n";
                    unsigned char frame[4 + sizeof(busy) - 1];
                    console_store_be32(frame, (uint32_t)(sizeof(busy) - 1));
                    memcpy(frame + 4, busy, sizeof(busy) - 1);
                    if (fifo) {
                        (void)!write(output, "B", 1);
                        close(output);
                    } else (void)!write(next, frame, sizeof(frame));
                    close(next);
                } else {
                    /* Every connection starts undecided; its first bytes
                     * route it to control or attach (polled next pass). */
                    memset(&ctl[slot], 0, sizeof(ctl[slot]));
                    ctl[slot].fd = next;
                    ctl[slot].output = output;
                    ctl[slot].phase = fifo ? CTL_READING : CTL_SNIFF;
                    ctl[slot].accepted_ms = ctl[slot].active_ms = now_ms();
                    ctl[slot].gen = ++b->next_gen;
                    if (fifo) {
                        ctl[slot].peer_pid = fifo_pid; /* verified: it holds the request FIFO open */
                        if (write(output, "R", 1) != 1) drop_control(&ctl[slot]);
                    } else if (hold_console_peer_pid(next, &ctl[slot].peer_pid) != 0) ctl[slot].peer_pid = 0;
                }
            }
        }

        for (int i = 0; pr > 0 && i < BROKER_MAX_CONTROL; i++) {
            struct control_client *c = &ctl[i];
            if (c->fd < 0 || ctl_idx[i] < 0 || !(pfds[ctl_idx[i]].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;
            if (c->phase == CTL_SNIFF) sniff_connection(b, c, &cl);
            else read_control(b, c);
        }

        if (client_idx >= 0 && cl.fd >= 0 && cl.input_closed) {
            /* Gone: the connection hung up - or, where poll does not say so
             * (macOS reports no hangup that was not asked for, and cannot
             * tell a closed socket from a half-closed one; nor a FIFO's end),
             * the client's process no longer exists. Looked at on every
             * pass, so the next client to knock finds the console free. */
            if ((pfds[client_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
                (cl.peer_pid > 0 && kill(cl.peer_pid, 0) != 0 && errno == ESRCH))
                drop_attach(b, &cl);
        } else if (client_idx >= 0 && cl.fd >= 0 &&
            (pfds[client_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(cl.fd, buf, sizeof(buf));
            if (n > 0) {
                if (process_attach_input(b, &cl, buf, (size_t)n) != 0) {
                    drop_attach(b, &cl);
                }
            } else if (n == 0) {
                if (!cl.decided && cl.pending_len > 0) {
                    (void)attach_input(b, cl.pending, cl.pending_len);
                    cl.pending_len = 0;
                }
                cl.decided = true;
                cl.input_closed = true;
            } else if (errno != EAGAIN && errno != EINTR) {
                drop_attach(b, &cl);
            }
        }
        if (cl.fd >= 0 && cl.needs_repaint && cl.decided) {
            cl.needs_repaint = false;
            if (repaint_attach(b, &cl) != 0) drop_attach(b, &cl);
        }

        if (inq_idx >= 0 && (pfds[inq_idx].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) flush_program_input(b);
        if (pr > 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            char buf[4096];
            ssize_t n = hold_term_pump_master_filtered(b->ep.out_fd, b->logfd, buf, sizeof(buf),
                                                       b->eof_echo ? strip_eof_echo : NULL, b);
            if (n > 0) {
                b->output_seq += (uint64_t)n;
                b->last_output_ms = now_ms();
                feed_screen(b, buf, (size_t)n);
                if (b->chat) {
                    /* A callbox shell's terminal is drawn from the screen, never
                     * from these bytes, so it cannot answer the program's
                     * queries: the screen always does. */
                    flush_screen_output(b);
                    chat_screen_changed(b->chat);
                } else {
                    if (cl.fd >= 0 && !cl.no_terminal) hold_screen_discard_output(b->screen);
                    else flush_screen_output(b);
                    /* Before the repaint, the client gets nothing: the repaint
                     * already includes these bytes. */
                    if (cl.fd >= 0 && !cl.needs_repaint && write_all_wait(cl.output, buf, (size_t)n) != 0) {
                        drop_attach(b, &cl);
                    }
                }
            } else if (n == 0) {
                break;
            }
        }

        now = now_ms();
        pump_msgs(b, now);
        for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
            if (ctl[i].fd >= 0 && ctl[i].phase == CTL_WAITING) check_wait(b, &ctl[i], now);
            else if (ctl[i].fd >= 0 && ctl[i].phase == CTL_READING && now - ctl[i].active_ms >= BROKER_READ_MS)
                drop_control(&ctl[i]);
            else if (ctl[i].fd >= 0) sniff_timeout(b, &ctl[i], &cl, now);
        }
        if (cl.fd >= 0 && cl.needs_repaint && cl.decided) {
            cl.needs_repaint = false;
            if (repaint_attach(b, &cl) != 0) drop_attach(b, &cl);
        }
        if (b->chat && cl.fd >= 0 && cl.decided && !cl.needs_repaint && chat_wants_frame(b->chat) &&
            now - b->frame_ms >= BROKER_FRAME_MS && send_frame(b, &cl, now) != 0) {
            drop_attach(b, &cl);
        }
        if (quiet && b->target_done) {
            break;
        }
    }

    /* No more output can come (the program exited, or closed its terminal
     * and may run on). Stop taking requests at once, so a client never waits
     * on a console that will not answer, and save the screen before waiting
     * for the program: peek then shows this final screen right away. */
    if (b->sock_path && *b->sock_path) unlink(b->sock_path);
    if (b->listener >= 0) close(b->listener);
    if (b->fifo_writer >= 0) close(b->fifo_writer);
    b->listener = b->fifo_writer = -1;
    write_final_screen(b);

    /* A program that closed its terminal and runs on has nothing more to
     * show, and nothing typed can reach it: whoever is attached is let go
     * now, not whenever it ends. (A program that is exiting is reaped within
     * moments; its client leaves below, with the exit recorded.) */
    for (int i = 0; i < 50 && !b->target_done && b->target > 0; i++) {
        reap_target(b, false);
        if (b->target_done || b->target <= 0) break;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 10 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
    if (!b->target_done && b->target > 0 && cl.fd >= 0) drop_attach(b, &cl);

    reap_target(b, true);
    if (b->target_done && !target_marked && b->store && b->run_id && *b->run_id) {
        /* The exit stamp must not be lost to the launch race: retry until the
         * parent's record exists. rc 1 means the record was purged while the
         * call was exiting — that removal is final, do not resurrect it. */
        for (int i = 0; i < 50; i++) {
            int mark_rc = hold_mark_run_finished(b->store, b->run_id, b->target_status, b->program_pid);
            if (mark_rc == 0 || mark_rc == 1) break;
            struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
            while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                continue;
            }
        }
    }
    /* Again, now that the program has been reaped: the saved screen then
     * records how it exited (the early copy may have said it was running). */
    write_final_screen(b);
    /* Pending waiters learn the outcome: their condition may still hold on
     * the final screen, else they see the exit. */
    int64_t now = now_ms();
    for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
        if (ctl[i].fd < 0) continue;
        if (ctl[i].phase == CTL_WAITING) check_wait(b, &ctl[i], now);
        /* A connection not yet known to be control may be an attach client
         * arriving as the call ends: a control reply would be printed as
         * the program's output. Closing it says the call is over. */
        else if (ctl[i].phase != CTL_SNIFF) control_replyf(&ctl[i], "error the call has ended\n");
        if (ctl[i].fd >= 0) drop_control(&ctl[i]);
    }
    for (int i = 0; i < BROKER_MAX_MSGS; i++) free(b->msgs[i].text);
    if (cl.fd >= 0) drop_attach(b, &cl);
    broker_cleanup_and_exit(b, 0);
}

/* ---- entry point --------------------------------------------------------- */

void hold_run_console_broker(int parent_pipe,
                               int target_pid_fd,
                               const struct hold_store *store,
                               const char *run_id,
                               const char *log_path,
                               const char *sock_path,
                               uid_t owner_uid,
                               bool have_allowed_peer_uid,
                               uid_t allowed_peer_uid,
                               int argc,
                               char **argv,
                               const char *exec_path,
                               const struct hold_console_options *options) {
    unsigned short init_rows = options && options->rows ? options->rows : HOLD_TERM_DEFAULT_ROWS;
    unsigned short init_cols = options && options->cols ? options->cols : HOLD_TERM_DEFAULT_COLS;
    struct broker b = {
        .store = store,
        .run_id = run_id,
        .log_path = log_path,
        .sock_path = sock_path,
        .parent_pipe = parent_pipe,
        .listener = -1,
        .fifo_writer = -1,
        .ep = {.in_fd = -1, .out_fd = -1},
        .logfd = -1,
        .target = -1,
        .owner_uid = owner_uid,
        .have_allowed_peer_uid = have_allowed_peer_uid,
        .allowed_peer_uid = allowed_peer_uid,
    };
    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(&b, EINVAL);
    }
    b.screen = hold_screen_new(init_rows, init_cols);
    if (!b.screen) {
        broker_fail_errno(&b, ENOMEM);
    }
    b.listener = hold_make_console_listener(sock_path, &b.fifo_writer);
    if (b.listener < 0) {
        broker_fail_errno(&b, errno);
    }
    b.logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (b.logfd < 0) {
        broker_fail_errno(&b, errno);
    }
    /* "alive", for shells that cannot see our pid - only when the fallback
     * was selected (our listener is a FIFO); the socket broker is untouched. */
    if (b.fifo_writer >= 0) hold_supervisor_lock_log(log_path);

    /* The one spawn engine puts the target on a fresh PTY (setsid + TIOCSCTTY
     * + dup2 x3 + exec behind the errno handshake). On failure nothing is
     * left open or unreaped, and the errno rides our own handshake pipe up to
     * the launching parent. The broker's listener/log fds are all CLOEXEC, so
     * the target never inherits them past exec. */
    struct hold_term_spawn spawn = {
        .argv = argv,
        .exec_path = exec_path,
        .rows = init_rows,
        .cols = init_cols,
    };
    int master = -1;
    b.log_start = lseek(b.logfd, 0, SEEK_END);
    if (b.log_start < 0) b.log_start = 0;
    if (options && options->pipe) {
        int in_w = -1, out_r = -1;
        if (hold_term_pipe_spawn(&spawn, &in_w, &out_r, &b.target) != 0) {
            broker_fail_errno(&b, errno);
        }
        int fl = fcntl(in_w, F_GETFL);
        if (fl >= 0) (void)fcntl(in_w, F_SETFL, fl | O_NONBLOCK);
        b.ep = (struct program_endpoint){.in_fd = in_w, .out_fd = out_r, .pty = false};
    } else {
        if (hold_term_pty_spawn(&spawn, &master, &b.target) != 0) {
            broker_fail_errno(&b, errno);
        }
        b.ep = (struct program_endpoint){.in_fd = master, .out_fd = master, .pty = true};
    }
    if (options && options->chat) {
        struct chat_host host = {
            .screen = b.screen,
            .master = b.ep.in_fd,
            .store = store,
            .run_id = run_id,
            .lead = options->lead_key,
            .detach_key = options->detach_key,
            .key = options->chat_key,
            .program = options->program,
        };
        b.chat = chat_new(&host);
        if (!b.chat) broker_fail_errno(&b, ENOMEM);
    }
    /* Report the real held group to the parent before releasing the handshake:
     * a failed write must still ride the handshake pipe as an errno, and the
     * write-before-close ordering keeps the parent's pid read deadlock-free. */
    b.program_pid = b.target;
    if (target_pid_fd >= 0) {
        if (hold_write_all(target_pid_fd, &b.target, sizeof(b.target)) != 0) {
            broker_fail_errno(&b, errno);
        }
        close(target_pid_fd);
    }
    close(parent_pipe);
    b.parent_pipe = -1;

    /* Forward a TERM aimed at the broker to the held group (no SA_RESTART so
     * the serve loop's poll/waitpid see EINTR). The handler is async-signal-
     * safe and also unblocks a TERM arriving during the final blocking waitpid. */
    g_broker_forward_target = b.target;
    struct sigaction term_sa;
    memset(&term_sa, 0, sizeof(term_sa));
    term_sa.sa_handler = broker_forward_term;
    sigemptyset(&term_sa.sa_mask);
    (void)sigaction(SIGTERM, &term_sa, NULL);

    broker_serve(&b);
}
