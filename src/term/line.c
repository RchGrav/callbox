#include "callbox/config.h"
#include "callbox/core.h"
#include "callbox/term.h"
#include "callbox/screen.h"

/* The line editing a terminal does in its normal mode, for a client that
 * must read keys itself (to see the detach keys and Ctrl-C) while the program
 * it types for is on pipes, not a PTY: echo, backspace, Ctrl-U, Enter, and
 * Ctrl-D for end of input. Nothing more - the program would not have line
 * history or arrow keys without a PTY either. */

void hold_term_line_init(struct hold_term_line *l, int echo_fd) {
    memset(l, 0, sizeof(*l));
    l->echo_fd = echo_fd;
}

static void echo(const struct hold_term_line *l, const char *s, size_t n) {
    if (l->echo_fd >= 0) (void)hold_write_all(l->echo_fd, s, n);
}

/* Erase the last character (a whole UTF-8 sequence) from the line and from
 * the screen - as many columns as it took (two for CJK, none for a
 * combining mark). */
static void erase_one(struct hold_term_line *l) {
    if (l->len == 0) return;
    size_t end = l->len;
    do {
        l->len--;
    } while (l->len > 0 && ((unsigned char)l->buf[l->len] & 0xc0) == 0x80);
    const unsigned char *u = (const unsigned char *)l->buf + l->len;
    size_t n = end - l->len;
    uint32_t cp = u[0];
    if (n == 2) cp = (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f);
    else if (n == 3) cp = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    else if (n >= 4) cp = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
    int cols = hold_char_width(cp);
    for (int i = 0; i < cols; i++) echo(l, "\b \b", 3);
}

int hold_term_line_feed(struct hold_term_line *l, unsigned char c, hold_term_detach_sink sink, void *ctx,
                        bool *eof) {
    *eof = false;
    /* Arrow and function keys arrive as escape sequences (ESC [ ... final,
     * or ESC O x): a line has no use for them, and none of their bytes
     * belong in it. */
    if (l->esc == 1) {
        l->esc = (c == '[' || c == 'O') ? 2 : 0;
        if (c == 'O') l->esc = 3;
        return 0;
    }
    if (l->esc == 2) {
        if (c >= 0x40 && c <= 0x7e) l->esc = 0;
        return 0;
    }
    if (l->esc == 3) {
        l->esc = 0;
        return 0;
    }
    if (c == 0x1b) {
        l->esc = 1;
        return 0;
    }
    /* Pasted text with CRLF line ends: the LF after a CR is the same Enter,
     * not an empty line after it. */
    bool cr_before = l->after_cr;
    l->after_cr = c == '\r';
    if (c == '\n' && cr_before) return 0;
    switch (c) {
    case '\r':
    case '\n': {
        /* "\n" alone: the client leaves output processing on, so the
         * terminal adds the carriage return itself ("\r\n" would be
         * "\r\r\n"). */
        echo(l, "\n", 1);
        if (l->len < sizeof(l->buf)) l->buf[l->len++] = '\n';
        size_t n = l->len;
        l->len = 0;
        return sink(ctx, (const unsigned char *)l->buf, n) != 0 ? -1 : 0;
    }
    case 0x04: /* Ctrl-D: end of input on an empty line, else send what is typed */
        if (l->len == 0) {
            *eof = true;
            return 0;
        } else {
            size_t n = l->len;
            l->len = 0;
            return sink(ctx, (const unsigned char *)l->buf, n) != 0 ? -1 : 0;
        }
    case 0x7f:
    case 0x08: /* backspace */
        erase_one(l);
        return 0;
    case 0x15: /* Ctrl-U: the whole line */
        while (l->len > 0) erase_one(l);
        return 0;
    default:
        break;
    }
    if (c < 0x20 && c != '\t') return 0; /* other controls mean nothing to a line */
    if (l->len >= sizeof(l->buf) - 1) {
        /* A line longer than the buffer goes in pieces, as a terminal's
         * normal mode does past its own limit. */
        size_t n = l->len;
        l->len = 0;
        if (sink(ctx, (const unsigned char *)l->buf, n) != 0) return -1;
    }
    l->buf[l->len++] = (char)c;
    echo(l, (const char *)&c, 1);
    return 0;
}

void hold_term_line_discard(struct hold_term_line *l) {
    l->len = 0;
}
