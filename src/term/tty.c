#include "callbox/config.h"
#include "callbox/core.h"
#include "callbox/term.h"

/* The person's terminal, while a callbox client has it: the input mode it is
 * put in and handed back from, and the signals that must not leave it in that
 * mode. One copy, so the attach client and the log follower restore a
 * terminal the same way. */

int hold_term_input_enter(int fd, enum hold_term_input_mode mode, struct hold_term_saved *saved) {
    memset(saved, 0, sizeof(*saved));
    if (mode == HOLD_TERM_INPUT_AS_IS || !isatty(fd) || tcgetattr(fd, &saved->t) != 0) return 0;
    struct termios t = saved->t;
    if (mode == HOLD_TERM_INPUT_RAW) {
        cfmakeraw(&t);
    } else {
        /* Key by key, for a client that reads keys itself (detach keys,
         * Ctrl-C) but leaves output as it was: no line buffering, no echo, no
         * signals from keys, no Ctrl-S/Ctrl-Q flow control (Ctrl-Q is half of
         * the detach keys). Output processing stays, so "\n" still starts a
         * new line. */
        t.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
    }
    if (tcsetattr(fd, TCSAFLUSH, &t) != 0) return -1;
    saved->saved = true;
    saved->fd = fd;
    return 0;
}

void hold_term_input_restore(struct hold_term_saved *saved) {
    if (!saved->saved) return;
    (void)tcsetattr(saved->fd, TCSAFLUSH, &saved->t);
    saved->saved = false;
}

/* ---- signals that must not leave the terminal behind ---------------------- */

static volatile sig_atomic_t g_resized = 0;
static volatile sig_atomic_t g_stop_sig = 0;
static volatile sig_atomic_t g_interrupted = 0;

static void on_winch(int signo) {
    (void)signo;
    g_resized = 1;
}

/* SIGTERM or SIGHUP: leave the loop, give the terminal back as it was, then
 * die of the signal as the default action would have. */
static void on_stop(int signo) {
    g_stop_sig = signo;
}

static void on_interrupt(int signo) {
    (void)signo;
    g_interrupted = 1;
}

static bool install(int sig, void (*fn)(int), struct sigaction *old) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask); /* no SA_RESTART: poll and read see EINTR */
    return sigaction(sig, &sa, old) == 0;
}

void hold_term_signals_install(struct hold_term_signals *s, bool want_interrupt) {
    memset(s, 0, sizeof(*s));
    g_resized = 1; /* the first pass reports the starting size */
    g_stop_sig = 0;
    g_interrupted = 0;
    s->have_winch = install(SIGWINCH, on_winch, &s->old_winch);
    s->have_pipe = install(SIGPIPE, SIG_IGN, &s->old_pipe);
    s->have_term = install(SIGTERM, on_stop, &s->old_term);
    s->have_hup = install(SIGHUP, on_stop, &s->old_hup);
    /* A signal the caller ignores (nohup) stays ignored. */
    if (s->have_term && s->old_term.sa_handler == SIG_IGN) sigaction(SIGTERM, &s->old_term, NULL);
    if (s->have_hup && s->old_hup.sa_handler == SIG_IGN) sigaction(SIGHUP, &s->old_hup, NULL);
    if (want_interrupt) s->have_int = install(SIGINT, on_interrupt, &s->old_int);
}

bool hold_term_take_resized(void) {
    if (!g_resized) return false;
    g_resized = 0;
    return true;
}

int hold_term_stop_signal(void) {
    return g_stop_sig;
}

bool hold_term_take_interrupt(void) {
    if (!g_interrupted) return false;
    g_interrupted = 0;
    return true;
}

void hold_term_signals_restore(struct hold_term_signals *s) {
    if (s->have_winch) sigaction(SIGWINCH, &s->old_winch, NULL);
    if (s->have_pipe) sigaction(SIGPIPE, &s->old_pipe, NULL);
    if (s->have_term) sigaction(SIGTERM, &s->old_term, NULL);
    if (s->have_hup) sigaction(SIGHUP, &s->old_hup, NULL);
    if (s->have_int) sigaction(SIGINT, &s->old_int, NULL);
    memset(s, 0, sizeof(*s));
}

void hold_term_reraise_stop(void) {
    int sig = g_stop_sig;
    if (!sig) return;
    /* The terminal is restored; now end as the signal meant us to. */
    signal(sig, SIG_DFL);
    raise(sig);
}

int hold_term_size(struct winsize *ws) {
    memset(ws, 0, sizeof(*ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) return 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) return 0;
    return -1;
}
