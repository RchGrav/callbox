#pragma once
#ifndef HOLD_TERM_H
#define HOLD_TERM_H

/* term: the one PTY spawn engine and the one PTY-master pump. This layer
 * knows PTYs, processes-on-PTYs, and pumps — never records, sockets, or
 * commands (layer DAG: store -> term -> console). */

#include "backmux/config.h"

struct hold_term_spawn {
    char *const *argv;     /* required: argv[0] non-NULL, NULL-terminated */
    const char *exec_path; /* pre-resolved binary => execv; NULL/"" => execvp(argv[0]) */
    const char *cwd;       /* chdir in the child before exec; NULL/"" => inherit */
    unsigned short rows;   /* initial PTY window size; 0x0 => 80x24 preset */
    unsigned short cols;
};

/* Spawns argv on a fresh PTY: opens the pair (nonzero winsize before exec),
 * forks, and in the child does setsid + TIOCSCTTY + dup2 x3 + exec, with any
 * pre-exec failure reported over the errno handshake (EOF = exec succeeded).
 * On success returns 0 with the PTY master and child pid; on any failure
 * returns -1 with errno set and nothing left open or unreaped. */
int hold_term_pty_spawn(const struct hold_term_spawn *spec,
                        int *master_out, pid_t *pid_out);

/* Spawns argv on pipes instead: its stdin reads from *in_w_out's pipe, its
 * stdout and stderr write to *out_r_out's, in a session of its own. For a
 * program whose input is relayed (backmux -i): the relay can be cut and
 * reconnected, and the program never holds a person's terminal. */
int hold_term_pipe_spawn(const struct hold_term_spawn *spec,
                         int *in_w_out, int *out_r_out, pid_t *pid_out);

/* THE CLOEXEC pipe: pipe2(O_CLOEXEC) where available, else pipe() + FD_CLOEXEC
 * (acceptable: backmux is single-threaded at every spawn site). Returns 0 with
 * fds[0]=read, fds[1]=write, or -1 with errno set and nothing left open. */
int hold_cloexec_pipe(int fds[2]);

/* One drain step of a PTY master: reads once into buf[n], appends the bytes
 * to the call's log, and returns them for caller fan-out (screen, client).
 * Returns >0 bytes pumped; 0 when the target side is gone (EOF, or EIO from
 * a failed read after the last slave closed); -1 on a retryable read error
 * (errno preserved). */
ssize_t hold_term_pump_master(int master, int logfd, char *buf, size_t n);
/* The same, with what was read passed through filter (bytes in -> bytes out,
 * at most 4 more than went in) before the log and the caller see it. A read
 * the filter took entirely returns -1 with EAGAIN: nothing now, not the end. */
typedef size_t (*hold_term_pump_filter)(void *ctx, const char *in, size_t n, char *out);
ssize_t hold_term_pump_master_filtered(int master, int logfd, char *buf, size_t n,
                                       hold_term_pump_filter filter, void *ctx);

/* THE detach-key FSM (Ctrl-P Ctrl-Q by default). The first key is a lead
 * key: it is held back until the next key says what it meant, however long
 * that takes - there is no timer, so a slow second key still works, and a
 * person can stop to think. The rest of the chord detaches (*completed
 * reports 1, nothing is forwarded); the lead key again sends one lead key
 * through; any other key sends the held keys and itself through, in order.
 * So nothing typed is ever lost, and the lead key alone reaches the program
 * as soon as anything follows it. A one-key chord detaches at once. */
#define HOLD_TERM_DETACH_MAX_KEYS 8

struct hold_term_detach {
    unsigned char keys[HOLD_TERM_DETACH_MAX_KEYS];
    size_t nkeys;
    unsigned char pending[HOLD_TERM_DETACH_MAX_KEYS];
    size_t pending_len;
    unsigned char intr; /* 0 = none; else this byte reports *completed = 2 */
};

/* Sink for bytes the FSM releases; nonzero aborts the feed/flush with -1. */
typedef int (*hold_term_detach_sink)(void *ctx, const unsigned char *bytes, size_t n);

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys);
int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, int *completed);
/* Send through whatever is held (the input has ended: no key will follow). */
int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx);
/* Also report this byte (Ctrl-C, 0x03) as an interrupt: *completed = 2, the
 * byte not forwarded. For a client whose program is not on a PTY, where a
 * typed Ctrl-C would otherwise be lost: the client turns it into a signal. A
 * held lead key is sent through first. 0 turns it off (the default). */
void hold_term_detach_set_interrupt(struct hold_term_detach *d, unsigned char intr);

/* ---- line editing for a program on pipes (term/line.c) -------------------- */

struct hold_term_line {
    char buf[4096];
    size_t len;
    int echo_fd; /* where typed characters are echoed; -1 = nowhere */
    int esc;     /* inside an escape sequence (arrow keys): 0 no, 1 ESC, 2 CSI, 3 SS3 */
    bool after_cr; /* the last byte was a CR: a LF right after it is the same Enter */
};
void hold_term_line_init(struct hold_term_line *l, int echo_fd);
/* One key: printable keys and tab are echoed and kept; Enter sends the line
 * (with "\n") to sink; backspace and Ctrl-U erase; Ctrl-D sends what is typed,
 * or on an empty line sets *eof. Other control keys are ignored. */
int hold_term_line_feed(struct hold_term_line *l, unsigned char c, hold_term_detach_sink sink, void *ctx,
                        bool *eof);
void hold_term_line_discard(struct hold_term_line *l); /* forget the typed line (Ctrl-C) */

/* ---- the person's terminal (term/tty.c) ----------------------------------- */

enum hold_term_input_mode {
    HOLD_TERM_INPUT_AS_IS, /* leave it alone */
    HOLD_TERM_INPUT_RAW,   /* cfmakeraw: every byte through, the program's PTY does the rest */
    HOLD_TERM_INPUT_KEYS,  /* key by key, no echo, no key signals; output processing kept */
};

struct hold_term_saved {
    struct termios t;
    int fd;
    bool saved;
};

/* Put fd (a terminal; anything else is left alone, returning 0) in mode,
 * remembering how it was. -1 when the terminal refused the change. */
int hold_term_input_enter(int fd, enum hold_term_input_mode mode, struct hold_term_saved *saved);
void hold_term_input_restore(struct hold_term_saved *saved);

/* SIGWINCH (resized), SIGTERM/SIGHUP (stop: restore the terminal, then die of
 * the signal), SIGPIPE ignored, and optionally SIGINT (interrupt) - so no
 * client can leave the terminal raw. A signal the caller ignores stays so. */
struct hold_term_signals {
    struct sigaction old_winch, old_pipe, old_term, old_hup, old_int;
    bool have_winch, have_pipe, have_term, have_hup, have_int;
};
void hold_term_signals_install(struct hold_term_signals *s, bool want_interrupt);
void hold_term_signals_restore(struct hold_term_signals *s);
bool hold_term_take_resized(void);   /* since the last call (true once at install) */
int hold_term_stop_signal(void);     /* SIGTERM/SIGHUP received, or 0 */
bool hold_term_take_interrupt(void); /* SIGINT received since the last call */
void hold_term_reraise_stop(void);   /* after restoring: die of the stop signal */
int hold_term_size(struct winsize *ws); /* stdin's (else stdout's) window, -1 if none */

#endif /* HOLD_TERM_H */
