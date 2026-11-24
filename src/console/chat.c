#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/screen.h"
#include "overlay.h"

/* A backmux shell's chat: the floating window, its conversations, and the
 * routing of everything the attached terminal sends.
 *
 * Conversations are kept per sender: a message from a call goes to that
 * call's conversation (keyed by its id, so a rename keeps it), and messages
 * from outside any call share one. What the person sends goes out over the ordinary backmux msg transport,
 * from this call, so an agent answers with a plain `backmux msg`.
 *
 * Nothing here writes to the shell except the person's own keystrokes and
 * the mouse events the shell's program asked for: an incoming message is
 * only ever shown. */

#define CHAT_MAX_THREADS 24
#define CHAT_MAX_MSGS 300 /* per conversation; the oldest fall off */
#define CHAT_MAX_INPUT (64 * 1024)
#define CHAT_MAX_INBOUND (256 * 1024)
#define CHAT_MIN_W 28
#define CHAT_MIN_H 6
#define CHAT_INPUT_ROWS 3
#define CHAT_ESC_MS 40        /* an Esc with nothing after it is the Esc key */
#define CHAT_HINT_MS 6000     /* the corner hint after a terminal attaches */
#define CHAT_NOTICE_MS 5000
#define CHAT_SEND_LIMIT_MS 90000
#define CHAT_PICK_REFRESH_MS 1000
#define CHAT_MOUSE_TAIL_MS 300 /* reports still in flight after reporting is turned off */
#define CHAT_MAX_PICK 64
/* A conversation's key (the call's id) and its title. */
#define CHAT_KEY_LEN (ALIAS_MAX_LEN + ID_STR_LEN + 1)
#define CHAT_LABEL_LEN (ALIAS_MAX_LEN + ID_STR_LEN + 16)

/* TH_OPERATOR: an agent outside any call; its replies go to its inbox under
 * that name (store/inbox.c), not to a broker. */
enum thread_kind { TH_CALL, TH_OPERATOR, TH_OUTSIDE };
enum msg_state { MSG_IN, MSG_SENDING, MSG_SENT, MSG_FAILED };

struct chat_msg {
    enum msg_state state;
    time_t at;
    char *text;
    size_t len;
    uint32_t serial;
    bool with_screen;
    char note[128]; /* why it failed */
    int wrap_width; /* text_lines is for this width; 0 = not counted yet */
    int text_lines;
};

struct chat_thread {
    enum thread_kind kind;
    char key[CHAT_KEY_LEN];
    char label[CHAT_LABEL_LEN];
    struct chat_msg *msgs;
    int count, cap;
    int unread;
    int scroll; /* lines up from the newest */
    int64_t active_ms;
};

struct pick_entry {
    enum thread_kind kind;
    char key[CHAT_KEY_LEN];
    char label[CHAT_LABEL_LEN];
    bool running;
    int unread;
};

struct outbound {
    pid_t pid; /* 0 = free slot */
    int fd;    /* -1 once the result is in */
    struct chat_thread *thread;
    uint32_t serial;
    char result[256];
    size_t len;
    int64_t started_ms;
};

enum grab { GRAB_NONE, GRAB_APP, GRAB_MOVE, GRAB_RESIZE, GRAB_CLOSE, GRAB_HELD };

struct chat {
    struct chat_host host;
    /* This call's name, as agents see it and reply to it: the label on your
     * own messages. Looked up when the window opens (the record is written
     * after the broker starts, and a rename changes it). */
    char self[CHAT_LABEL_LEN];
    struct compositor *comp;
    bool attached;
    int trows, tcols; /* the attached terminal */

    bool visible, focused, placed;
    int ux, uy, uw, uh; /* the window where the person put it, in terminal cells */
    int x, y, w, h;     /* ... and as it fits on the terminal right now */

    struct chat_thread *threads[CHAT_MAX_THREADS];
    int nthreads;
    struct chat_thread *current;

    char *input; /* the message being written, UTF-8 */
    size_t in_len, in_cap, in_cur;
    bool include_screen;

    bool picking; /* choosing who to talk to */
    char filter[ALIAS_MAX_LEN + 1];
    size_t filter_len;
    int pick_sel;
    struct pick_entry picks[CHAT_MAX_PICK];
    int npicks;
    int pick_first; /* the first entry the last frame listed */
    int64_t picks_ms;

    enum grab grab;
    int grab_dx, grab_dy;

    unsigned char pend[64]; /* an escape sequence or character still arriving */
    size_t pend_len;
    int64_t pend_ms;
    bool pasting;
    bool lead_pending; /* the lead key was pressed; the next key says what it meant */
    bool want_detach;
    int64_t input_ms;
    bool mouse_on; /* the terminal was asked to report the mouse */
    int64_t mouse_off_ms;

    char notice[160]; /* a line in the window: why something did not happen, or a tip */
    bool notice_tip;
    int64_t notice_until_ms;
    char hint[CHAT_LABEL_LEN + 64]; /* a tip in the corner while the window is hidden; holds the call's name */
    int64_t hint_until_ms;

    struct outbound out[CHAT_MAX_OUTBOUND];
    uint32_t serial;

    bool dirty, screen_dirty;
    int cur_row, cur_col; /* where the chat's own cursor was last drawn */
    int badge_col;        /* the corner badge's first column, -1 = none shown */
    int body_rows;        /* of the last frame, for paging */
    int64_t now;
};

/* ---- looks ----------------------------------------------------------------- */

static const struct comp_style ST_TITLE = {HOLD_ATTR_BOLD, 231, 25};
static const struct comp_style ST_TITLE_IDLE = {0, 252, 240};
static const struct comp_style ST_BODY = {0, 252, 235};
static const struct comp_style ST_DIM = {0, 245, 235};
static const struct comp_style ST_YOU = {HOLD_ATTR_BOLD, 110, 235};
static const struct comp_style ST_PEER = {HOLD_ATTR_BOLD, 150, 235};
static const struct comp_style ST_BAD = {HOLD_ATTR_BOLD, 210, 235};
static const struct comp_style ST_INPUT = {0, 255, 238};
static const struct comp_style ST_INPUT_HINT = {0, 246, 238};
static const struct comp_style ST_SEL = {HOLD_ATTR_BOLD, 231, 24};
static const struct comp_style ST_BADGE = {HOLD_ATTR_BOLD, 231, 25};

/* ---- small helpers ---------------------------------------------------------- */

static void set_notice(struct chat *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->notice, sizeof(c->notice), fmt, ap);
    va_end(ap);
    c->notice_tip = false;
    c->notice_until_ms = c->now + CHAT_NOTICE_MS;
    c->dirty = true;
}

/* A control key as a person reads it: 0x10 -> "Ctrl-P". */
static void key_name(unsigned char key, char out[12]) {
    snprintf(out, 12, "Ctrl-%c", key | 0x40);
}

/* The two keys that reach chat: "Ctrl-P Ctrl-T". */
static void chat_keys(const struct chat *c, char out[24]) {
    char lead[12], key[12];
    key_name(c->host.lead, lead);
    key_name(c->host.key, key);
    snprintf(out, 24, "%s %s", lead, key);
}

/* On-screen help, shown at the moment it is useful and gone a few seconds
 * later: what just happened, and the key that undoes it. `what` goes before
 * the key's name, `then` after it. In the corner while the window is hidden;
 * as a line in the window while it is up. Like everything drawn here, it
 * never reaches the shell, its screen, or its log. */
static void set_tip(struct chat *c, const char *what, const char *then) {
    char key[24];
    chat_keys(c, key);
    if (c->visible) {
        /* the key first: in a narrow window the end of the line is cut off */
        snprintf(c->notice, sizeof(c->notice), "%s: %s | %s", key, then, what);
        c->notice_tip = true;
        c->notice_until_ms = c->now + CHAT_NOTICE_MS;
    } else {
        snprintf(c->hint, sizeof(c->hint), " %s | %s: %s ", what, key, then);
        c->hint_until_ms = c->now + CHAT_HINT_MS;
    }
    c->dirty = true;
}

static void to_shell(struct chat *c, const unsigned char *p, size_t n) {
    if (n > 0 && c->host.master >= 0) (void)hold_write_all(c->host.master, p, n);
}

/* Input the screen layer encoded for the program (a forwarded mouse event). */
static void flush_screen_input(struct chat *c) {
    char buf[512];
    size_t n;
    while ((n = hold_screen_take_output(c->host.screen, buf, sizeof(buf))) > 0)
        if (c->host.master >= 0) (void)hold_write_all(c->host.master, buf, n);
}

static int total_unread(const struct chat *c, const struct chat_thread *except) {
    int n = 0;
    for (int i = 0; i < c->nthreads; i++)
        if (c->threads[i] != except) n += c->threads[i]->unread;
    return n;
}

/* ---- conversations ----------------------------------------------------------- */

static void free_thread(struct chat_thread *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) free(t->msgs[i].text);
    free(t->msgs);
    free(t);
}

static bool thread_busy(const struct chat *c, const struct chat_thread *t) {
    for (int i = 0; i < CHAT_MAX_OUTBOUND; i++)
        if (c->out[i].pid && c->out[i].thread == t) return true;
    return false;
}

static struct chat_thread *find_thread(const struct chat *c, enum thread_kind kind, const char *key) {
    for (int i = 0; i < c->nthreads; i++)
        if (c->threads[i]->kind == kind && !strcmp(c->threads[i]->key, key)) return c->threads[i];
    return NULL;
}

static struct chat_thread *get_thread(struct chat *c, enum thread_kind kind, const char *key, const char *label) {
    struct chat_thread *t = find_thread(c, kind, key);
    if (!t) {
        if (c->nthreads == CHAT_MAX_THREADS) {
            /* Full: the conversation left alone longest makes room. */
            int oldest = -1;
            for (int i = 0; i < c->nthreads; i++) {
                struct chat_thread *o = c->threads[i];
                if (o == c->current || thread_busy(c, o)) continue;
                if (oldest < 0 || o->active_ms < c->threads[oldest]->active_ms) oldest = i;
            }
            if (oldest < 0) return NULL;
            free_thread(c->threads[oldest]);
            c->threads[oldest] = c->threads[--c->nthreads];
        }
        t = calloc(1, sizeof(*t));
        if (!t) return NULL;
        t->kind = kind;
        snprintf(t->key, sizeof(t->key), "%s", key);
        c->threads[c->nthreads++] = t;
    }
    if (label && *label) snprintf(t->label, sizeof(t->label), "%s", label);
    t->active_ms = c->now;
    return t;
}

static struct chat_msg *add_msg(struct chat_thread *t, enum msg_state state, const char *text, size_t len) {
    if (t->count == CHAT_MAX_MSGS) {
        free(t->msgs[0].text);
        memmove(t->msgs, t->msgs + 1, (size_t)(t->count - 1) * sizeof(*t->msgs));
        t->count--;
    }
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        struct chat_msg *p = realloc(t->msgs, (size_t)cap * sizeof(*p));
        if (!p) return NULL;
        t->msgs = p;
        t->cap = cap;
    }
    struct chat_msg *m = &t->msgs[t->count];
    memset(m, 0, sizeof(*m));
    m->text = malloc(len + 1);
    if (!m->text) return NULL;
    memcpy(m->text, text, len);
    m->text[len] = '\0';
    m->len = len;
    m->state = state;
    m->at = time(NULL);
    t->count++;
    return m;
}

static struct chat_msg *find_msg(struct chat_thread *t, uint32_t serial) {
    for (int i = t->count - 1; i >= 0; i--)
        if (t->msgs[i].serial == serial && t->msgs[i].state != MSG_IN) return &t->msgs[i];
    return NULL;
}

/* ---- text layout -------------------------------------------------------------- */

/* The next display line of text, from *pos (0 to begin): it breaks at a
 * newline, at the last space that fits, or mid-word when one word is wider
 * than the line. Returns false once every line has been given; a text that
 * ends in a newline (or is empty) has one last, empty line. */
#define WRAP_DONE SIZE_MAX
static bool wrap_next(const char *text, size_t len, int width, bool words, size_t *pos, size_t *start, size_t *end) {
    if (*pos == WRAP_DONE || width < 1) return false;
    size_t i = *pos, last_space = SIZE_MAX;
    int cols = 0;
    *start = i;
    while (i < len) {
        uint32_t cp;
        size_t n = comp_utf8_next(text + i, len - i, &cp);
        if (cp == '\n') {
            *end = i;
            *pos = i + n;
            return true;
        }
        int w = cp < 0x20 ? 1 : hold_char_width(cp);
        if (cols + w > width && i > *start) {
            if (words && last_space != SIZE_MAX && last_space > *start) {
                *end = last_space;
                *pos = last_space + 1;
            } else {
                *end = i;
                *pos = i;
            }
            return true;
        }
        if (cp == ' ') last_space = i;
        cols += w;
        i += n;
    }
    *end = len;
    *pos = WRAP_DONE;
    return true;
}

static int count_lines(const char *text, size_t len, int width, bool words) {
    size_t pos = 0, s, e;
    int n = 0;
    while (wrap_next(text, len, width, words, &pos, &s, &e)) n++;
    return n;
}

/* A message in the conversation: a header line, its text, and a note when a
 * screen went with it. */
/* A message's text never changes, so its wrapped line count is kept for the
 * width it was counted at: a frame does not re-wrap the whole history. */
static int msg_text_lines(struct chat_msg *m, int width) {
    if (m->wrap_width != width) {
        m->text_lines = count_lines(m->text, m->len, width - 1, true);
        m->wrap_width = width;
    }
    return m->text_lines;
}

static int msg_lines(struct chat_msg *m, int width) {
    return 1 + msg_text_lines(m, width) + (m->with_screen ? 1 : 0) +
           (m->state == MSG_FAILED ? count_lines(m->note, strlen(m->note), width - 1, true) : 0);
}

/* ---- geometry ------------------------------------------------------------------ */

/* The window as it fits on the terminal: where and as large as the person
 * left it when there is room, smaller and nudged inward when there is not
 * (and back again when the terminal grows). */
static void refit(struct chat *c) {
    int minw = CHAT_MIN_W < c->tcols ? CHAT_MIN_W : c->tcols, minh = CHAT_MIN_H < c->trows ? CHAT_MIN_H : c->trows;
    c->x = c->ux;
    c->y = c->uy;
    c->w = c->uw;
    c->h = c->uh;
    if (c->w > c->tcols) c->w = c->tcols;
    if (c->h > c->trows) c->h = c->trows;
    if (c->w < minw) c->w = minw;
    if (c->h < minh) c->h = minh;
    /* The whole window stays on the terminal, so its title bar and close
     * control are always within reach. */
    if (c->x > c->tcols - c->w) c->x = c->tcols - c->w;
    if (c->y > c->trows - c->h) c->y = c->trows - c->h;
    if (c->x < 0) c->x = 0;
    if (c->y < 0) c->y = 0;
}

static void place(struct chat *c) {
    if (!c->placed) {
        c->uw = c->tcols - 2 < 50 ? c->tcols - 2 : 50;
        c->uh = c->trows - 2 < 16 ? c->trows - 2 : 16;
        c->ux = c->tcols - c->uw - 1;
        c->uy = 1;
        c->placed = true;
    }
    refit(c);
}

struct layout {
    int body_top, body_rows;
    int rule;   /* -1 = no room */
    int input_top, input_rows;
    int footer; /* -1 = no room */
};

static void layout(const struct chat *c, int input_lines, struct layout *L) {
    memset(L, 0, sizeof(*L));
    L->rule = L->footer = -1;
    if (c->h >= CHAT_MIN_H) {
        int k = input_lines < 1 ? 1 : input_lines > CHAT_INPUT_ROWS ? CHAT_INPUT_ROWS : input_lines;
        while (k > 1 && c->h - 3 - k < 2) k--; /* the conversation keeps two rows */
        L->footer = c->y + c->h - 1;
        L->input_rows = k;
        L->input_top = L->footer - k;
        L->rule = L->input_top - 1;
        L->body_top = c->y + 1;
        L->body_rows = L->rule - L->body_top;
    } else {
        /* A terminal too small for the whole window: the title, the message
         * line, and what fits between. */
        L->input_rows = c->h >= 2 ? 1 : 0;
        L->input_top = c->y + c->h - 1;
        L->body_top = c->y + 1;
        L->body_rows = c->h - 1 - L->input_rows;
    }
    if (L->body_rows < 0) L->body_rows = 0;
}

static bool inside(const struct chat *c, int row, int col) {
    return row >= c->y && row < c->y + c->h && col >= c->x && col < c->x + c->w;
}

/* ---- the message being written -------------------------------------------------- */

struct input_view {
    size_t starts[512];
    int lines;
    int cur_line, cur_col;
};

/* The draft, wrapped at exactly `width` columns (no word wrap: the cursor
 * must sit where the next character will land). */
static void view_input(const struct chat *c, int width, struct input_view *v) {
    v->lines = 1;
    v->starts[0] = 0;
    v->cur_line = v->cur_col = 0;
    int col = 0;
    if (width < 1) width = 1;
    for (size_t i = 0; i <= c->in_len;) {
        if (i == c->in_cur) {
            v->cur_line = v->lines - 1;
            v->cur_col = col;
        }
        if (i == c->in_len) break;
        uint32_t cp;
        size_t n = comp_utf8_next(c->input + i, c->in_len - i, &cp);
        if (cp == '\n') {
            i += n;
            if (v->lines < (int)(sizeof(v->starts) / sizeof(v->starts[0]))) v->starts[v->lines++] = i;
            col = 0;
            continue;
        }
        int w = cp < 0x20 ? 1 : hold_char_width(cp);
        if (col + w > width && col > 0) {
            if (v->lines < (int)(sizeof(v->starts) / sizeof(v->starts[0]))) v->starts[v->lines++] = i;
            col = 0;
            if (i == c->in_cur) {
                v->cur_line = v->lines - 1;
                v->cur_col = 0;
            }
        }
        col += w;
        i += n;
    }
}

static bool input_reserve(struct chat *c, size_t more) {
    if (c->in_len + more > CHAT_MAX_INPUT) return false;
    if (c->in_len + more + 1 > c->in_cap) {
        size_t cap = c->in_cap ? c->in_cap : 256;
        while (cap < c->in_len + more + 1) cap *= 2;
        char *p = realloc(c->input, cap);
        if (!p) return false;
        c->input = p;
        c->in_cap = cap;
    }
    return true;
}

static void input_insert(struct chat *c, const char *s, size_t n) {
    if (n == 0 || !input_reserve(c, n)) return;
    memmove(c->input + c->in_cur + n, c->input + c->in_cur, c->in_len - c->in_cur);
    memcpy(c->input + c->in_cur, s, n);
    c->in_len += n;
    c->in_cur += n;
    c->input[c->in_len] = '\0';
    c->dirty = true;
}

static size_t prev_char(const char *s, size_t at) {
    if (at == 0) return 0;
    at--;
    while (at > 0 && ((unsigned char)s[at] & 0xc0) == 0x80) at--;
    return at;
}

static size_t next_char(const char *s, size_t len, size_t at) {
    if (at >= len) return len;
    at++;
    while (at < len && ((unsigned char)s[at] & 0xc0) == 0x80) at++;
    return at;
}

static void input_delete(struct chat *c, size_t from, size_t to) {
    if (from >= to || to > c->in_len) return;
    memmove(c->input + from, c->input + to, c->in_len - to);
    c->in_len -= to - from;
    c->input[c->in_len] = '\0';
    c->in_cur = from;
    c->dirty = true;
}

/* ---- who to talk to --------------------------------------------------------------- */

struct pick_scan {
    struct chat *c;
};

static int pick_record(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)path;
    struct chat *c = ((struct pick_scan *)vctx)->c;
    if (!r || c->npicks >= CHAT_MAX_PICK || !strcmp(id, c->host.run_id)) return 0;
    /* A call that can be messaged: on a PTY, not known to have ended, its
     * console still listening. */
    if (!r->has_console || r->has_exit_code || !r->has_state || strcmp(r->state, "running") != 0 ||
        !hold_path_exists(r->console_sock))
        return 0;
    struct pick_entry *e = &c->picks[c->npicks++];
    memset(e, 0, sizeof(*e));
    e->kind = TH_CALL;
    snprintf(e->key, sizeof(e->key), "%s", id);
    if (r->has_name && r->name[0]) snprintf(e->label, sizeof(e->label), "%s", r->name);
    else snprintf(e->label, sizeof(e->label), "%.12s", id);
    e->running = true;
    return 0;
}

/* Calls that can be messaged first, then the conversations that can only be
 * read; by name within each. */
static int pick_order(const void *a, const void *b) {
    const struct pick_entry *x = a, *y = b;
    if (x->running != y->running) return x->running ? -1 : 1;
    if (x->kind != y->kind) return (int)x->kind - (int)y->kind;
    return strcmp(x->label, y->label);
}

static void refresh_picks(struct chat *c) {
    c->npicks = 0;
    c->picks_ms = c->now;
    struct pick_scan scan = {c};
    if (c->host.store) (void)hold_for_each_record(c->host.store->record_dir, pick_record, &scan);
    /* Every conversation stays readable: one with a call that has since
     * ended, and those backmux could not tie to a call. */
    for (int i = 0; i < c->nthreads; i++) {
        struct chat_thread *t = c->threads[i];
        struct pick_entry *e = NULL;
        for (int k = 0; k < c->npicks && !e; k++)
            if (c->picks[k].kind == t->kind && !strcmp(c->picks[k].key, t->key)) e = &c->picks[k];
        if (!e && c->npicks < CHAT_MAX_PICK) {
            e = &c->picks[c->npicks++];
            memset(e, 0, sizeof(*e));
            e->kind = t->kind;
            memcpy(e->key, t->key, sizeof(e->key));
            memcpy(e->label, t->label, sizeof(e->label));
        }
        if (e) e->unread = t->unread;
    }
    qsort(c->picks, (size_t)c->npicks, sizeof(c->picks[0]), pick_order);
}

static bool pick_matches(const struct chat *c, const struct pick_entry *e) {
    if (c->filter_len == 0) return true;
    if (e->kind == TH_CALL && !strncmp(e->key, c->filter, c->filter_len)) return true;
    for (const char *p = e->label; *p; p++)
        if (!strncasecmp(p, c->filter, c->filter_len)) return true;
    return false;
}

/* The n-th entry the filter lets through, or NULL. */
static const struct pick_entry *pick_at(const struct chat *c, int n) {
    for (int i = 0; i < c->npicks; i++)
        if (pick_matches(c, &c->picks[i]) && n-- == 0) return &c->picks[i];
    return NULL;
}

static int pick_count(const struct chat *c) {
    int n = 0;
    for (int i = 0; i < c->npicks; i++)
        if (pick_matches(c, &c->picks[i])) n++;
    return n;
}

static void show_thread(struct chat *c, struct chat_thread *t) {
    c->current = t;
    c->picking = false;
    if (t) {
        t->unread = 0;
        t->scroll = 0;
    }
    c->dirty = true;
}

static void start_picking(struct chat *c) {
    c->picking = true;
    c->filter_len = 0;
    c->filter[0] = '\0';
    c->pick_sel = 0;
    refresh_picks(c);
    c->dirty = true;
}

static void pick_confirm(struct chat *c) {
    const struct pick_entry *e = pick_at(c, c->pick_sel);
    if (!e) {
        if (c->filter_len) set_notice(c, "no running call matches '%s'", c->filter);
        else set_notice(c, "no other PTY call is running");
        return;
    }
    struct chat_thread *t = get_thread(c, e->kind, e->key, e->label);
    if (!t) {
        set_notice(c, "too many conversations are open");
        return;
    }
    show_thread(c, t);
}

/* ---- sending ------------------------------------------------------------------------ */

static void hex_into(char *out, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[(unsigned char)s[i] >> 4];
        out[2 * i + 1] = hx[(unsigned char)s[i] & 15];
    }
}

/* The helper process of one send. backmux msg's transport is a blocking
 * request (the receiver answers once the text is typed into its prompt; a
 * busy console is retried for up to 10 s), so it runs beside the broker,
 * which must keep the shell's display alive. It names this call as the
 * sender (self_id), so the reply comes back to this chat. */
static _Noreturn void send_helper(const char *sock, const char *self_id, const char *body, size_t body_len, int wfd) {
    for (int fd = 3; fd < 1024; fd++)
        if (fd != wfd) close(fd);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    char head[160 + 2 * ID_STR_LEN];
    int hn = snprintf(head, sizeof(head), "msg from=");
    size_t idl = strlen(self_id);
    hex_into(head + hn, self_id, idl);
    hn += (int)(2 * idl);
    head[hn++] = '\n';
    size_t req_len = (size_t)hn + 2 * body_len;
    char *req = malloc(req_len + 1);
    char line[sizeof(((struct outbound *)0)->result)];
    snprintf(line, sizeof(line), "error out of memory\n");
    if (req) {
        memcpy(req, head, (size_t)hn);
        hex_into(req + hn, body, body_len);
        long delay_ms = 20, waited_ms = 0;
        for (;;) {
            unsigned char *reply = NULL;
            size_t reply_len = 0;
            bool again = false;
            if (hold_console_request(sock, req, req_len, &reply, &reply_len) != 0) {
                if (errno == ECONNREFUSED && waited_ms < 10000) again = true; /* its accept queue is full */
                else if (errno == ENOENT) snprintf(line, sizeof(line), "error it has ended\n");
                else if (errno == EMSGSIZE) snprintf(line, sizeof(line), "error the message is too large to send\n");
                else snprintf(line, sizeof(line), "error its console cannot be reached: %s\n", strerror(errno));
            } else if (!strcmp((const char *)reply, "error busy\n") && waited_ms < 10000) {
                again = true;
            } else {
                snprintf(line, sizeof(line), "%.*s\n", (int)strcspn((const char *)reply, "\n"), (const char *)reply);
            }
            free(reply);
            if (!again) break;
            struct timespec sl = {.tv_sec = delay_ms / 1000, .tv_nsec = (delay_ms % 1000) * 1000000L};
            nanosleep(&sl, NULL);
            waited_ms += delay_ms;
            delay_ms = delay_ms * 2 > 500 ? 500 : delay_ms * 2;
        }
    }
    (void)hold_write_all(wfd, line, strlen(line));
    _exit(0);
}

static void self_label(const struct chat *c, char *out, size_t n) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    snprintf(out, n, "%.12s", c->host.run_id);
    if (c->host.store && hold_load_record_by_id(c->host.store->record_dir, c->host.run_id, &r, path, sizeof(path)) == 0) {
        if (r.has_name && r.name[0]) snprintf(out, n, "%s", r.name);
        hold_free_run_record(&r);
    }
}

/* text, then the shell's clean screen as plain rows: what backmux peek prints,
 * so never the chat window. */
static char *with_screen(const struct chat *c, const char *text, size_t len, size_t *out_len) {
    struct hold_snapshot snap;
    if (hold_screen_snapshot(c->host.screen, &snap) != 0) return NULL;
    size_t row_cap = (size_t)snap.cols * 4 * HOLD_CELL_CHARS + 2;
    size_t cap = len + 256 + (size_t)snap.rows * row_cap;
    char *out = malloc(cap), name[ALIAS_MAX_LEN + 1];
    if (!out) {
        hold_snapshot_free(&snap);
        return NULL;
    }
    self_label(c, name, sizeof(name));
    memcpy(out, text, len);
    size_t n = len;
    n += (size_t)snprintf(out + n, cap - n, "\n\n[screen of %s: %u columns x %u rows, cursor at column %u, row %u]\n", name,
                          snap.cols, snap.rows, snap.cursor_col, snap.cursor_row);
    int last = snap.rows - 1;
    char *line = malloc(row_cap);
    while (line && last >= 0) {
        hold_snapshot_row_text(&snap, last, line, row_cap);
        if (line[0]) break;
        last--;
    }
    for (int r = 0; line && r <= last; r++) {
        hold_snapshot_row_text(&snap, r, line, row_cap);
        n += (size_t)snprintf(out + n, cap - n, "%s\n", line);
    }
    free(line);
    hold_snapshot_free(&snap);
    while (n > 0 && out[n - 1] == '\n') n--;
    *out_len = n;
    return out;
}

static void fail_msg(struct chat *c, struct chat_msg *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->note, sizeof(m->note), fmt, ap);
    va_end(ap);
    m->state = MSG_FAILED;
    c->dirty = true;
}

static void send_message(struct chat *c) {
    struct chat_thread *t = c->current;
    size_t len = c->in_len;
    while (len > 0 && (c->input[len - 1] == ' ' || c->input[len - 1] == '\n')) len--;
    if (len == 0) return;
    if (!t) {
        start_picking(c);
        return;
    }
    if (t->kind == TH_OUTSIDE) {
        set_notice(c, "sent from outside backmux: no call to answer");
        return;
    }
    struct chat_msg *m = add_msg(t, MSG_SENDING, c->input, len);
    if (!m) {
        set_notice(c, "out of memory");
        return;
    }
    m->serial = ++c->serial;
    m->with_screen = c->include_screen;
    t->active_ms = c->now;
    t->scroll = 0;
    c->dirty = true;

    /* The body before the draft is cleared; a failure from here on is shown
     * on the message itself, never as delivered. */
    size_t body_len = len;
    char *body = m->with_screen ? with_screen(c, c->input, len, &body_len) : NULL;
    bool screen_failed = m->with_screen && !body;
    c->in_len = c->in_cur = 0;
    if (c->input) c->input[0] = '\0';
    c->include_screen = false;
    if (screen_failed) {
        fail_msg(c, m, "the screen could not be captured");
        return;
    }

    if (t->kind == TH_OPERATOR) {
        /* No broker to ask: the reply goes straight into its inbox. */
        char self[ALIAS_MAX_LEN + 1];
        self_label(c, self, sizeof(self));
        int rc = c->host.store ? hold_inbox_deliver(c->host.store, t->key, self, body ? body : m->text,
                                                     body ? body_len : m->len)
                               : -1;
        free(body);
        if (rc != 0) fail_msg(c, m, "%s has no inbox now (it expires an hour after its last use)", t->label);
        else {
            m->state = MSG_SENT;
            c->dirty = true;
        }
        return;
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX], sock[HOLD_PATH_MAX] = "";
    if (!c->host.store || hold_load_record_by_id(c->host.store->record_dir, t->key, &r, path, sizeof(path)) != 0) {
        fail_msg(c, m, "%s is gone (its call was removed)", t->label);
        free(body);
        return;
    }
    if (r.has_name && r.name[0]) snprintf(t->label, sizeof(t->label), "%s", r.name);
    bool reachable = r.has_console && !r.has_exit_code && hold_path_exists(r.console_sock);
    if (reachable) snprintf(sock, sizeof(sock), "%s", r.console_sock);
    hold_free_run_record(&r);
    if (!reachable) {
        fail_msg(c, m, "%s is not running", t->label);
        free(body);
        return;
    }
    struct outbound *o = NULL;
    for (int i = 0; i < CHAT_MAX_OUTBOUND && !o; i++)
        if (!c->out[i].pid) o = &c->out[i];
    int fds[2] = {-1, -1};
    if (!o) {
        fail_msg(c, m, "too many messages are still on their way; send it again");
        free(body);
        return;
    }
    if (pipe(fds) != 0) {
        fail_msg(c, m, "could not start the send: %s", strerror(errno));
        free(body);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        send_helper(sock, c->host.run_id, body ? body : m->text, body_len, fds[1]);
    }
    close(fds[1]);
    free(body);
    if (pid < 0) {
        close(fds[0]);
        fail_msg(c, m, "could not start the send: %s", strerror(errno));
        return;
    }
    (void)fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    memset(o, 0, sizeof(*o));
    o->pid = pid;
    o->fd = fds[0];
    o->thread = t;
    o->serial = m->serial;
    o->started_ms = c->now;
}

/* The helper's one line: "ok delivered ..." or "error <why>". Anything else,
 * or nothing, is a failure too. */
static void finish_send(struct chat *c, struct outbound *o, const char *forced) {
    struct chat_msg *m = find_msg(o->thread, o->serial);
    if (o->fd >= 0) close(o->fd);
    o->fd = -1;
    o->result[o->len] = '\0';
    if (m && m->state == MSG_SENDING) {
        char *nl = strchr(o->result, '\n');
        if (nl) *nl = '\0';
        if (forced) fail_msg(c, m, "%s", forced);
        else if (!strncmp(o->result, "ok", 2)) m->state = MSG_SENT;
        else if (!strncmp(o->result, "error ", 6)) fail_msg(c, m, "%s", o->result + 6);
        else fail_msg(c, m, "%s", o->result[0] ? o->result : "the send was interrupted");
        c->dirty = true;
    }
}

static void reap_sends(struct chat *c) {
    for (int i = 0; i < CHAT_MAX_OUTBOUND; i++) {
        struct outbound *o = &c->out[i];
        if (!o->pid) continue;
        if (o->fd >= 0 && c->now - o->started_ms > CHAT_SEND_LIMIT_MS) {
            kill(o->pid, SIGKILL);
            finish_send(c, o, "no answer from its console in time");
        }
        if (o->fd >= 0) continue;
        int st;
        pid_t got = waitpid(o->pid, &st, WNOHANG);
        if (got == o->pid || (got < 0 && errno == ECHILD)) o->pid = 0;
    }
}

int chat_poll_fds(const struct chat *c, struct pollfd *pfds, int max) {
    int n = 0;
    for (int i = 0; i < CHAT_MAX_OUTBOUND && n < max; i++)
        if (c->out[i].pid && c->out[i].fd >= 0) pfds[n++] = (struct pollfd){.fd = c->out[i].fd, .events = POLLIN};
    return n;
}

void chat_poll_done(struct chat *c, const struct pollfd *pfds, int n, int64_t now) {
    c->now = now;
    for (int k = 0; k < n; k++) {
        if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
        for (int i = 0; i < CHAT_MAX_OUTBOUND; i++) {
            struct outbound *o = &c->out[i];
            if (!o->pid || o->fd != pfds[k].fd) continue;
            ssize_t got = read(o->fd, o->result + o->len, sizeof(o->result) - 1 - o->len);
            if (got > 0) o->len += (size_t)got;
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR) || o->len == sizeof(o->result) - 1)
                finish_send(c, o, NULL);
        }
    }
    reap_sends(c);
}

/* ---- incoming ------------------------------------------------------------------------- */

void chat_inbound(struct chat *c, const char *sender_id, const char *label, bool operator, const char *text, size_t len) {
    struct chat_thread *t = sender_id && *sender_id
                                ? get_thread(c, operator ? TH_OPERATOR : TH_CALL, sender_id, label ? label : sender_id)
                                : get_thread(c, TH_OUTSIDE, "", "outside backmux");
    if (!t) return; /* every conversation is in use: backmux msg --received still has it */
    if (len > CHAT_MAX_INBOUND) len = CHAT_MAX_INBOUND;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ')) len--;
    struct chat_msg *m = add_msg(t, MSG_IN, text, len);
    if (!m) return;
    bool seen = c->attached && c->visible && !c->picking && c->current == t;
    if (!seen) t->unread++;
    else if (t->scroll > 0) t->scroll += msg_lines(m, c->w - 2); /* keep the lines being read in place */
    c->dirty = true;
}

/* ---- keys, when chat has the keyboard ------------------------------------------------- */

static void hide(struct chat *c) {
    c->visible = c->focused = false;
    c->picking = false;
    c->notice[0] = '\0';
    set_tip(c, "chat hidden", "bring it back");
}

/* Who the keyboard is for, while the window is up. Handing it to the shell
 * says so in the window: the title bar dims, but that alone is easy to miss,
 * and typing into the wrong one is the mistake this window invites. */
static void set_focus(struct chat *c, bool chat) {
    if (c->focused == chat) return;
    c->focused = chat;
    if (chat) {
        if (c->notice_tip) c->notice[0] = '\0';
        if (c->current && !c->picking) c->current->unread = 0;
    } else if (c->visible) {
        set_tip(c, c->host.program ? "typing in the program" : "typing in the shell", "chat");
    }
    c->dirty = true;
}

static void open_chat(struct chat *c) {
    c->visible = c->focused = true;
    self_label(c, c->self, sizeof(c->self));
    place(c);
    if (c->current) show_thread(c, c->current);
    else {
        /* Straight to the conversation that has something new, if any. */
        struct chat_thread *unread = NULL;
        for (int i = 0; i < c->nthreads && !unread; i++)
            if (c->threads[i]->unread) unread = c->threads[i];
        if (unread) show_thread(c, unread);
        else start_picking(c);
    }
    c->dirty = true;
}

/* The chat key: open chat, or hand the keyboard between chat and the shell. */
static void chat_key(struct chat *c) {
    if (!c->visible) open_chat(c);
    else set_focus(c, !c->focused);
    c->dirty = true;
}

static void scroll_by(struct chat *c, int lines) {
    struct chat_thread *t = c->current;
    if (!t || c->picking) return;
    t->scroll += lines;
    if (t->scroll < 0) t->scroll = 0;
    c->dirty = true; /* the frame clamps it to the conversation's length */
}

enum edit_key { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE, K_PGUP, K_PGDN };

static void edit_key(struct chat *c, enum edit_key k) {
    if (c->picking) {
        int n = pick_count(c);
        if (k == K_UP && c->pick_sel > 0) c->pick_sel--;
        if (k == K_DOWN && c->pick_sel + 1 < n) c->pick_sel++;
        c->dirty = true;
        return;
    }
    switch (k) {
    case K_UP: scroll_by(c, 1); break;
    case K_DOWN: scroll_by(c, -1); break;
    case K_PGUP: scroll_by(c, c->body_rows > 1 ? c->body_rows - 1 : 1); break;
    case K_PGDN: scroll_by(c, -(c->body_rows > 1 ? c->body_rows - 1 : 1)); break;
    case K_LEFT: c->in_cur = prev_char(c->input, c->in_cur); break;
    case K_RIGHT: c->in_cur = next_char(c->input, c->in_len, c->in_cur); break;
    case K_HOME: c->in_cur = 0; break;
    case K_END: c->in_cur = c->in_len; break;
    case K_DELETE: input_delete(c, c->in_cur, next_char(c->input, c->in_len, c->in_cur)); break;
    }
    c->dirty = true;
}

static void chat_char(struct chat *c, uint32_t cp) {
    char u[4];
    size_t n = hold_utf8_encode(cp, u, sizeof(u));
    if (c->picking) {
        if (cp < 0x80 && c->filter_len + 1 < sizeof(c->filter)) {
            c->filter[c->filter_len++] = (char)cp;
            c->filter[c->filter_len] = '\0';
            c->pick_sel = 0;
            c->dirty = true;
        }
        return;
    }
    input_insert(c, u, n);
}

static void chat_ctrl(struct chat *c, unsigned char b) {
    c->dirty = true;
    if (c->picking) {
        switch (b) {
        case '\r': case '\n': pick_confirm(c); break;
        case '\t':
            if (c->current) c->picking = false;
            break;
        case 0x7f: case 0x08:
            if (c->filter_len) c->filter[--c->filter_len] = '\0';
            c->pick_sel = 0;
            break;
        case 0x15:
            c->filter_len = 0;
            c->filter[0] = '\0';
            c->pick_sel = 0;
            break;
        default: break;
        }
        return;
    }
    switch (b) {
    case '\r': send_message(c); break;
    case '\n': input_insert(c, "\n", 1); break;
    case '\t': start_picking(c); break;
    case 0x7f: case 0x08: input_delete(c, prev_char(c->input, c->in_cur), c->in_cur); break;
    case 0x01: c->in_cur = 0; break;                      /* Ctrl-A */
    case 0x05: c->in_cur = c->in_len; break;              /* Ctrl-E */
    case 0x15: input_delete(c, 0, c->in_cur); break;      /* Ctrl-U */
    case 0x0b: input_delete(c, c->in_cur, c->in_len); break; /* Ctrl-K */
    case 0x13: c->include_screen = !c->include_screen; break; /* Ctrl-S */
    case 0x17: {                                           /* Ctrl-W */
        size_t at = c->in_cur;
        while (at > 0 && c->input[at - 1] == ' ') at--;
        while (at > 0 && c->input[at - 1] != ' ' && c->input[at - 1] != '\n') at--;
        input_delete(c, at, c->in_cur);
        break;
    }
    default: break;
    }
}

/* ---- the mouse ---------------------------------------------------------------------------- */

static void app_mouse(struct chat *c, enum hold_mouse_event ev, int button, int row, int col, int mods) {
    /* The screen layer reports only what the program's mouse mode asks for:
     * with reporting off, the program hears nothing. */
    hold_screen_mouse_event(c->host.screen, ev, button, row, col, mods);
    flush_screen_input(c);
}

static bool on_close(const struct chat *c, int row, int col) {
    return row == c->y && col >= c->x + c->w - 3 && col < c->x + c->w;
}

static void window_press(struct chat *c, int row, int col) {
    struct layout L;
    struct input_view v;
    view_input(c, c->w - 4, &v);
    layout(c, v.lines, &L);
    set_focus(c, true);
    c->grab = GRAB_HELD;
    if (row == c->y) {
        if (on_close(c, row, col)) c->grab = GRAB_CLOSE;
        else {
            c->grab = GRAB_MOVE;
            c->grab_dx = col - c->x;
            c->grab_dy = row - c->y;
        }
    } else if (row == c->y + c->h - 1 && col >= c->x + c->w - 2) {
        c->grab = GRAB_RESIZE;
    } else if (row == L.footer && col >= c->x + 1 && col <= c->x + 10) {
        c->include_screen = !c->include_screen;
    } else if (c->picking && row > L.body_top && row < L.body_top + L.body_rows) {
        int n = c->pick_first + row - L.body_top - 1;
        if (n < pick_count(c)) {
            c->pick_sel = n;
            pick_confirm(c);
        }
    }
    c->dirty = true;
}

static void chat_mouse(struct chat *c, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1, button = (b & 3) + 1, mods = (b >> 2) & 7;
    bool wheel = (b & 64) != 0, motion = (b & 32) != 0;
    enum hold_mouse_event ev = wheel ? ((b & 1) ? HOLD_MOUSE_WHEEL_DOWN : HOLD_MOUSE_WHEEL_UP)
                                     : motion ? HOLD_MOUSE_MOTION : release ? HOLD_MOUSE_RELEASE : HOLD_MOUSE_PRESS;
    bool over = c->visible && inside(c, row, col);

    /* A gesture belongs to whoever it started on until the button is let go,
     * wherever the pointer wanders in between. */
    if (c->grab == GRAB_APP) {
        app_mouse(c, ev, button, row, col, mods);
        if (ev == HOLD_MOUSE_RELEASE) c->grab = GRAB_NONE;
        return;
    }
    if (c->grab != GRAB_NONE) {
        if (ev == HOLD_MOUSE_MOTION && (c->grab == GRAB_MOVE || c->grab == GRAB_RESIZE)) {
            /* The person is placing the window: where it lands (within the
             * terminal) is where it now belongs, at the size it has. */
            c->uw = c->w;
            c->uh = c->h;
            if (c->grab == GRAB_MOVE) {
                c->ux = col - c->grab_dx;
                c->uy = row - c->grab_dy;
            } else {
                c->ux = c->x;
                c->uy = c->y;
                c->uw = col - c->x + 1;
                c->uh = row - c->y + 1;
            }
            refit(c);
            c->ux = c->x;
            c->uy = c->y;
            c->uw = c->w;
            c->uh = c->h;
            c->dirty = true;
        } else if (ev == HOLD_MOUSE_RELEASE) {
            if (c->grab == GRAB_CLOSE && on_close(c, row, col)) hide(c);
            c->grab = GRAB_NONE;
        }
        return;
    }
    if (ev == HOLD_MOUSE_PRESS) {
        if (over) {
            window_press(c, row, col);
        } else if (!c->visible && c->badge_col >= 0 && row == 0 && col >= c->badge_col) {
            open_chat(c);
            c->grab = GRAB_HELD;
        } else {
            /* Outside the window: the keyboard goes back to the shell, and the
             * press is the program's (if it listens for the mouse). */
            set_focus(c, false);
            c->grab = GRAB_APP;
            app_mouse(c, ev, button, row, col, mods);
        }
    } else if (wheel) {
        if (over) scroll_by(c, ev == HOLD_MOUSE_WHEEL_UP ? 3 : -3);
        else app_mouse(c, ev, button, row, col, mods);
    } else if (!over) {
        app_mouse(c, ev, button, row, col, mods);
    }
}

/* ---- what the attached terminal sends ----------------------------------------------------- */

static void csi(struct chat *c, const unsigned char *p, size_t n) {
    unsigned char final = p[n - 1];
    if (n >= 6 && p[2] == '<' && (final == 'M' || final == 'm')) {
        int b = 0, x = 0, y = 0;
        char tmp[48];
        size_t k = n - 4 < sizeof(tmp) - 1 ? n - 4 : sizeof(tmp) - 1;
        memcpy(tmp, p + 3, k);
        tmp[k] = '\0';
        if (sscanf(tmp, "%d;%d;%d", &b, &x, &y) == 3) chat_mouse(c, b, x, y, final == 'm');
        return;
    }
    if (!c->focused) {
        to_shell(c, p, n);
        return;
    }
    int num = n > 3 && isdigit(p[2]) ? atoi((const char *)p + 2) : 0;
    switch (final) {
    case 'A': edit_key(c, K_UP); break;
    case 'B': edit_key(c, K_DOWN); break;
    case 'C': edit_key(c, K_RIGHT); break;
    case 'D': edit_key(c, K_LEFT); break;
    case 'H': edit_key(c, K_HOME); break;
    case 'F': edit_key(c, K_END); break;
    case '~':
        if (num == 1 || num == 7) edit_key(c, K_HOME);
        else if (num == 4 || num == 8) edit_key(c, K_END);
        else if (num == 3) edit_key(c, K_DELETE);
        else if (num == 5) edit_key(c, K_PGUP);
        else if (num == 6) edit_key(c, K_PGDN);
        else if (num == 200) c->pasting = true;
        break;
    default: break; /* focus reports and keys chat has no use for */
    }
}

/* Text pasted into chat: as written, newlines kept, nothing interpreted. */
static void paste_text(struct chat *c, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = (char)p[i];
        if (ch == '\r') {
            if (i + 1 < n && p[i + 1] == '\n') continue;
            ch = '\n';
        }
        if (ch == '\t') ch = ' ';
        if ((unsigned char)ch < 0x20 && ch != '\n') continue;
        if (ch == 0x7f) continue;
        if (c->picking) {
            if (ch != '\n') chat_char(c, (unsigned char)ch);
        } else {
            input_insert(c, &ch, 1);
        }
    }
}

static size_t consume_paste(struct chat *c, const unsigned char *p, size_t n) {
    static const char end[] = "\033[201~";
    const unsigned char *hit = memmem(p, n, end, 6);
    if (hit) {
        paste_text(c, p, (size_t)(hit - p));
        c->pasting = false;
        return (size_t)(hit - p) + 6;
    }
    /* Keep back a tail that may be the start of the end marker. */
    size_t keep = n < 5 ? n : 5;
    while (keep > 0 && memcmp(p + n - keep, end, keep) != 0) keep--;
    paste_text(c, p, n - keep);
    return n - keep; /* 0: only a possible marker so far - wait for more */
}

/* The lead key was not followed by one of backmux's keys: it was the
 * program's all along. It goes through (to the shell; chat has no use for
 * it) ahead of whatever was typed after it. */
static void lead_was_typed(struct chat *c) {
    if (!c->lead_pending) return;
    c->lead_pending = false;
    c->dirty = true;
    if (!c->focused) to_shell(c, &c->host.lead, 1);
}

/* One token of input. Returns the bytes it took; 0 = incomplete, wait. */
static size_t consume(struct chat *c, const unsigned char *p, size_t n) {
    if (c->pasting) return consume_paste(c, p, n);
    unsigned char b = p[0];
    /* The lead key waits for the next key, however long that takes: there is
     * no timer, so there is time to read what it offers (the menu is on
     * screen while it waits). Then: the detach key detaches, the chat key
     * opens chat or hands the keyboard over, the lead key again sends one
     * lead key through, and any other key sends both through. A mouse report
     * is not an answer; it is dealt with and the lead key goes on waiting. */
    if (c->lead_pending && b != 0x1b) {
        if (b == c->host.detach_key || b == c->host.key || b == c->host.lead) {
            c->lead_pending = false;
            c->dirty = true;
            if (b == c->host.detach_key) c->want_detach = true;
            else if (b == c->host.key) chat_key(c);
            else if (!c->focused) to_shell(c, &b, 1);
            return 1;
        }
        lead_was_typed(c);
    } else if (b == c->host.lead) {
        c->lead_pending = true;
        c->dirty = true;
        return 1;
    }
    /* With the shell at the keyboard and no mouse reports to pick out, its
     * bytes pass as they are, unparsed and undelayed. */
    bool parse = c->focused || c->mouse_on || c->now - c->mouse_off_ms < CHAT_MOUSE_TAIL_MS;
    if (!parse) {
        size_t k = 1;
        lead_was_typed(c);
        while (k < n && p[k] != c->host.lead) k++;
        to_shell(c, p, k);
        return k;
    }
    if (b == 0x1b) {
        if (n < 2) return 0;
        if (p[1] == '[') {
            size_t k = 2;
            while (k < n && p[k] >= 0x20 && p[k] <= 0x3f) k++;
            if (k >= n) {
                if (k < sizeof(c->pend) - 8) return 0; /* still arriving */
                lead_was_typed(c);
                if (!c->focused) to_shell(c, p, k);    /* not a sequence we know: as it came */
                return k;
            }
            if (p[k] < 0x40 || p[k] > 0x7e) {
                lead_was_typed(c);
                if (!c->focused) to_shell(c, p, k);
                return k;
            }
            if (!(p[2] == '<' && (p[k] == 'M' || p[k] == 'm'))) lead_was_typed(c); /* a key, not the mouse */
            csi(c, p, k + 1);
            return k + 1;
        }
        lead_was_typed(c);
        if (p[1] == 'O') {
            if (n < 3) return 0;
            static const char finals[] = "ABCDHF";
            static const enum edit_key keys[] = {K_UP, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END};
            const char *hit = p[2] ? strchr(finals, p[2]) : NULL;
            if (!c->focused) to_shell(c, p, 3);
            else if (hit) edit_key(c, keys[hit - finals]);
            return 3;
        }
        if (p[1] == 0x1b) { /* Esc, then the start of something else */
            if (!c->focused) to_shell(c, p, 1);
            else hide(c);
            return 1;
        }
        /* Alt with a key */
        if (!c->focused) to_shell(c, p, 2);
        else if (p[1] == '\r' || p[1] == '\n') {
            if (!c->picking) input_insert(c, "\n", 1);
        }
        return 2;
    }
    if (!c->focused) {
        size_t k = 1;
        while (k < n && p[k] != 0x1b && p[k] != c->host.lead) k++;
        to_shell(c, p, k);
        return k;
    }
    if (b < 0x20 || b == 0x7f) {
        chat_ctrl(c, b);
        return 1;
    }
    size_t need = b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
    if (need == 0) return 1; /* not the start of a character */
    if (n < need) return 0;
    uint32_t cp;
    size_t took = comp_utf8_next((const char *)p, need, &cp);
    if (took == need && cp != 0xfffd) chat_char(c, cp);
    return took;
}

void chat_input(struct chat *c, const unsigned char *buf, size_t n, int64_t now) {
    unsigned char stack[8192 + sizeof(c->pend)], *data = stack;
    size_t total = c->pend_len + n;
    c->now = c->input_ms = now;
    if (total > sizeof(stack)) {
        data = malloc(total);
        if (!data) return;
    }
    memcpy(data, c->pend, c->pend_len);
    memcpy(data + c->pend_len, buf, n);
    c->pend_len = 0;
    size_t i = 0;
    while (i < total) {
        size_t used = consume(c, data + i, total - i);
        if (used == 0) break;
        i += used;
    }
    size_t rest = total - i;
    if (rest > sizeof(c->pend)) { /* cannot be a token still arriving */
        if (!c->focused && !c->pasting) to_shell(c, data + i, rest);
        rest = 0;
    }
    memcpy(c->pend, data + i, rest);
    c->pend_len = rest;
    c->pend_ms = now;
    if (data != stack) free(data);
}

bool chat_take_detach(struct chat *c) {
    bool want = c->want_detach;
    c->want_detach = false;
    return want;
}

/* What was left pending never completed. */
static void pending_expired(struct chat *c) {
    lead_was_typed(c); /* an Esc (or half a sequence) after the lead key: both were typed */
    if (c->pasting) paste_text(c, c->pend, c->pend_len);
    else if (!c->focused) to_shell(c, c->pend, c->pend_len); /* a lone Esc, or half a sequence: the shell's */
    else if (c->pend_len == 1 && c->pend[0] == 0x1b) hide(c); /* the Esc key */
    c->pend_len = 0;
}

/* ---- timers --------------------------------------------------------------------------------- */

static bool hint_showing(const struct chat *c, int64_t now) {
    return !c->visible && c->hint[0] && now < c->hint_until_ms;
}

int64_t chat_due_in(const struct chat *c, int64_t now) {
    int64_t due = INT64_MAX, d;
    if (c->pend_len > 0) {
        d = c->pend_ms + (c->pasting ? 1000 : CHAT_ESC_MS) - now;
        if (d < due) due = d;
    }
    if (c->pasting) {
        d = c->input_ms + 1000 - now;
        if (d < due) due = d;
    }
    if (c->attached && hint_showing(c, now)) {
        d = c->hint_until_ms - now;
        if (d < due) due = d;
    }
    if (c->notice[0]) {
        d = c->notice_until_ms - now;
        if (d < due) due = d;
    }
    if (c->attached && c->visible && c->picking) {
        d = c->picks_ms + CHAT_PICK_REFRESH_MS - now;
        if (d < due) due = d;
    }
    for (int i = 0; i < CHAT_MAX_OUTBOUND; i++)
        if (c->out[i].pid && 1000 < due) due = 1000;
    return due < 0 ? 0 : due;
}

void chat_tick(struct chat *c, int64_t now) {
    c->now = now;
    if (c->pend_len > 0 && now - c->pend_ms >= (c->pasting ? 1000 : CHAT_ESC_MS)) {
        pending_expired(c);
        c->dirty = true;
    }
    if (c->pasting && now - c->input_ms >= 1000) c->pasting = false; /* the end marker never came */
    if (c->hint[0] && now >= c->hint_until_ms) {
        c->hint[0] = '\0';
        c->dirty = true;
    }
    if (c->notice[0] && now >= c->notice_until_ms) {
        c->notice[0] = '\0';
        c->dirty = true;
    }
    if (c->attached && c->visible && c->picking && now - c->picks_ms >= CHAT_PICK_REFRESH_MS) {
        refresh_picks(c);
        int n = pick_count(c);
        if (c->pick_sel >= n) c->pick_sel = n > 0 ? n - 1 : 0;
        c->dirty = true;
    }
    reap_sends(c);
}

/* ---- drawing ---------------------------------------------------------------------------------- */

/* What the lead key offers, shown for as long as it waits. */
static void lead_menu(const struct chat *c, char *out, size_t n) {
    char lead[12], detach[12], key[12];
    key_name(c->host.lead, lead);
    key_name(c->host.detach_key, detach);
    key_name(c->host.key, key);
    /* (in the window, kept short enough for its default width) */
    if (c->visible) snprintf(out, n, "%s: detach | %s: %s | %s: itself", detach, key, c->focused ? "shell" : "chat", lead);
    else snprintf(out, n, "%s: detach | %s: chat | %s: send %s", detach, key, lead, lead);
}

static int put(struct chat *c, int row, int col, int max, const char *s, const struct comp_style *st) {
    return max > 0 ? comp_text(c->comp, row, col, max, s, strlen(s), st) : 0;
}

static void draw_history(struct chat *c, const struct layout *L) {
    struct chat_thread *t = c->current;
    int width = c->w - 2, total = 0;
    if (width < 2 || L->body_rows < 1) return;
    for (int i = 0; i < t->count; i++) total += msg_lines(&t->msgs[i], width);
    int max_scroll = total > L->body_rows ? total - L->body_rows : 0;
    if (t->scroll > max_scroll) t->scroll = max_scroll;
    /* Bottom-anchored: line 0 is the newest; the view shows lines
     * [scroll, scroll + body_rows). */
    int below = 0;
    for (int i = t->count - 1; i >= 0 && below < t->scroll + L->body_rows; i--) {
        struct chat_msg *m = &t->msgs[i];
        int lines = msg_lines(m, width);
        int text_lines = msg_text_lines(m, width);
        /* One forward pass over the text: wrap state carries from line to
         * line, and the pass stops once a line falls below the view. */
        size_t text_pos = 0, note_pos = 0;
        /* this message's k-th line sits `below + lines - 1 - k` lines up */
        for (int k = 0; k < lines; k++) {
            int up = below + lines - 1 - k - t->scroll;
            if (up < 0) break;
            bool shown = up < L->body_rows;
            int row = L->body_top + L->body_rows - 1 - up;
            if (k == 0 && !shown) continue;
            if (k == 0) {
                char when[16] = "", head[CHAT_LABEL_LEN + 32];
                struct tm tmv;
                if (localtime_r(&m->at, &tmv)) strftime(when, sizeof(when), "%H:%M", &tmv);
                bool mine = m->state != MSG_IN;
                /* Yours under your call's name, not "you": the name the
                 * other side sees and answers. */
                /* Padded to 15 (the longest generated name), so the time
                 * starts at one column; a longer name pushes it out, never
                 * cut. Why: times that line up read as a column. */
                snprintf(head, sizeof(head), "%-15s", mine ? (c->self[0] ? c->self : "you") : t->label);
                int used = put(c, row, c->x + 1, width, head, mine ? &ST_YOU : &ST_PEER);
                used += put(c, row, c->x + 1 + used, width - used, " ", &ST_DIM);
                used += put(c, row, c->x + 1 + used, width - used, when, &ST_DIM);
                if (m->state == MSG_SENDING) put(c, row, c->x + 1 + used, width - used, " \xc2\xb7 sending...", &ST_DIM);
                else if (m->state == MSG_SENT) put(c, row, c->x + 1 + used, width - used, " \xc2\xb7 delivered", &ST_DIM);
                else if (m->state == MSG_FAILED) put(c, row, c->x + 1 + used, width - used, " \xc2\xb7 NOT DELIVERED", &ST_BAD);
                continue;
            }
            /* then its text, the note that a screen went along, and - when it
             * was not delivered - why, in full */
            const char *from = m->text;
            size_t from_len = m->len, *pos = &text_pos;
            const struct comp_style *st = &ST_BODY;
            if (k > text_lines) {
                if (m->with_screen && k == text_lines + 1) {
                    if (shown) put(c, row, c->x + 2, width - 1, "[the shell's screen went with this]", &ST_DIM);
                    continue;
                }
                from = m->note;
                from_len = strlen(m->note);
                st = &ST_BAD;
                pos = &note_pos;
            }
            size_t s = 0, e = 0;
            if (!wrap_next(from, from_len, width - 1, true, pos, &s, &e)) continue;
            if (shown) comp_text(c->comp, row, c->x + 2, width - 1, from + s, e - s, st);
        }
        below += lines;
    }
    if (t->count == 0) {
        put(c, L->body_top, c->x + 1, width, "Nothing said yet. Type below; Enter sends.", &ST_DIM);
    }
}

static void draw_picker(struct chat *c, const struct layout *L) {
    int width = c->w - 2, row = L->body_top;
    if (L->body_rows < 1 || width < 4) return;
    int used = put(c, row, c->x + 1, width, "to: ", &ST_DIM);
    used += put(c, row, c->x + 1 + used, width - used, c->filter, &ST_BODY);
    c->cur_row = row;
    c->cur_col = c->x + 1 + used;
    int n = pick_count(c), shown = L->body_rows - 1, first = 0;
    if (L->body_rows >= 4) shown--; /* the last row explains the keys */
    if (shown > 0 && c->pick_sel >= shown) first = c->pick_sel - shown + 1;
    c->pick_first = first;
    for (int i = 0; i < shown && first + i < n; i++) {
        const struct pick_entry *e = pick_at(c, first + i);
        bool sel = first + i == c->pick_sel;
        const struct comp_style *st = sel ? &ST_SEL : &ST_BODY;
        int r = row + 1 + i;
        comp_fill(c->comp, r, c->x + 1, width, st);
        int u = put(c, r, c->x + 1, width, sel ? "\xe2\x96\xb8 " : "  ", st);
        u += put(c, r, c->x + 1 + u, width - u, e->label, st);
        char tail[48] = "";
        if (e->unread) snprintf(tail, sizeof(tail), "  %d new", e->unread);
        else if (!e->running && e->kind == TH_CALL) snprintf(tail, sizeof(tail), "  ended");
        put(c, r, c->x + 1 + u, width - u, tail, sel ? st : &ST_DIM);
    }
    if (n == 0) {
        put(c, row + 1 < L->body_top + L->body_rows ? row + 1 : row, c->x + 1, width,
            c->filter_len ? "no running call matches" : "no other call is running on a PTY", &ST_DIM);
    }
    if (L->body_rows >= 4)
        put(c, L->body_top + L->body_rows - 1, c->x + 1, width,
            "\xe2\x86\x91\xe2\x86\x93 choose \xc2\xb7 Enter select \xc2\xb7 type to narrow", &ST_DIM);
}

static void draw_window(struct chat *c) {
    struct layout L;
    struct input_view v;
    int text_w = c->w - 4; /* a margin each side, and the "> " */
    view_input(c, text_w, &v);
    layout(c, v.lines, &L);
    c->body_rows = L.body_rows;
    for (int r = c->y; r < c->y + c->h; r++) comp_fill(c->comp, r, c->x, c->w, &ST_BODY);

    /* title bar: the handle to drag, and the close control */
    const struct comp_style *ts = c->focused ? &ST_TITLE : &ST_TITLE_IDLE;
    comp_fill(c->comp, c->y, c->x, c->w, ts);
    /* "<you> chat -> <them>": both ends of the conversation, and the way
     * your messages go. Why: the title is the one line always in view, and
     * it is where you look to learn your own name (the one others reply to). */
    int room = c->w - 5, used = 0;
    if (c->self[0]) {
        used += put(c, c->y, c->x + 1, room, c->self, ts);
        used += put(c, c->y, c->x + 1 + used, room - used, " ", ts);
    }
    used += put(c, c->y, c->x + 1 + used, room - used, "chat", ts);
    if (c->picking) used += put(c, c->y, c->x + 1 + used, room - used, " -> who to talk to", ts);
    else if (c->current) {
        used += put(c, c->y, c->x + 1 + used, room - used, " -> ", ts);
        used += put(c, c->y, c->x + 1 + used, room - used, c->current->label, ts);
    }
    int others = total_unread(c, c->picking ? NULL : c->current);
    if (others) {
        char more[32];
        snprintf(more, sizeof(more), "  +%d new", others);
        put(c, c->y, c->x + 1 + used, room - used, more, ts);
    }
    comp_put(c->comp, c->y, c->x + c->w - 2, 0xd7, ts);

    c->cur_row = -1;
    if (c->picking) draw_picker(c, &L);
    else if (c->current) draw_history(c, &L);
    else if (L.body_rows > 0) put(c, L.body_top, c->x + 1, c->w - 2, "Tab chooses who to talk to.", &ST_DIM);

    if (L.rule >= 0) {
        for (int i = 0; i < c->w; i++) comp_put(c->comp, L.rule, c->x + i, 0x2500, &ST_DIM);
        char menu[96];
        lead_menu(c, menu, sizeof(menu));
        if (c->lead_pending || c->notice[0]) {
            const char *line = c->lead_pending ? menu : c->notice;
            const struct comp_style *ns = c->lead_pending || c->notice_tip ? &ST_SEL : &ST_BAD;
            int u = put(c, L.rule, c->x + 1, c->w - 2, " ", ns);
            u += put(c, L.rule, c->x + 1 + u, c->w - 3 - u, line, ns);
            put(c, L.rule, c->x + 1 + u, c->w - 2 - u, " ", ns);
        } else if (c->current && !c->picking && c->current->scroll > 0) {
            put(c, L.rule, c->x + 2, c->w - 4, " earlier messages shown \xc2\xb7 PgDn ", &ST_DIM);
        }
    }

    /* the message line(s) */
    int first = v.cur_line >= L.input_rows ? v.cur_line - L.input_rows + 1 : 0;
    for (int i = 0; i < L.input_rows; i++) {
        int row = L.input_top + i, line = first + i;
        comp_fill(c->comp, row, c->x, c->w, &ST_INPUT);
        put(c, row, c->x + 1, 2, line == 0 ? "> " : "  ", &ST_INPUT_HINT);
        if (line < v.lines && c->in_len > 0) {
            size_t s = v.starts[line], e = line + 1 < v.lines ? v.starts[line + 1] : c->in_len;
            if (e > s && c->input[e - 1] == '\n') e--;
            if (e > s) comp_text(c->comp, row, c->x + 3, text_w, c->input + s, e - s, &ST_INPUT);
        }
        if (c->in_len == 0 && i == 0) {
            char hint[CHAT_LABEL_LEN + 32];
            if (c->current && c->current->kind != TH_OUTSIDE) snprintf(hint, sizeof(hint), "message %s", c->current->label);
            else snprintf(hint, sizeof(hint), "Tab: choose who to talk to");
            put(c, row, c->x + 3, text_w, hint, &ST_INPUT_HINT);
        }
        if (!c->picking && line == v.cur_line) {
            c->cur_row = row;
            c->cur_col = c->x + 3 + (v.cur_col < text_w ? v.cur_col : text_w - 1);
        }
    }

    if (L.footer >= 0) {
        int u = put(c, L.footer, c->x + 1, c->w - 4, c->include_screen ? "[x] screen" : "[ ] screen", c->include_screen ? &ST_BODY : &ST_DIM);
        put(c, L.footer, c->x + 1 + u, c->w - 4 - u, "  ^S \xc2\xb7 Tab: to \xc2\xb7 Esc: hide", &ST_DIM);
        comp_put(c->comp, L.footer, c->x + c->w - 1, 0x25e2, &ST_DIM); /* the corner to resize by */
    }
}

/* With chat hidden: what is unread, or (just after attaching) how to open it. */
static void draw_badge(struct chat *c, int64_t now) {
    char text[CHAT_LABEL_LEN + 96] = "", key[24], menu[96];
    int unread = total_unread(c, NULL);
    chat_keys(c, key);
    c->badge_col = -1;
    if (c->lead_pending) {
        lead_menu(c, menu, sizeof(menu));
        snprintf(text, sizeof(text), " %s ", menu);
    } else if (unread) {
        const struct chat_thread *from = NULL;
        int senders = 0;
        for (int i = 0; i < c->nthreads; i++)
            if (c->threads[i]->unread) {
                from = c->threads[i];
                senders++;
            }
        if (senders == 1) snprintf(text, sizeof(text), " chat: %d new from %.40s | %s ", unread, from->label, key);
        else snprintf(text, sizeof(text), " chat: %d new | %s ", unread, key);
    } else if (hint_showing(c, now)) {
        snprintf(text, sizeof(text), "%s", c->hint);
    }
    if (!text[0]) return;
    int len = (int)strlen(text);
    if (len > c->tcols) len = c->tcols;
    c->badge_col = c->tcols - len;
    comp_text(c->comp, 0, c->badge_col, len, text, strlen(text), &ST_BADGE);
}

bool chat_wants_frame(const struct chat *c) {
    return c->attached && (c->dirty || c->screen_dirty);
}

void chat_screen_changed(struct chat *c) {
    c->screen_dirty = true;
}

int chat_render(struct chat *c, int64_t now, char **buf, size_t *len, size_t *cap) {
    struct hold_snapshot snap;
    c->now = now;
    if (!c->attached) return 0;
    if (hold_screen_snapshot(c->host.screen, &snap) != 0) return -1;
    comp_begin(c->comp, &snap);
    if (c->visible) {
        refit(c);
        draw_window(c);
        c->badge_col = -1;
    } else {
        draw_badge(c, now);
    }

    struct comp_want want;
    memset(&want, 0, sizeof(want));
    want.mouse = snap.modes.mouse;
    /* While the window is up its title bar and corner must be draggable:
     * presses, releases and drags are reported even if the program wants
     * none of them (those outside the window are then simply dropped). */
    if (c->visible && want.mouse < HOLD_MOUSE_DRAG) want.mouse = HOLD_MOUSE_DRAG;
    if (c->mouse_on && want.mouse == HOLD_MOUSE_NONE) c->mouse_off_ms = now;
    c->mouse_on = want.mouse != HOLD_MOUSE_NONE;
    if (c->visible && c->focused) {
        want.bracketed_paste = true; /* a paste arrives whole, never as keys */
        want.cursor_visible = c->cur_row >= 0;
        want.cursor_row = c->cur_row < 0 ? 0 : c->cur_row;
        want.cursor_col = c->cur_col;
    } else {
        want.app_cursor_keys = snap.modes.app_cursor_keys;
        want.app_keypad = snap.modes.app_keypad;
        want.bracketed_paste = snap.modes.bracketed_paste;
        want.focus_report = snap.modes.focus_report;
        want.cursor_row = snap.cursor_row;
        want.cursor_col = snap.cursor_col;
        /* The shell's cursor, unless the window (or the terminal's edge) hides it. */
        want.cursor_visible = snap.modes.cursor_visible && snap.cursor_row < c->trows && snap.cursor_col < c->tcols &&
                              !(c->visible && inside(c, snap.cursor_row, snap.cursor_col)) &&
                              !(c->badge_col >= 0 && snap.cursor_row == 0 && snap.cursor_col >= c->badge_col);
    }
    want.cursor_style = hold_screen_cursor_style(c->host.screen);
    want.title = hold_screen_title(c->host.screen);
    want.bell = hold_screen_take_bells(c->host.screen) > 0;
    hold_snapshot_free(&snap);
    c->dirty = c->screen_dirty = false;
    return comp_flush(c->comp, &want, buf, len, cap);
}

/* ---- life ----------------------------------------------------------------------------------------- */

struct chat *chat_new(const struct chat_host *host) {
    struct chat *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->host = *host;
    if (!c->host.lead) c->host.lead = HOLD_LEAD_KEY_DEFAULT;
    if (!c->host.detach_key) c->host.detach_key = HOLD_DETACH_KEY_DEFAULT;
    if (!c->host.key) c->host.key = HOLD_CHAT_KEY_DEFAULT;
    c->comp = comp_new();
    c->badge_col = -1;
    c->cur_row = -1;
    if (!c->comp) {
        free(c);
        return NULL;
    }
    return c;
}

void chat_free(struct chat *c) {
    if (!c) return;
    for (int i = 0; i < c->nthreads; i++) free_thread(c->threads[i]);
    for (int i = 0; i < CHAT_MAX_OUTBOUND; i++)
        if (c->out[i].pid && c->out[i].fd >= 0) close(c->out[i].fd);
    comp_free(c->comp);
    free(c->input);
    free(c);
}

void chat_terminal_resized(struct chat *c, int rows, int cols) {
    if (rows < 1 || cols < 1) return;
    c->trows = rows;
    c->tcols = cols;
    (void)comp_set_size(c->comp, rows, cols);
    if (c->placed) refit(c);
    c->dirty = true;
}

void chat_attached(struct chat *c, int rows, int cols, int64_t now) {
    c->now = now;
    c->attached = true;
    c->grab = GRAB_NONE;
    c->pend_len = 0;
    c->pasting = false;
    c->mouse_on = false;
    chat_terminal_resized(c, rows, cols);
    /* The name is read on every attach, not first when the chat opens, and
     * the tip is the name alone. Why: it is what msg, peek and attach take,
     * and whoever starts or rejoins a shell or tool had no way to learn it
     * until they chatted; the command they just typed needs no repeating.
     * -it shows it in a corner banner; with the chat window on, this tip is
     * that banner. */
    self_label(c, c->self, sizeof(c->self));
    if (!c->visible) set_tip(c, c->self, "chat");
    comp_invalidate(c->comp); /* a fresh terminal: paint it all, set every mode */
    /* What arrived for the conversation on show while nobody was attached is
     * in front of them now. */
    if (c->visible && c->current && !c->picking) c->current->unread = 0;
    c->dirty = true;
}

void chat_detached(struct chat *c) {
    c->attached = false;
    c->grab = GRAB_NONE;
    c->pend_len = 0;
    c->pasting = false;
    c->lead_pending = c->want_detach = false;
    /* The window, the keyboard's place, and the draft are as they were when
     * the person comes back. */
}
