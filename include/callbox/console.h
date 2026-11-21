#pragma once
#ifndef HOLD_CONSOLE_H
#define HOLD_CONSOLE_H

#include "callbox/config.h"
#include "callbox/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h.
 *
 * The broker serves one interactive attach client at a time (others receive
 * a short "already attached" error) plus short-lived control clients. */
int hold_format_console_sock_path(const struct hold_store *store,
                             const char *id,
                             char *out,
                             size_t n);
int hold_console_set_detach_keys(const unsigned char *keys, size_t len);
/* How a call's console starts. rows/cols: the size of its PTY and of callbox's
 * screen before the program runs (the launcher resolves it; the broker has no
 * terminal to ask). chat: a callbox shell - the broker composites the display
 * for whoever attaches (the shell's screen under the chat window, see
 * chat.c) and takes callbox msg into the chat inbox instead of typing it. */
#define HOLD_LEAD_KEY_DEFAULT 0x10   /* Ctrl-P */
#define HOLD_DETACH_KEY_DEFAULT 0x11 /* Ctrl-Q */
#define HOLD_CHAT_KEY_DEFAULT 0x14   /* Ctrl-T */
struct hold_console_options {
    unsigned short rows, cols;
    bool chat;
    /* A callbox shell's keys, read by its broker (which can then show what the
     * lead key offers while it waits): the lead key, and what follows it to
     * detach or to open chat. 0 = the default. */
    unsigned char lead_key, detach_key, chat_key;
};
/* The detach keys in force for this process (--detach-keys, else Ctrl-P
 * Ctrl-Q): copies up to cap of them, returns how many there are. */
size_t hold_console_get_detach_keys(unsigned char *out, size_t cap);
/* The call about to be attached reads the lead key itself (a callbox shell):
 * the attach client passes every key through and leaves them to its broker. */
void hold_console_set_composited(bool on);
/* target_pid_fd: -1 = do not report; otherwise the broker writes its forked
 * target pid exactly once after exec-handshake success, then closes the fd. The
 * write precedes closing parent_pipe so a pid-write failure still reaches the
 * parent's handshake read as an errno; the ordering also guarantees that
 * handshake EOF at the parent implies the pid was already written (or the broker
 * died), so the parent's unbounded pid read cannot deadlock. */
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
                        const struct hold_console_options *options);
/* Largest control request the broker accepts. Text in send/msg/wait --match
 * travels hex-encoded, so about half of this is usable text. */
#define HOLD_CONSOLE_MAX_REQUEST (1u << 20)

/* One control request to a call's broker (callbox peek/send/wait/resize): the
 * reply (NUL-terminated, malloc'd) begins with a text status line; peek's
 * carries an HSNAP1 snapshot after it. -1 with errno on connect/IO failure
 * (EMSGSIZE: longer than HOLD_CONSOLE_MAX_REQUEST). */
int hold_console_request(const char *sock_path, const char *request, size_t request_len,
                         unsigned char **reply, size_t *reply_len);

/* Whether callbox's fallback is selected: CALLBOX_CONSOLE_TRANSPORT=fifo, or Unix
 * sockets are denied (some sandboxes refuse them). Only then do calls carry
 * the cross-shell machinery (supervisor lock, -d control FIFO); otherwise the
 * original broker and blocking waiter run untouched. */
bool hold_console_fallback_selected(void);

/* Remove FIFO-console exchange directories ("<endpoint>.XXXXXX") left by
 * clients that died mid-handshake. */
void hold_remove_fifo_exchanges(const char *endpoint_path);

/* THE client loop (console/client.c): attaching to a console and following a
 * log both run here. The output source is a console connection, or a log
 * file read as it grows (out_is_file; file_done says when no more can come,
 * NULL = stop at its end). Keys from in_fd go through *keys (the detach
 * chord and, when set, the interrupt byte); with keys NULL they go straight
 * to on_input. Hooks return <0 error, >0 stop, 0 go on; a missing on_detach
 * or on_interrupt stops. want_sigint makes SIGINT an interrupt instead of
 * killing the client. The caller owns the terminal's mode and, after a
 * SIGNALED end, restores it and calls hold_term_reraise_stop(). */
struct hold_term_detach;
enum hold_client_end {
    HOLD_CLIENT_OUTPUT_ENDED, /* the source closed (or the followed call ended) */
    HOLD_CLIENT_STOPPED,      /* a hook asked to stop */
    HOLD_CLIENT_SIGNALED,     /* SIGTERM or SIGHUP */
    HOLD_CLIENT_ERROR,
};
struct hold_client {
    int out_fd;
    bool out_is_file;
    int (*file_done)(void *ctx);
    int (*on_output)(void *ctx, const char *buf, size_t n); /* NULL: to stdout */
    bool probe_fifo;
    int in_fd; /* -1: read no input */
    struct hold_term_detach *keys;
    int (*on_input)(void *ctx, const unsigned char *bytes, size_t n);
    int (*on_detach)(void *ctx);
    int (*on_interrupt)(void *ctx);
    int (*on_input_end)(void *ctx);
    int (*on_resize)(void *ctx, const struct winsize *ws);
    bool want_sigint;
    void *ctx;
};
enum hold_client_end hold_client_run(const struct hold_client *c);

/* The attach client: connect to a call's console socket and bridge the
 * terminal until the detach chord (Ctrl-P Ctrl-Q by default) or the call ends. */
int hold_run_native_console(const char *sock_path);
/* The console of the call this process runs inside (CALLBOX_ID), when it
 * has one: an attach started from here is nested in that call's terminal,
 * and says so to that call's broker for as long as it is attached. The
 * detach keys then let go of the innermost attach first - they reach the
 * outermost client, whose broker passes them inward instead of detaching -
 * and an attach left inside a terminal nobody is attached to any more lets
 * go by itself. "" or NULL: not inside a call. */
void hold_console_set_enclosing(const char *sock_path);
/* Whether the last hold_run_native_console was turned away because another
 * client has the console (*by its pid, 0 unknown). The function then returns
 * 3 and prints nothing itself: its caller knows the call's name. */
bool hold_console_was_refused(pid_t *by);
/* Whether it found no console to connect to: the call's output had already
 * ended. Returns 3 and prints nothing then, as above. */
bool hold_console_was_gone(void);

#endif /* HOLD_CONSOLE_H */
