#pragma once
#ifndef HOLD_CONSOLE_OVERLAY_H
#define HOLD_CONSOLE_OVERLAY_H

/* A backmux shell's display, private to the console layer.
 *
 * The broker keeps the one authoritative screen of the shell (the screen
 * layer, fed by the PTY). For a backmux shell, whoever attaches is not sent the
 * PTY's raw bytes: each frame is composed from that screen as the base layer
 * with the chat window drawn over it as a second pass, and only the
 * difference from what the terminal already shows is written (compose.c).
 * The chat window, its conversations, and the input from the attached
 * terminal are chat.c. Nothing drawn here ever reaches the screen, the log,
 * or the program. */

#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/screen.h"

/* ---- compose.c: frames ---------------------------------------------------- */

struct compositor;

/* A cell's look in the overlay: 256-color palette indexes, -1 = the
 * terminal's default. */
struct comp_style {
    uint8_t attrs; /* HOLD_ATTR_* */
    int fg, bg;
};

/* What the attached terminal should be told besides its cells. */
struct comp_want {
    bool app_cursor_keys, app_keypad, bracketed_paste, focus_report;
    uint8_t mouse; /* enum hold_mouse_mode; reports are always asked for in SGR form */
    bool cursor_visible;
    int cursor_row, cursor_col;
    int cursor_style; /* DECSCUSR number, 0 = the terminal's own */
    const char *title; /* NULL or "" = leave it */
    bool bell;
};

struct compositor *comp_new(void);
void comp_free(struct compositor *c);
/* The attached terminal's size; a change repaints everything. */
int comp_set_size(struct compositor *c, int rows, int cols);
void comp_size(const struct compositor *c, int *rows, int *cols);
/* Forget what the terminal shows and how it is set: the next frame clears
 * and repaints it and sends every mode (a terminal just attached). */
void comp_invalidate(struct compositor *c);
/* Start a frame with the shell's screen as the base layer, clipped or padded
 * to the terminal. */
void comp_begin(struct compositor *c, const struct hold_snapshot *base);
/* A pane: snap's top-left rows x cols drawn at (row, col), clipped. */
void comp_blit(struct compositor *c, const struct hold_snapshot *snap, int row, int col, int rows, int cols);
/* Draw over the base. Cells outside the terminal are dropped; a wide
 * character cut in half by what is drawn over it becomes a blank. */
void comp_put(struct compositor *c, int row, int col, uint32_t cp, const struct comp_style *st);
void comp_fill(struct compositor *c, int row, int col, int n, const struct comp_style *st);
/* UTF-8 text clipped to max_cols; returns the columns it took. */
int comp_text(struct compositor *c, int row, int col, int max_cols, const char *text, size_t len,
              const struct comp_style *st);
/* One codepoint of UTF-8 (malformed bytes read as U+FFFD, one at a time);
 * returns the bytes it took. len must be at least 1. */
size_t comp_utf8_next(const char *s, size_t len, uint32_t *cp);
/* End the frame: append to *buf the bytes that take the terminal from what
 * it shows to this frame. -1 on allocation failure. */
int comp_flush(struct compositor *c, const struct comp_want *want, char **buf, size_t *len, size_t *cap);

/* ---- chat.c: the chat window, its conversations, the terminal's input ----- */

struct chat;

/* What chat needs of the broker it lives in. */
struct chat_host {
    struct hold_screen *screen;     /* the shell's screen */
    int master;                     /* the PTY: where the shell's input goes */
    const struct hold_store *store; /* where this call and its peers are recorded */
    const char *run_id;             /* this call */
    /* The keys chat reads for itself, out of what the terminal sends: the
     * lead key, then the detach key or the chat key (0 = the defaults). */
    unsigned char lead, detach_key, key;
    /* Any program in user mode, not a shell: the hints name the program
     * ("backmux", "typing in the program") instead of the shell. */
    bool program;
    /* Hosted by the compositor (mux.c) instead of one call's broker: chat
     * belongs to the person, and floats over whatever panes are on screen.
     * person: their chat name - what they send goes out from it (as backmux
     * msg from outside any call does) and replies come to its inbox.
     * to_app: keys that are not chat's (the focused pane's). app_pointer: a
     * pointer report (SGR b;x;y, release) outside the window, for the
     * compositor to route. screen (above) is the focused pane's, for
     * Ctrl-S. */
    const char *person;
    void *app;
    void (*to_app)(void *app, const unsigned char *bytes, size_t n);
    void (*app_pointer)(void *app, int b, int x, int y, bool release);
};

#define CHAT_MAX_OUTBOUND 8

struct chat *chat_new(const struct chat_host *host);
void chat_free(struct chat *c);
/* A terminal attached (rows x cols as far as known) or went away. */
void chat_attached(struct chat *c, int rows, int cols, int64_t now);
void chat_detached(struct chat *c);
/* The attached terminal's window changed size. */
void chat_terminal_resized(struct chat *c, int rows, int cols);
/* Bytes typed (or reported) by the attached terminal: to the shell, or to
 * chat, by who has the keyboard and where the pointer is. */
void chat_input(struct chat *c, const unsigned char *buf, size_t n, int64_t now);
/* Did that input ask to detach (the lead key, then the detach key)? Reported
 * once: the broker lets go of the attach client, or of one nested in it. */
bool chat_take_detach(struct chat *c);
/* A message backmux msg delivered to this call: from the call sender_id
 * (label its name now), or, with sender_id NULL, from outside any call. */
void chat_inbound(struct chat *c, const char *sender_id, const char *label, bool operator, const char *text, size_t len);
/* Timers (a lone Esc, the corner hint, a send in flight) and the descriptors
 * of sends in flight. */
int64_t chat_due_in(const struct chat *c, int64_t now);
void chat_tick(struct chat *c, int64_t now);
int chat_poll_fds(const struct chat *c, struct pollfd *pfds, int max);
void chat_poll_done(struct chat *c, const struct pollfd *pfds, int n, int64_t now);
/* The shell's screen changed (output, or a resize of its PTY). */
void chat_screen_changed(struct chat *c);
bool chat_wants_frame(const struct chat *c);
/* Compose a frame for the attached terminal; bytes are appended to *buf. */
int chat_render(struct chat *c, int64_t now, char **buf, size_t *len, size_t *cap);

/* ---- hosted by the compositor ---------------------------------------------
 * The compositor composes the panes into comp, then chat draws its window
 * (or its corner badge) over them; want gets the cursor and modes chat
 * needs while it has the keyboard. */
void chat_draw(struct chat *c, struct compositor *comp, int64_t now, struct comp_want *want);
/* The terminal is ready for chat to draw on (rows x cols), with no tip. */
void chat_ready(struct chat *c, int rows, int cols, int64_t now);
/* The focused pane changed: its screen is what Ctrl-S sends. */
void chat_set_screen(struct chat *c, struct hold_screen *screen);
/* The chat key (Ctrl-P Ctrl-T): open chat, or hand the keyboard between
 * chat and the program. chat_open_on: open it on a call's conversation. */
void chat_toggle(struct chat *c);
void chat_open_on(struct chat *c, const char *call_id, const char *label);
void chat_put_away(struct chat *c);
bool chat_visible(const struct chat *c);
/* The person moved to a call's pane: its conversation on show, if chat is
 * up, without taking the keyboard. */
void chat_show_call(struct chat *c, const char *call_id, const char *label);
/* What the compositor routes by: chat has the keyboard; (row, col) is over
 * the window; a drag that started on the window is under way. */
bool chat_has_keyboard(const struct chat *c);
bool chat_covers(const struct chat *c, int row, int col);
bool chat_dragging(const struct chat *c);
/* A message sent from this chat, as it went out: for the conversation log. */
void chat_set_on_sent(struct chat *c, void (*fn)(void *app, const char *label, const char *text, size_t len));
/* History from the conversation log, as if it had just happened. */
void chat_replay(struct chat *c, bool mine, const char *key, const char *label, bool operator, time_t at,
                 const char *text, size_t len);

#endif /* HOLD_CONSOLE_OVERLAY_H */
