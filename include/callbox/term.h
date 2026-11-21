#pragma once
#ifndef HOLD_TERM_H
#define HOLD_TERM_H

/* term: the one PTY spawn engine and the one PTY-master pump. This layer
 * knows PTYs, processes-on-PTYs, and pumps — never records, sockets, or
 * commands (layer DAG: store -> term -> console). */

#include "callbox/config.h"

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

/* THE CLOEXEC pipe: pipe2(O_CLOEXEC) where available, else pipe() + FD_CLOEXEC
 * (acceptable: callbox is single-threaded at every spawn site). Returns 0 with
 * fds[0]=read, fds[1]=write, or -1 with errno set and nothing left open. */
int hold_cloexec_pipe(int fds[2]);

/* One drain step of a PTY master: reads once into buf[n], appends the bytes
 * to the call's log, and returns them for caller fan-out (screen, client).
 * Returns >0 bytes pumped; 0 when the target side is gone (EOF, or EIO from
 * a failed read after the last slave closed); -1 on a retryable read error
 * (errno preserved). */
ssize_t hold_term_pump_master(int master, int logfd, char *buf, size_t n);

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
};

/* Sink for bytes the FSM releases; nonzero aborts the feed/flush with -1. */
typedef int (*hold_term_detach_sink)(void *ctx, const unsigned char *bytes, size_t n);

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys);
int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, int *completed);
/* Send through whatever is held (the input has ended: no key will follow). */
int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx);

#endif /* HOLD_TERM_H */
