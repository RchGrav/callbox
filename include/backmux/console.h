#pragma once
#ifndef HOLD_CONSOLE_H
#define HOLD_CONSOLE_H

#include "backmux/config.h"
#include "backmux/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h.
 *
 * The broker serves one interactive attach client at a time. A new attachment
 * replaces the previous client; control requests use separate connections. */
int hold_format_console_sock_path(const struct hold_store *store,
                             const char *id,
                             char *out,
                             size_t n);
int hold_console_set_detach_keys(const unsigned char *keys, size_t len);
/* Broker options. The launcher resolves rows/cols before the program starts.
 * When chat is enabled, the broker composites a chat overlay and routes
 * messages to its inbox. Current CLI launches use the attach client's chat
 * overlay instead. */
#define HOLD_LEAD_KEY_DEFAULT 0x00   /* Ctrl-Space (NUL): the prefix */
#define HOLD_DETACH_KEY_DEFAULT 'q'  /* the prefix then q detaches */
#define HOLD_CHAT_KEY_DEFAULT 'c'    /* the prefix then c: chat */
struct hold_console_options {
    unsigned short rows, cols;
    /* A fresh launch on the person's terminal starts inline: the screen
     * model's cursor starts where theirs is (1-based; 0 = top, as before). */
    unsigned short start_row, start_col;
    bool chat;
    /* The program on pipes, not a PTY (backmux -i): its input is relayed by
     * the broker, so it can be cut and reconnected and the program never
     * holds a person's terminal. */
    bool pipe;
    bool program; /* program, rather than shell, in broker-chat hints */
    /* Broker-chat keys: prefix, detach, and chat toggle. */
    unsigned char lead_key, detach_key, chat_key;
};
/* The detach sequence (--detach-keys, otherwise Ctrl-Space q): copies up to
 * cap bytes and returns the full sequence length. */
size_t hold_console_get_detach_keys(unsigned char *out, size_t cap);
/* Broker-composited mode: the attach client forwards keys for the broker
 * to interpret. */
void hold_console_set_composited(bool on);
/* The process about to be attached runs on pipes (backmux -i): the attach keeps
 * the terminal's lines (no alternate screen), edits each line itself, and
 * turns Ctrl-C into an interrupt and Ctrl-D into end of input. */
void hold_console_set_line_mode(bool on);
/* A launch attaching to the program it just started, from a script (not a
 * terminal): ask for the run's output as a stream rather than a screen
 * repaint. backmux attach keeps its repaint of the live screen. */
void hold_console_set_stream(bool on);
/* A launch's own attach: SIGTERM to backmux is passed on to the program. */
void hold_console_set_forward_term(bool on);
/* A fresh launch attaching where the person's cursor is: no alternate
 * screen (the screen is painted from the model, which starts on the
 * cursor's row), nothing reprinted when it ends - it is all there already. */
void hold_console_set_inline(bool on);
bool hold_console_was_inline(void);
/* Whether the last attach stopped because its stdout reader went away. */
bool hold_console_reader_gone(void);
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

/* One control request to a process's broker (backmux peek/send/wait/resize): the
 * reply (NUL-terminated, malloc'd) begins with a text status line; peek's
 * carries an HSNAP1 snapshot after it. -1 with errno on connect/IO failure
 * (EMSGSIZE: longer than HOLD_CONSOLE_MAX_REQUEST). */
int hold_console_request(const char *sock_path, const char *request, size_t request_len,
                         unsigned char **reply, size_t *reply_len);

/* Whether backmux's fallback is selected: BACKMUX_CONSOLE_TRANSPORT=fifo, or Unix
 * sockets are denied (some sandboxes refuse them). Only then do processes carry
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
    HOLD_CLIENT_OUTPUT_ENDED, /* the source closed (or the followed process ended) */
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
    int (*on_command)(void *ctx, unsigned char key); /* the lead key, then a command key */
    /* the keys read after a command that stopped the loop (the rest of
     * that read): whoever takes over has them */
    void (*on_rest)(void *ctx, const unsigned char *bytes, size_t n);
    bool want_sigint;
    void *ctx;
};
enum hold_client_end hold_client_run(const struct hold_client *c);

/* Attach to a process console until it ends or the detach sequence is entered.
 * Pane, window, and overlay controls transfer display handling to mux.c.
 * The compositor can return to plain attach when only one pane remains
 * and no overlay is open. */
int hold_run_native_console(const char *sock_path);
/* Compositor context: the store for pane and chat recipient selection, and
 * the user's chat inbox name. NULL disables compositor controls. */
void hold_console_set_mux(const struct hold_store *store, const char *chat_name);
/* The console of the process this process runs inside (BACKMUX_ID), when it
 * has one: an attach started from here is nested in that process's terminal,
 * and says so to that process's broker for as long as it is attached. The
 * detach keys then let go of the innermost attach first - they reach the
 * outermost client, whose broker passes them inward instead of detaching -
 * and an attach left inside a terminal nobody is attached to any more lets
 * go by itself. "" or NULL: not inside a process. */
void hold_console_set_enclosing(const char *sock_path);
/* Whether the last hold_run_native_console was turned away because another
 * client has the console (*by its pid, 0 unknown). The function then returns
 * 3 and prints nothing itself: its caller knows the process's name. */
bool hold_console_was_refused(pid_t *by);
/* Whether it found no console to connect to: the process's output had already
 * ended. Returns 3 and prints nothing then, as above. */
bool hold_console_was_gone(void);
/* The person detached; names are the panes let go ("" for the process alone). */
bool hold_console_was_detached(const char **names);
void hold_console_note_detached(const char *name);
/* Whether the last attach received anything from the broker. */
bool hold_console_heard_output(void);

/* ---- agent harnesses (console/agents.c) ----------------------------------
 * Chat lists only agent harnesses; see agents.c for how one is told. */
#define AGENT_NAME_MAX 32
/* Names the person added with --agent (the config's "agents"), space-separated. */
void hold_agents_set_extra(const char *list);
/* Whose config to read that list from (and again every few seconds). */
void hold_agents_set_home(const char *home);
/* Whether a program name is a harness's; out gets the harness's name. */
bool hold_agent_name(const char *word, char *out, size_t n);
bool hold_agent_of_argv(char *const *argv, int argc, char *out, size_t n);
/* The name --agent records for a command: its program's, or its script's
 * when an interpreter runs it. */
bool hold_agent_word(char *const *argv, int argc, char *out, size_t n);
/* A running process, its terminal's foreground job, or a child of either. */
bool hold_agent_of_process(pid_t pid, char *out, size_t n);
/* A process record: the program it was started as, then (running) what runs now. */
bool hold_agent_of_record(const struct hold_run_record *r, char *out, size_t n);

/* The compositor takes the terminal from the start and keeps it - the bar,
 * chat open - instead of plain attach: backmux shell, the one launch meant
 * for a person rather than a script. */
void hold_console_set_resident(bool on);

#endif /* HOLD_CONSOLE_H */
