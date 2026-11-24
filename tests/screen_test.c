/* Unit tests for the screen layer (src/screen): the headless terminal, input
 * encoding by mode, HSNAP1 round trips, and repaint fidelity. */
#include "backmux/config.h"
#include "backmux/screen.h"
#include "../src/console/overlay.h"
#include "backmux/term.h"

static int failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static void feed(struct hold_screen *s, const char *bytes) {
    hold_screen_feed(s, bytes, strlen(bytes));
}

static void row_text(struct hold_screen *s, int row, char *out, size_t n) {
    struct hold_snapshot snap;
    if (hold_screen_snapshot(s, &snap) != 0) {
        out[0] = '\0';
        return;
    }
    hold_snapshot_row_text(&snap, row, out, n);
    hold_snapshot_free(&snap);
}

static size_t take(struct hold_screen *s, char *buf, size_t n) {
    size_t got = hold_screen_take_output(s, buf, n - 1);
    buf[got] = '\0';
    return got;
}

static void test_text_and_cursor(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    feed(s, "hello\r\n\033[1;31mred\033[0m wide:\xe4\xb8\xad!");
    char line[256];
    row_text(s, 0, line, sizeof(line));
    CHECK(!strcmp(line, "hello"), "row0 '%s'", line);
    row_text(s, 1, line, sizeof(line));
    CHECK(!strcmp(line, "red wide:\xe4\xb8\xad!"), "row1 '%s'", line);
    struct hold_snapshot snap;
    CHECK(hold_screen_snapshot(s, &snap) == 0, "snapshot");
    CHECK(snap.cursor_row == 1 && snap.cursor_col == 12, "cursor %u,%u", snap.cursor_row, snap.cursor_col);
    const struct hold_cell *r = &snap.cells[1 * 20 + 0];
    CHECK(r->ch[0] == 'r' && (r->attrs & HOLD_ATTR_BOLD) && !(r->flags & HOLD_CELL_FG_DEFAULT), "red cell attrs");
    CHECK(r->fg[0] > 150 && r->fg[1] < 100, "red fg %u,%u,%u", r->fg[0], r->fg[1], r->fg[2]);
    CHECK(snap.cells[1 * 20 + 9].width == 2 && snap.cells[1 * 20 + 10].width == 0, "wide char cells");
    hold_snapshot_free(&snap);
    hold_screen_free(s);
}

/* SGR 2 (faint) is tracked (a local libvterm patch): dim text such as shell
 * autosuggestions must stay distinguishable from typed text. SGR 22 clears
 * faint and bold together; SGR 0 clears it too. */
static void test_dim_attribute(void) {
    struct hold_screen *s = hold_screen_new(3, 20);
    feed(s, "a\033[2mb\033[22mc\033[1;2md\033[22me\033[2mf\033[0mg");
    struct hold_snapshot snap;
    CHECK(hold_screen_snapshot(s, &snap) == 0, "snapshot");
    const char *want = "-D-D-D"; /* a..f: dim on b, d, f */
    for (int i = 0; i < 6; i++) {
        bool dim = snap.cells[i].attrs & HOLD_ATTR_DIM;
        CHECK(dim == (want[i] == 'D'), "cell %c dim=%d", 'a' + i, dim);
    }
    CHECK(!(snap.cells[6].attrs & HOLD_ATTR_DIM), "SGR 0 clears dim");
    CHECK((snap.cells[3].attrs & HOLD_ATTR_BOLD) && !(snap.cells[4].attrs & HOLD_ATTR_BOLD), "SGR 22 clears bold too");
    hold_snapshot_free(&snap);
    hold_screen_free(s);
}

static void test_keys_follow_modes(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    char out[64];
    CHECK(hold_screen_send_key(s, "Up") == 0, "Up");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[A"), "normal Up '%s'", out);
    feed(s, "\033[?1h"); /* DECCKM */
    hold_screen_send_key(s, "Up");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033OA"), "app Up '%s'", out);
    hold_screen_send_key(s, "C-c");
    take(s, out, sizeof(out));
    CHECK(out[0] == 0x03 && out[1] == '\0', "C-c");
    hold_screen_send_key(s, "C-m");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\r"), "C-m is CR");
    hold_screen_send_key(s, "Enter");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\r"), "Enter is CR");
    hold_screen_send_key(s, "M-x");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033x"), "M-x");
    CHECK(hold_screen_send_key(s, "NoSuchKey") == -1, "unknown key rejected");
    hold_screen_send_key(s, "F5");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[15~"), "F5 '%s'", out);
    hold_screen_free(s);
}

static void test_paste_and_mouse(void) {
    struct hold_screen *s = hold_screen_new(10, 40);
    char out[128];
    hold_screen_send_paste(s, "a\nb", 3);
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "a\nb"), "paste without 2004 is raw");
    feed(s, "\033[?2004h");
    hold_screen_send_paste(s, "a\nb", 3);
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[200~a\nb\033[201~"), "bracketed paste '%s'", out);
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 5) == 1, "mouse off reports 1");
    feed(s, "\033[?1000h\033[?1006h");
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 5) == 0, "mouse on");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[<0;6;3M\033[<0;6;3m"), "SGR click '%s'", out);
    hold_screen_resize(s, 10, 300);
    feed(s, "\033[?1006l"); /* back to the legacy byte encoding */
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 100) == 0, "legacy encoding fits col 100");
    take(s, out, sizeof(out));
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 250) == 2, "legacy encoding past 222 reports clamping");
    take(s, out, sizeof(out));
    feed(s, "\033[?1006h");
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 250) == 0, "SGR reaches col 250");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[<0;251;3M\033[<0;251;3m"), "SGR col 250 '%s'", out);
    struct hold_screen_modes m;
    hold_screen_modes(s, &m);
    CHECK(m.mouse == HOLD_MOUSE_CLICK && m.mouse_sgr && m.bracketed_paste, "modes reported");
    hold_screen_free(s);
}

static void test_query_reply(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    feed(s, "ab\033[6n"); /* cursor position report */
    char out[64];
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[1;3R"), "CPR reply '%s'", out);
    hold_screen_free(s);
}

static void test_roundtrip_and_repaint(void) {
    struct hold_screen *s = hold_screen_new(6, 30);
    feed(s, "\033]0;my title\007\033[44;97m blue \033[0m plain\r\n\033[7minv\033[0m \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\033[3;5H");
    feed(s, "\033[?1h\033[?2004h");
    struct hold_snapshot a, b;
    CHECK(hold_screen_snapshot(s, &a) == 0, "snapshot a");
    a.meta.output_seq = 12345;
    a.meta.running = true;
    a.meta.has_mouse = true;
    a.meta.mouse_row = 2;
    a.meta.mouse_col = 7;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    CHECK(hold_snapshot_serialize(&a, &blob, &blob_len) == 0, "serialize");
    CHECK(hold_snapshot_parse(blob, blob_len, &b) == 0, "parse");
    CHECK(b.rows == 6 && b.cols == 30 && b.cursor_row == 2 && b.cursor_col == 4, "dims/cursor");
    CHECK(!strcmp(b.title, "my title"), "title '%s'", b.title);
    CHECK(b.meta.output_seq == 12345 && b.meta.running && b.meta.has_mouse && b.meta.mouse_col == 7, "meta");
    CHECK(b.modes.app_cursor_keys && b.modes.bracketed_paste, "modes");
    /* Every field HSNAP1 carries survives; the palette index does not travel
     * (a live screen knows it, a parsed snapshot has resolved RGB only). */
    bool wire_same = true, index_leaked = false;
    const uint8_t dflt = HOLD_CELL_FG_DEFAULT | HOLD_CELL_BG_DEFAULT;
    for (size_t i = 0; i < 6u * 30u; i++) {
        const struct hold_cell *x = &a.cells[i], *y = &b.cells[i];
        if (memcmp(x->ch, y->ch, sizeof(x->ch)) || x->width != y->width || x->attrs != y->attrs ||
            (x->flags & dflt) != (y->flags & dflt) || memcmp(x->fg, y->fg, 3) || memcmp(x->bg, y->bg, 3))
            wire_same = false;
        if ((y->flags & ~dflt) || y->fg_idx || y->bg_idx) index_leaked = true;
    }
    CHECK(wire_same, "cells identical");
    CHECK(!index_leaked, "a parsed snapshot carries no palette index");
    CHECK((a.cells[1].flags & HOLD_CELL_BG_INDEXED) && a.cells[1].bg_idx == 4 &&
          (a.cells[1].flags & HOLD_CELL_FG_INDEXED) && a.cells[1].fg_idx == 15,
          "a live snapshot knows the palette index (bg %u fg %u)", a.cells[1].bg_idx, a.cells[1].fg_idx);
    CHECK(!(a.cells[8].flags & (HOLD_CELL_FG_INDEXED | HOLD_CELL_BG_INDEXED)), "default colors are not indexed");
    hold_snapshot_free(&b);
    CHECK(hold_snapshot_parse(blob, blob_len - 1, &b) == -1, "truncated blob rejected");
    free(blob);

    char *rep = NULL;
    size_t len = 0, cap = 0;
    CHECK(hold_snapshot_repaint(&a, &rep, &len, &cap) == 0, "repaint");
    CHECK(len > 11 && !memcmp(rep, "\033[0m\033[H\033[2J", 11), "repaint starts with a full clear");
    struct hold_screen *t = hold_screen_new(6, 30);
    hold_screen_feed(t, rep, len);
    struct hold_snapshot c;
    CHECK(hold_screen_snapshot(t, &c) == 0, "snapshot c");
    bool same = true;
    for (size_t i = 0; i < 6u * 30u; i++) {
        const struct hold_cell *x = &a.cells[i], *y = &c.cells[i];
        if (x->ch[0] != y->ch[0] || x->attrs != y->attrs || memcmp(x->fg, y->fg, 3) || memcmp(x->bg, y->bg, 3)) {
            same = false;
            fprintf(stderr, "  cell %zu differs: %u/%u attrs %u/%u\n", i, x->ch[0], y->ch[0], x->attrs, y->attrs);
            break;
        }
    }
    CHECK(same, "repaint reproduces every cell");
    CHECK(c.cursor_row == 2 && c.cursor_col == 4, "repaint cursor %u,%u", c.cursor_row, c.cursor_col);
    CHECK(c.modes.app_cursor_keys && c.modes.bracketed_paste, "repaint restores modes");
    free(rep);
    hold_snapshot_free(&a);
    hold_snapshot_free(&c);
    hold_screen_free(s);
    hold_screen_free(t);
}

static void test_resize_and_text_ruler(void) {
    struct hold_screen *s = hold_screen_new(3, 12);
    feed(s, "abc");
    hold_screen_resize(s, 4, 15);
    int rows, cols;
    hold_screen_size(s, &rows, &cols);
    CHECK(rows == 4 && cols == 15, "resized %dx%d", rows, cols);
    struct hold_snapshot snap;
    hold_screen_snapshot(s, &snap);
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    hold_snapshot_write_text(&snap, f, true);
    fclose(f);
    CHECK(strstr(buf, "   0         1    \n   012345678901234\n 0|abc\n") == buf, "ruler text:\n%s", buf);
    free(buf);
    hold_snapshot_free(&snap);
    hold_screen_free(s);
}

/* ---- the compositor of a backmux shell's display (src/console/compose.c) ------ */

/* Is `term` showing exactly `want`? Characters, widths, attributes, colors. */
static bool shows(struct hold_screen *term, const struct hold_snapshot *want, const char *what) {
    struct hold_snapshot got;
    bool same = hold_screen_snapshot(term, &got) == 0 && got.rows == want->rows && got.cols == want->cols;
    const uint8_t dflt = HOLD_CELL_FG_DEFAULT | HOLD_CELL_BG_DEFAULT;
    for (size_t i = 0; same && i < (size_t)want->rows * want->cols; i++) {
        const struct hold_cell *x = &want->cells[i], *y = &got.cells[i];
        uint32_t xc = x->ch[0] ? x->ch[0] : ' ', yc = y->ch[0] ? y->ch[0] : ' ';
        if (x->width == 0 && y->width == 0) continue;
        if (xc != yc || x->ch[1] != y->ch[1] || x->width != y->width || x->attrs != y->attrs ||
            (x->flags & dflt) != (y->flags & dflt) ||
            (!(x->flags & HOLD_CELL_FG_DEFAULT) && memcmp(x->fg, y->fg, 3)) ||
            (!(x->flags & HOLD_CELL_BG_DEFAULT) && memcmp(x->bg, y->bg, 3))) {
            fprintf(stderr, "  %s: cell %zu,%zu differs: U+%04X/U+%04X width %u/%u attrs %u/%u\n", what,
                    i / want->cols, i % want->cols, xc, yc, x->width, y->width, x->attrs, y->attrs);
            same = false;
        }
    }
    hold_snapshot_free(&got);
    return same;
}

/* One frame: the shell's screen as the base, optionally a strip of overlay
 * over it; its bytes go to `term`. Returns how many bytes that took. */
static size_t frame(struct compositor *c, struct hold_screen *shell, struct hold_screen *term, bool overlay, int orow,
                    int ocol, const char *otext) {
    struct hold_snapshot base;
    static const struct comp_style st = {HOLD_ATTR_BOLD, 231, 25};
    hold_screen_snapshot(shell, &base);
    comp_begin(c, &base);
    if (overlay) comp_text(c, orow, ocol, 40, otext, strlen(otext), &st);
    struct comp_want want = {.cursor_visible = true, .cursor_row = base.cursor_row, .cursor_col = base.cursor_col};
    char *buf = NULL;
    size_t len = 0, cap = 0;
    CHECK(comp_flush(c, &want, &buf, &len, &cap) == 0, "flush");
    hold_screen_feed(term, buf, len);
    free(buf);
    hold_snapshot_free(&base);
    return len;
}

static void test_compositor_frames(void) {
    struct hold_screen *shell = hold_screen_new(6, 30), *term = hold_screen_new(6, 30);
    struct compositor *c = comp_new();
    CHECK(c && comp_set_size(c, 6, 30) == 0, "compositor");
    feed(shell, "\033[31mred\033[0m plain \033[1;4;44mbold on blue\033[0m\r\n"
                "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e wide e\xcc\x81\r\n"
                "\033[38;5;208morange\033[0m \033[38;2;1;2;3mrgb\033[0m \033[7minverse\033[0m\r\n$ ");
    struct hold_snapshot want;

    /* The first frame paints the terminal into the shell's screen. */
    size_t first = frame(c, shell, term, false, 0, 0, "");
    hold_screen_snapshot(shell, &want);
    CHECK(shows(term, &want, "first frame"), "the terminal shows the shell's screen");
    struct hold_snapshot t;
    hold_screen_snapshot(term, &t);
    CHECK(t.cursor_row == want.cursor_row && t.cursor_col == want.cursor_col, "the cursor is the shell's (%u,%u)",
          t.cursor_row, t.cursor_col);
    hold_snapshot_free(&t);
    hold_snapshot_free(&want);

    /* Nothing changed: nothing is written. One cell changed: a few bytes. */
    CHECK(frame(c, shell, term, false, 0, 0, "") == 0, "an unchanged frame writes nothing");
    feed(shell, "x");
    size_t one = frame(c, shell, term, false, 0, 0, "");
    CHECK(one > 0 && one < 64 && one < first, "one changed cell is a small update (%zu bytes, first %zu)", one, first);
    hold_screen_snapshot(shell, &want);
    CHECK(shows(term, &want, "one cell"), "the update lands");
    hold_snapshot_free(&want);

    /* An overlay strip over the wide characters, starting on the right half
     * of one and ending on the left half of another: each cut character is
     * blanked, the strip is drawn, and the shell's own screen is untouched. */
    frame(c, shell, term, true, 1, 1, "chat");
    hold_screen_snapshot(term, &t);
    char row[128];
    hold_snapshot_row_text(&t, 1, row, sizeof(row));
    CHECK(!strcmp(row, " chat  wide e\xcc\x81"), "overlay row '%s'", row);
    CHECK((t.cells[30 + 1].attrs & HOLD_ATTR_BOLD) && t.cells[30 + 1].bg_idx == 25, "the overlay has its own look");
    hold_snapshot_free(&t);
    hold_screen_snapshot(shell, &want);
    hold_snapshot_row_text(&want, 1, row, sizeof(row));
    CHECK(!strncmp(row, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e wide", 14), "the shell's screen never sees the overlay: '%s'", row);

    /* The shell keeps drawing under the overlay; when the overlay goes, the
     * cells it covered show what the shell has there now. */
    feed(shell, "\033[2;1HNEW TEXT UNDERNEATH");
    frame(c, shell, term, true, 1, 1, "chat");
    hold_screen_snapshot(term, &t);
    hold_snapshot_row_text(&t, 1, row, sizeof(row));
    CHECK(!strcmp(row, "NchatEXT UNDERNEATH"), "live under the overlay: '%s'", row);
    hold_snapshot_free(&t);
    frame(c, shell, term, false, 0, 0, "");
    hold_snapshot_free(&want);
    hold_screen_snapshot(shell, &want);
    CHECK(shows(term, &want, "uncovered"), "uncovered cells show the shell's current content");
    hold_snapshot_free(&want);

    /* A smaller terminal clips the base; a wide character cut by its edge is
     * a blank, never half a glyph. */
    struct hold_screen *narrow = hold_screen_new(3, 5);
    feed(shell, "\033[H\033[2J\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    CHECK(comp_set_size(c, 3, 5) == 0, "resize");
    frame(c, shell, narrow, false, 0, 0, "");
    hold_screen_snapshot(narrow, &t);
    hold_snapshot_row_text(&t, 0, row, sizeof(row));
    CHECK(!strcmp(row, "\xe6\x97\xa5\xe6\x9c\xac"), "clipped row '%s'", row);
    hold_snapshot_free(&t);
    hold_screen_free(narrow);
    comp_free(c);
    hold_screen_free(shell);
    hold_screen_free(term);
}

/* ---- the -i line editor --------------------------------------------------- */

/* line.c echoes through core's hold_write_all; core's util.c pulls in much
 * more, so the test gives it this one function (same contract). */
int hold_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

struct line_sink {
    char got[256];
    size_t len;
    int sends;
};

static int line_sink_fn(void *ctx, const unsigned char *b, size_t n) {
    struct line_sink *s = ctx;
    if (s->len + n < sizeof(s->got)) {
        memcpy(s->got + s->len, b, n);
        s->len += n;
        s->got[s->len] = '\0';
    }
    s->sends++;
    return 0;
}

static void line_type(struct hold_term_line *l, struct line_sink *s, const char *keys, bool *eof) {
    for (const char *k = keys; *k; k++) hold_term_line_feed(l, (unsigned char)*k, line_sink_fn, s, eof);
}

static void test_line_editor(void) {
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    struct hold_term_line l;
    hold_term_line_init(&l, p[1]);
    struct line_sink s = {0};
    bool eof = false;
    /* backspace and Ctrl-U edit; Enter sends the line with a newline */
    line_type(&l, &s, "abx\x7f" "c\r", &eof);
    CHECK(!strcmp(s.got, "abc\n") && s.sends == 1, "line '%s' sends %d", s.got, s.sends);
    line_type(&l, &s, "zz\x15ok\n", &eof);
    CHECK(!strcmp(s.got, "abc\nok\n"), "after Ctrl-U '%s'", s.got);
    /* pasted CRLF is one Enter, not an empty line after it */
    s.len = 0; s.got[0] = '\0'; s.sends = 0;
    line_type(&l, &s, "one\r\ntwo\r\n", &eof);
    CHECK(!strcmp(s.got, "one\ntwo\n") && s.sends == 2, "CRLF paste '%s' sends %d", s.got, s.sends);
    /* arrow keys leave nothing in the line */
    s.len = 0; s.got[0] = '\0';
    line_type(&l, &s, "a\x1b[Ab\n", &eof);
    CHECK(!strcmp(s.got, "ab\n"), "arrow key '%s'", s.got);
    /* Ctrl-D: sends what is typed, then on an empty line is the end */
    s.len = 0; s.got[0] = '\0';
    line_type(&l, &s, "x\x04", &eof);
    CHECK(!strcmp(s.got, "x") && !eof, "Ctrl-D with text '%s'", s.got);
    line_type(&l, &s, "\x04", &eof);
    CHECK(eof, "Ctrl-D on an empty line is end of input");
    /* the echo: Enter is "\n" alone (output processing adds the CR) */
    close(p[1]);
    char echo[512];
    ssize_t n = read(p[0], echo, sizeof(echo) - 1);
    close(p[0]);
    echo[n > 0 ? n : 0] = '\0';
    CHECK(strstr(echo, "\r\n") == NULL, "echo has no CR before LF");
    CHECK(strstr(echo, "abx\b \bc\n") != NULL, "echo of the first line");
}

int main(void) {
    test_text_and_cursor();
    test_dim_attribute();
    test_keys_follow_modes();
    test_paste_and_mouse();
    test_query_reply();
    test_roundtrip_and_repaint();
    test_resize_and_text_ruler();
    test_compositor_frames();
    test_line_editor();
    if (failures) {
        fprintf(stderr, "screen-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("screen-test: all passed\n");
    return 0;
}
