#pragma once
#ifndef HOLD_CONSOLE_INTERNAL_H
#define HOLD_CONSOLE_INTERNAL_H

/* Console-private wire constants and the socket seam, shared by the broker
 * (serve side) and attach (client side) translation units. Not part of the
 * public console.h surface. */
#include "backmux/config.h"

/* Wire protocol: a client that opens with the magic speaks 3-byte
 * type + be16-length frames; anything else is raw passthrough (decision D-5
 * keeps nc/socat attaches working). */
#define CONSOLE_ATTACH_MAGIC "holdv1\0\0"
#define CONSOLE_ATTACH_MAGIC_LEN 8
#define CONSOLE_FRAME_DATA 'D'
#define CONSOLE_FRAME_RESIZE 'W'
#define CONSOLE_FRAME_DETACH 'X'
/* End of input (Ctrl-D, or piped input ran out): a program on pipes reads
 * EOF. On a PTY (sent by a launch whose script input ran out), the broker
 * types the terminal's end-of-input character, its echo kept out of the
 * output. */
#define CONSOLE_FRAME_EOF 'E'
/* Interrupt (Ctrl-C) for a program on pipes, where no PTY turns the key
 * into a signal: payload empty; the broker sends SIGINT to its group. With
 * a one-byte payload (SIGINT or SIGTERM), that signal, for either kind of
 * program: how a launch passes on a SIGTERM it was sent. */
#define CONSOLE_FRAME_INTERRUPT 'S'
/* The client is not a terminal (a script, a pipe): send it the output as a
 * stream - this run's output so far, then what follows - instead of a screen
 * repaint full of cursor moves. Sent first, right after the magic. */
#define CONSOLE_FRAME_STREAM 'P'
/* No terminal on the client's side (its stdin or stdout is not one), so
 * nothing there will answer the program's terminal queries (cursor
 * position, device attributes): the broker's screen answers them, as it
 * does with nobody attached. Without it a program that waits for a reply
 * stalls under `backmux <cmd>` from a script. Payload empty. */
#define CONSOLE_FRAME_NO_TERMINAL 'N'
#define CONSOLE_FRAME_HEADER_LEN 3

/* Control clients (backmux peek/send/wait/resize) open with this magic, then
 * send one be32-length-prefixed text request; the broker answers with one
 * be32-length-prefixed reply ("ok ...\n" | "error ...\n" | "timeout ...\n",
 * optionally followed by a binary body) and closes. Requests:
 *   peek [alt=1]                 -> "ok\n" + HSNAP1 snapshot (alt=1: the last
 *                                   alternate-screen frame the program left)
 *   send\n<op>\n...              -> "ok seq=N [mouse=off]\n"
 *        ops: text <hex> | paste <hex> | key <name> | mouse <action> <btn> <row> <col>
 *   resize rows=R cols=C         -> "ok rows=R cols=C\n"
 *   wait [idle=MS] [since=SEQ] [match=<hex>] [regex=1] [exit=1] [timeout=MS]
 *                                -> "ok reason=idle|match|exit seq=N [exit_code=C]\n"
 *                                 | "ended reason=exit ..." | "timeout seq=N\n"
 *   nested keys=<hex>            -> no reply; the connection stays open. Sent by
 *                                   an attach client running in this call's
 *                                   terminal, for as long as it is attached:
 *                                   keys are its detach keys (see attach.c)
 *   msg [from=<hex>] [submit=0]\n<hex text>
 *                                -> "ok delivered verified=0|1 id=<6 hex>\n" once
 *                                   typed in (a backmux shell: once in its chat
 *                                   inbox, never typed)
 *   signal sig=N                 -> "ok\n"; N is TERM, KILL, INT or HUP, sent to
 *                                   the program's process group
 * Errors are "error <why>\n". docs/screen.md has the full table. */
#define CONSOLE_CONTROL_MAGIC "holdctl1"
#define CONSOLE_CONTROL_MAGIC_LEN 8

static inline uint16_t console_load_be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t console_load_be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline void console_store_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline void console_store_be16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/* socket.c: the with_console_dir seam (bind/connect via a short relative
 * sun_path from inside the console directory) and peer-uid authn. */
int hold_make_console_listener(const char *sock_path, int *fifo_writer);
/* fifo.c: the console over FIFOs where Unix sockets are denied. A channel
 * is a read fd (returned) and a write fd (*output; the same fd for a socket). */
int hold_make_fifo_listener(const char *path, int *writer);
int hold_open_channel(const char *path, char kind, int *output);

/* The person's terminal across a hand-off between plain attach and the
 * compositor (attach.c): who has it raw and on the alternate screen, and how
 * to give it back. */
#include "backmux/term.h"
struct hold_console_term {
    bool owned;
    bool alt_screen;
    struct hold_term_saved saved;
};
extern struct hold_console_term g_console_term;
void hold_console_term_release(void);

/* The compositor (mux.c): panes of TTY calls and one floating window over
 * them, drawn into the person's terminal. Starts on first_sock's call, as if
 * the lead key then command had been pressed there. Returns how it ended;
 * *handed_back with the console to show plainly when one pane is left and
 * nothing floats. */
int hold_mux_run(const struct hold_store *store, const char *chat_name, const char *first_sock,
                 unsigned char command, char *handback, size_t n, bool *handed_back);
/* After an attach was refused with EBUSY over FIFOs: the pid that has the
 * console, 0 if the broker did not say. */
pid_t hold_fifo_busy_holder(void);
int hold_fifo_accept(int listener, const char *path, char *kind, int *output, uid_t *peer_uid, pid_t *peer_pid);
int hold_console_peer_uid(int fd, uid_t *uid_out);
int hold_console_peer_pid(int fd, pid_t *pid_out);
int hold_connect_console_socket(const char *sock_path);

#endif /* HOLD_CONSOLE_INTERNAL_H */
