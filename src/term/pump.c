#include "backmux/config.h"
#include "backmux/core.h"
#include "backmux/term.h"

/* THE PTY-master pump. Every reader of a backmux-owned PTY master drains it
 * through this one step so the capture path (log first, then caller
 * fan-out) and the end-of-target semantics live in exactly one place.
 *
 * End-of-target: a PTY master reports the last slave closing either as EOF
 * or as read() failing with EIO (Linux). EIO is checked only when the read
 * actually failed — never against a stale errno from a successful read. */
ssize_t hold_term_pump_master_filtered(int master, int logfd, char *buf, size_t n,
                                       hold_term_pump_filter filter, void *ctx) {
    if (!filter) return hold_term_pump_master(master, logfd, buf, n);
    char raw[4096];
    size_t want = n > 4 ? n - 4 : 0;
    if (want > sizeof(raw)) want = sizeof(raw);
    ssize_t r = read(master, raw, want);
    if (r > 0) {
        size_t out = filter(ctx, raw, (size_t)r, buf);
        if (out == 0) {
            errno = EAGAIN;
            return -1;
        }
        (void)hold_write_all(logfd, buf, out);
        return (ssize_t)out;
    }
    if (r == 0 || (r < 0 && errno == EIO)) {
        return 0;
    }
    return -1;
}

ssize_t hold_term_pump_master(int master, int logfd, char *buf, size_t n) {
    ssize_t r = read(master, buf, n);
    if (r > 0) {
        (void)hold_write_all(logfd, buf, (size_t)r);
        return r;
    }
    if (r == 0 || (r < 0 && errno == EIO)) {
        return 0;
    }
    return -1;
}

/* ---- THE detach-key FSM (console attach) -----------------------------------
 *
 * pending is always a strict prefix of a chord, so feeding a byte either
 * extends the prefix (held until the next key), completes a chord, or
 * unwinds: the byte that broke the match is popped, the surviving prefix is
 * released through the sink, and the byte is re-fed against the empty state.
 */

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys) {
    memset(d, 0, sizeof(*d));
    if (!keys || nkeys == 0 || nkeys > HOLD_TERM_DETACH_MAX_KEYS) return;
    memcpy(d->keys, keys, nkeys);
    d->nkeys = nkeys;
}

/* Does pending match the first len bytes of this chord? Reports completion. */
static bool chord_prefix(const unsigned char *keys, size_t nkeys,
                         const unsigned char *pending, size_t len, bool *complete) {
    if (nkeys == 0 || len > nkeys || memcmp(keys, pending, len) != 0) return false;
    if (len == nkeys) *complete = true;
    return true;
}

void hold_term_detach_set_commands(struct hold_term_detach *d, const unsigned char *cmds, size_t n) {
    if (n > sizeof(d->commands)) n = sizeof(d->commands);
    memcpy(d->commands, cmds, n);
    d->ncommands = n;
}

void hold_term_detach_set_interrupt(struct hold_term_detach *d, unsigned char intr) {
    d->intr = intr;
}

int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx) {
    if (d->pending_len == 0) return 0;
    size_t n = d->pending_len;
    d->pending_len = 0;
    return sink(ctx, d->pending, n) != 0 ? -1 : 0;
}

/* The detach keys as a lead key plus the next key, held with no timer: not
 * everyone types fast, and Docker leaves its lead key untimed too. The lead
 * key twice is the quick way to send one through. */
int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, int *completed) {
    *completed = 0;
    /* While the lead key is held, an escape sequence goes through whole (a
     * mouse report is not the second key; moving the mouse must never
     * answer a key prompt), and the lead key goes on waiting. */
    if (d->esc_pass) {
        if (d->esc_pass == 1) d->esc_pass = c == '[' ? 2 : 0;
        else if (c >= 0x40 && c <= 0x7e) d->esc_pass = 0;
        return sink(ctx, &c, 1) != 0 ? -1 : 0;
    }
    if (d->ncommands && d->nkeys == 2 && d->pending_len == 1) {
        if (c == 0x1b) {
            d->esc_pass = 1;
            return sink(ctx, &c, 1) != 0 ? -1 : 0;
        }
        if (c != d->keys[1] && c != d->keys[0]) {
            d->pending_len = 0;
            if (memchr(d->commands, c, d->ncommands)) {
                d->command = c;
                *completed = 3;
            }
            /* not a command: cancelled, both keys dropped. Why: with a
             * handful of commands a wrong second key is almost always a
             * mistyped one, and passing it on would hit the program with a
             * stray prefix and a letter it never meant to get. */
            return 0;
        }
    }
    for (;;) {
        if (d->pending_len == 0 && d->intr && c == d->intr) {
            *completed = 2;
            return 0;
        }
        if (d->nkeys == 0) return sink(ctx, &c, 1) != 0 ? -1 : 0;
        if (d->pending_len == 0 && c != d->keys[0]) {
            return sink(ctx, &c, 1) != 0 ? -1 : 0;
        }
        if (d->pending_len >= HOLD_TERM_DETACH_MAX_KEYS) {
            if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
            continue;
        }
        d->pending[d->pending_len++] = c;
        bool done = false;
        bool live = chord_prefix(d->keys, d->nkeys, d->pending, d->pending_len, &done);
        if (done) {
            d->pending_len = 0;
            *completed = 1;
            return 0;
        }
        if (live) return 0; /* held until the next key, whenever it comes */
        d->pending_len--;
        /* The lead key twice: one of them is the program's. */
        if (d->pending_len == 1 && c == d->keys[0]) {
            d->pending_len = 0;
            return sink(ctx, &c, 1) != 0 ? -1 : 0;
        }
        if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
        /* re-feed c against the now-empty pending state */
    }
}
