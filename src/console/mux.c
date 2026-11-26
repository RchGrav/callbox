/* console/mux.c - pane composition and floating windows.
 *
 * A single process uses plain attach (attach.c) until a compositor command
 * is requested. Ctrl-Space p a opens the attach picker; Ctrl-Space c opens
 * chat; Ctrl-Space ? opens keyboard help. The compositor returns to plain
 * attach when one pane remains with no floating window or active mode.
 *
 * Each pane attaches to one broker. The broker owns the authoritative
 * screen and answers terminal queries for NO_TERMINAL clients. This layer
 * mirrors each process's output and composes the panes and overlays, then
 * writes only changed cells (compose.c).
 *
 * Each window contains up to four panes. Slot letters span windows: A-D in
 * window 1, E-H in window 2, through Z. Later slots have no letter. Within a
 * window, occupied slots are contiguous. Resize mode adjusts the dividers:
 *
 *     1 pane   2 panes   3 panes   4 panes
 *       A       A B       A B       A B
 *                         C         C D
 *
 * The first split is left/right, preserving both panes' full height until
 * a third pane is added. The letters above illustrate window 1. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/console.h"
#include "backmux/core.h"
#include "backmux/store.h"
#include "backmux/screen.h"
#include "backmux/term.h"
#include "backmux/console_internal.h"
#include "overlay.h"

#define MUX_MAX_WINDOWS 9 /* selected by number keys 1-9 */
#define MUX_MAX_PANES (MUX_MAX_WINDOWS * 4)
#define MUX_FRAME_MS 16      /* output is drawn at most this often */
#define MUX_LOOK_MS 250      /* how often chat looks at the inbox and its owner */
/* How often a FIFO pane is read without poll saying so. Why: macOS's poll
 * never reports a FIFO's writer closing (Linux says POLLHUP), so a pane whose
 * process ended would stay on screen; a read returning 0 is the only sign.
 * The same interval as the attach client's probe. */
#define MUX_FIFO_PROBE_MS 250
#define MUX_PICK_MAX 64

/* Ctrl-Space sends NUL; the following unmodified key selects a command. */
#define KEY_PREFIX 0x00 /* the default; --detach-keys may name another lead key */
#define MUX_RESIZE_STEP 5 /* percentage points per resize keypress */
#define MUX_PCT_MIN 10    /* no pane narrower or shorter than this share */
#define MUX_PCT_MAX 90

struct pane {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char sock[HOLD_PATH_MAX];
    int rfd, wfd;
    struct hold_screen *scr;
    int row, col, rows, cols; /* where its program is drawn, on its window */
    int frow;                 /* its footer row: under the program, or its last row when the
                               * footer only shows for a moment (footers off) */
    bool ended;
    char held[96];            /* opening bytes that may be a refusal */
    size_t held_len;
    bool heard;
};

enum floater { FL_NONE, FL_ATTACH, FL_JUMP };

/* A mode keeps the keyboard until Esc or Enter, so moving around takes no
 * prefix before every press. ? in a mode shows that mode's keys. */
enum mode { MODE_NONE, MODE_PANE, MODE_TAB, MODE_RESIZE, MODE_MOVE };

/* Profile name prompt in the active pane's footer: Enter confirms, Esc cancels. */
enum prompt { PROMPT_NONE, PROMPT_RENAME, PROMPT_SAVE };

struct pick {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char sock[HOLD_PATH_MAX];
    char program[256];
    char args[512];
    char working[HOLD_PATH_MAX];
    char started[64];
};

struct mux {
    const struct hold_store *store;
    char me[ALIAS_MAX_LEN + 1];
    struct pane *slot[MUX_MAX_PANES]; /* window w holds slot[w*4 .. w*4+3], dense from its start */
    struct pane *full; /* the pane toggled full screen, NULL none */
    enum mode mode;
    bool help;   /* keyboard help for the active mode or prefix commands */
    bool swapping;  /* prefix s: letters (and a number) say what goes where */
    int swap_first; /* the first pane chosen, -1 none yet */
    enum prompt prompt;
    struct pane *prompt_pane;
    char in[ALIAS_MAX_LEN + 1];
    size_t in_len;
    /* where each window's lines are, in percent: the line between left and
     * right, and the line in each column (resize mode moves them) */
    int colpct[MUX_MAX_WINDOWS], lpct[MUX_MAX_WINDOWS], rpct[MUX_MAX_WINDOWS];
    int64_t fifo_probe_at; /* the next read of FIFO panes poll said nothing of */
    bool assumed;          /* the frame on the terminal was assumed (inline) */
    int view;  /* the window on screen */
    int focus; /* a slot on the window on screen */
    struct compositor *comp;
    int trows, tcols;
    bool dirty;
    int64_t now;
    bool lead;
    /* the keys in force (--detach-keys, else Ctrl-Space then q): plain attach
     * and the compositor read the same ones, so a prefix given there is the
     * prefix here too */
    unsigned char prefix, detach_key;
    int64_t window_note_until;
    int64_t names_until;
    bool advertise; /* the panes changed: rewrite chat/mux.<pid> for backmux swap */
    /* the attach picker's list, as last drawn (a click picks a row) */
    int pick_x, pick_w, pick_top, pick_first, pick_rows;
    /* the floating window that is not chat */
    enum floater fl;
    struct pick picks[MUX_PICK_MAX];
    int npicks, pick_sel;
    char notice[160];
    int64_t notice_until;
    /* The user's chat window, floating over the panes. */
    struct chat *chat;
    bool chat_loaded; /* the conversation log is replayed into it */
    off_t inbox_off;
    int64_t next_look;
    /* input not yet whole: an escape sequence split across reads */
    unsigned char esc[64];
    size_t esc_len;
    unsigned char out[4096]; /* keys for the focused pane, sent at the end of a read */
    size_t out_len;
};

/* ---- what outlives one run of the compositor ----------------------------
 * Plain attach hands the terminal to the compositor and takes it back as
 * panes come and go, in one process; these belong to the person's terminal,
 * not to one run, so they live here. */
static bool g_footers = true; /* prefix f: every pane's footer row */
static char g_locked[MUX_MAX_PANES][ID_STR_LEN];

static bool pane_locked(const struct pane *p) {
    for (int i = 0; p && p->id[0] && i < MUX_MAX_PANES; i++)
        if (!strcmp(g_locked[i], p->id)) return true;
    return false;
}

static void set_locked(const struct pane *p, bool on) {
    if (!p || !p->id[0]) return;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (!strcmp(g_locked[i], p->id)) g_locked[i][0] = '\0';
    for (int i = 0; on && i < MUX_MAX_PANES; i++)
        if (!g_locked[i][0]) {
            snprintf(g_locked[i], sizeof(g_locked[i]), "%s", p->id);
            return;
        }
}

/* Slot letters span windows: A-D in window 1, E-H in window 2, through Z.
 * Number keys can move panes to windows whose slots have no letter. */
static char slot_letter(int slot) {
    return slot >= 0 && slot < 26 ? (char)('A' + slot) : 0;
}

static int letter_slot(unsigned char c) {
    int k = toupper(c) - 'A';
    return k >= 0 && k < 26 && k < MUX_MAX_PANES && isalpha(c) ? k : -1;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notice(struct mux *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->notice, sizeof(m->notice), fmt, ap);
    va_end(ap);
    m->notice_until = m->now + 3000;
    m->dirty = true;
}

/* ---- the wire to one broker ------------------------------------------- */

static int frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char h[CONSOLE_FRAME_HEADER_LEN];
    h[0] = type;
    console_store_be16(h + 1, len);
    if (hold_write_all(fd, h, sizeof(h)) != 0) return -1;
    return len ? hold_write_all(fd, payload, len) : 0;
}

static void pane_send(struct pane *p, const void *bytes, size_t n) {
    const unsigned char *b = bytes;
    while (n > 0 && p->wfd >= 0 && !p->ended) {
        uint16_t k = n > 4096 ? 4096 : (uint16_t)n;
        if (frame(p->wfd, CONSOLE_FRAME_DATA, b, k) != 0) {
            p->ended = true;
            return;
        }
        b += k;
        n -= k;
    }
}

static void pane_resize(struct pane *p, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    int r0, c0;
    hold_screen_size(p->scr, &r0, &c0);
    if (r0 != rows || c0 != cols) hold_screen_resize(p->scr, rows, cols);
    unsigned char payload[4];
    console_store_be16(payload, (uint16_t)rows);
    console_store_be16(payload + 2, (uint16_t)cols);
    if (p->wfd >= 0 && frame(p->wfd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload)) != 0) p->ended = true;
}

static void pane_free(struct pane *p) {
    if (!p) return;
    if (p->wfd >= 0 && p->wfd != p->rfd) close(p->wfd);
    if (p->rfd >= 0) close(p->rfd);
    hold_screen_free(p->scr);
    free(p);
}

/* Attach to a process's console as a pane, retrying connection-refused
 * errors briefly. */
static struct pane *pane_open(const char *id, const char *name, const char *sock, int rows, int cols, const char **why) {
    *why = NULL;
    for (int tries = 0; tries < 20; tries++) {
        int w = -1;
        int fd = hold_open_channel(sock, 'A', &w);
        if (fd < 0) {
            if (errno == EBUSY) {
                struct timespec sl = {0, 50 * 1000000L};
                nanosleep(&sl, NULL);
                continue;
            }
            *why = errno == ENOENT || errno == ECONNREFUSED ? "console unavailable" : strerror(errno);
            return NULL;
        }
        struct pane *p = calloc(1, sizeof(*p));
        if (!p || !(p->scr = hold_screen_new(rows > 0 ? rows : 1, cols > 0 ? cols : 1))) {
            free(p);
            if (w != fd) close(w);
            close(fd);
            *why = "out of memory";
            return NULL;
        }
        p->rfd = fd;
        p->wfd = w;
        snprintf(p->id, sizeof(p->id), "%s", id);
        snprintf(p->name, sizeof(p->name), "%s", name);
        snprintf(p->sock, sizeof(p->sock), "%s", sock);
        /* NO_TERMINAL: the broker's own screen answers the program's
         * queries; this side only mirrors. */
        if (hold_write_all(w, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0 ||
            frame(w, CONSOLE_FRAME_NO_TERMINAL, NULL, 0) != 0) {
            pane_free(p);
            *why = "console handshake failed";
            return NULL;
        }
        pane_resize(p, rows, cols);
        /* A refusal comes as one line of text before the broker hangs up. */
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 150) > 0) {
            ssize_t n = read(fd, p->held, sizeof(p->held));
            if (n > 0) p->held_len = (size_t)n;
            static const char lead[] = "backmux: console already attached";
            if (n > 0 && (size_t)n >= sizeof(lead) - 1 && !memcmp(p->held, lead, sizeof(lead) - 1)) {
                pane_free(p);
                struct timespec sl = {0, 50 * 1000000L};
                nanosleep(&sl, NULL);
                *why = "console already attached";
                continue;
            }
            if (n == 0) {
                pane_free(p);
                *why = "console connection closed";
                return NULL;
            }
            if (n > 0) hold_screen_feed(p->scr, p->held, (size_t)n);
            hold_screen_discard_output(p->scr);
            p->held_len = 0;
            p->heard = n > 0;
        }
        (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        return p;
    }
    if (!*why) *why = "console busy";
    return NULL;
}

/* ---- windows and slots ------------------------------------------------ */

static int win_count(const struct mux *m, int w) {
    int k = 0;
    while (k < 4 && m->slot[w * 4 + k]) k++;
    return k;
}

static int pane_total(const struct mux *m) {
    int n = 0;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) n += win_count(m, w);
    return n;
}

/* A window's number is its key, 1-9. */
static int window_key(int w) {
    return w + 1;
}

/* The next window, after w, that has panes; w itself if none other does. */
static int next_window(const struct mux *m, int w) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = (w + i) % MUX_MAX_WINDOWS;
        if (win_count(m, c) > 0) return c;
    }
    return w;
}

/* The lines of window w, on a rows x cols screen: the column of the line
 * between left and right, and the row of the line in each column (left
 * column, right column). A share of 0 means half: a window never resized. */
static int pct_or_half(int pct) {
    return pct ? pct : 50;
}

static void lines_of(const struct mux *m, int w, int rows, int cols, int *left, int *ltop, int *rtop) {
    *left = (cols - 1) * pct_or_half(m->colpct[w]) / 100;
    *ltop = (rows - 1) * pct_or_half(m->lpct[w]) / 100;
    *rtop = (rows - 1) * pct_or_half(m->rpct[w]) / 100;
    if (*left < 1) *left = 1;
    if (*left > cols - 2) *left = cols - 2;
    if (*ltop < 1) *ltop = 1;
    if (*ltop > rows - 2) *ltop = rows - 2;
    if (*rtop < 1) *rtop = 1;
    if (*rtop > rows - 2) *rtop = rows - 2;
}

/* Where slot k of window w (n panes) goes, on a rows x cols screen. One
 * column of line separates left from right, one row top from bottom: A left,
 * B right, C under A, D under B. */
static void place(const struct mux *m, int w, int n, int k, int rows, int cols, int *r, int *c, int *h, int *wd) {
    if (n <= 1 || cols < 3 || rows < 3) {
        *r = 0, *c = 0, *h = rows, *wd = cols;
        return;
    }
    int left, ltop, rtop;
    lines_of(m, w, rows, cols, &left, &ltop, &rtop);
    int rcol = left + 1, rw = cols - rcol;
    switch (k) {
    case 0: /* A: the left column, or its top from three on */
        *r = 0, *c = 0, *wd = left, *h = n >= 3 ? ltop : rows;
        break;
    case 1: /* B: the right column, or its top once there are four */
        *r = 0, *c = rcol, *wd = rw, *h = n == 4 ? rtop : rows;
        break;
    case 2: /* C: under A */
        *r = ltop + 1, *c = 0, *wd = left, *h = rows - ltop - 1;
        break;
    default: /* D: under B */
        *r = rtop + 1, *c = rcol, *wd = rw, *h = rows - rtop - 1;
        break;
    }
}

/* Every pane where it goes; a pane toggled full takes its window's whole
 * screen (the others in that window wait behind it, at their sizes). */
/* Inline, leaving the person's screen for the alternate one: first one
 * plain frame on theirs - the pane alone, the cursor where the program has
 * it - which erases the corner label and anything this compositor drew
 * there; the alternate screen then keeps their screen as it is. */
static void go_alt(struct mux *m) {
    struct pane *f = NULL;
    for (int i = 0; i < MUX_MAX_PANES && !f; i++) f = m->slot[i];
    if (f) {
        struct comp_want want = {0};
        struct hold_snapshot snap;
        comp_begin(m->comp, NULL);
        if (hold_screen_snapshot(f->scr, &snap) == 0) {
            comp_blit(m->comp, &snap, 0, 0, snap.rows, snap.cols);
            want.cursor_row = snap.cursor_row;
            want.cursor_col = snap.cursor_col;
            want.cursor_visible = snap.modes.cursor_visible;
            hold_snapshot_free(&snap);
        }
        char *buf = NULL;
        size_t len = 0, cap = 0;
        if (comp_flush(m->comp, &want, &buf, &len, &cap) == 0 && len) (void)hold_write_all(STDOUT_FILENO, buf, len);
        free(buf);
    }
    hold_console_leave_inline();
    comp_invalidate(m->comp);
    m->assumed = false;
    m->dirty = true;
}

static void relayout(struct mux *m) {
    /* An inline terminal still shows the person's lines above the launch,
     * which no pane holds: once a second pane splits the screen it is all
     * ours - on the alternate screen, so leaving gives theirs back - and
     * the next frame paints all of it. (Before the panes are resized: the
     * last frame on the person's screen is the pane as it was.) */
    if (m->assumed && pane_total(m) > 1) go_alt(m);
    /* Footers take a row from each pane while there is more than one: one
     * pane left goes back to plain attach, and a row taken and given back
     * would make its program redraw twice for nothing. */
    bool footers = g_footers && pane_total(m) > 1;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        int n = win_count(m, w);
        for (int k = 0; k < n; k++) {
            struct pane *p = m->slot[w * 4 + k];
            int r, c, h, wd;
            if (p == m->full) r = 0, c = 0, h = m->trows, wd = m->tcols;
            else place(m, w, n, k, m->trows, m->tcols, &r, &c, &h, &wd);
            p->frow = r + h - 1;
            if (footers && h > 1) h--;
            p->row = r, p->col = c;
            if (p->rows != h || p->cols != wd) {
                p->rows = h, p->cols = wd;
                pane_resize(p, h, wd);
            }
        }
    }
    m->dirty = true;
    m->advertise = true;
}

/* Keep each window's panes dense from its start, in arrival order (there is
 * no C before a B). Windows keep their numbers: an empty one is a gap. The
 * view moves on if its window emptied. */
static void compact(struct mux *m) {
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        struct pane *in[4];
        int k = 0;
        for (int i = 0; i < 4; i++)
            if (m->slot[w * 4 + i]) in[k++] = m->slot[w * 4 + i];
        for (int i = 0; i < 4; i++) m->slot[w * 4 + i] = i < k ? in[i] : NULL;
    }
    if (m->full) {
        bool there = false;
        for (int i = 0; i < MUX_MAX_PANES && !there; i++) there = m->slot[i] == m->full;
        if (!there) m->full = NULL;
    }
    if (win_count(m, m->view) == 0) m->view = next_window(m, m->view);
    int n = win_count(m, m->view);
    if (m->focus < m->view * 4 || m->focus >= m->view * 4 + n) m->focus = m->view * 4;
}

static struct pane *focused(const struct mux *m) {
    return m->slot[m->focus];
}

static void set_focus(struct mux *m, int slot) {
    if (slot < 0 || slot >= MUX_MAX_PANES || !m->slot[slot]) return;
    /* chat talks to the pane you are in: moving there while it is up moves
     * the conversation with you (Tab still reaches anyone else) */
    if (m->chat && m->focus != slot) {
        chat_set_screen(m->chat, m->slot[slot]->scr, m->slot[slot]->name); /* Ctrl-S sends this pane's screen */
        chat_show_call(m->chat, m->slot[slot]->id, m->slot[slot]->name);
    }
    m->focus = slot;
    m->view = slot / 4;
    m->dirty = true;
    m->advertise = true;
}

/* After panes left: close up the windows, lay them out again, and point chat
 * at the pane now in focus (the one it showed may be gone). */
static void settle(struct mux *m) {
    compact(m);
    relayout(m);
    struct pane *f = focused(m);
    if (f && m->chat) chat_set_screen(m->chat, f->scr, f->name);
    m->dirty = true;
}

/* A new pane joins the window on screen when it has room, else the first
 * window that does. Focus goes to it. */
static int add_pane(struct mux *m, struct pane *p) {
    for (int i = 0; i < MUX_MAX_WINDOWS; i++) {
        int w = i == 0 ? m->view : i - 1;
        if (i > 0 && w == m->view) continue;
        int n = win_count(m, w);
        if (n < 4) {
            m->slot[w * 4 + n] = p;
            relayout(m);
            set_focus(m, w * 4 + n);
            return 0;
        }
    }
    return -1;
}

static bool has_pane(const struct mux *m, const char *id) {
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strcmp(m->slot[i]->id, id)) return true;
    return false;
}

/* A pane by its process's name or id: what backmux swap takes from a shell
 * or an agent, which cannot see this screen. -1 if none. */
static int slot_of_name(const struct mux *m, const char *w) {
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strcmp(m->slot[i]->name, w)) return i;
    size_t l = strlen(w);
    for (int i = 0; l >= 4 && i < MUX_MAX_PANES; i++)
        if (m->slot[i] && !strncmp(m->slot[i]->id, w, l)) return i;
    return -1;
}

/* Two panes trade places: their windows and positions. */
static void do_swap(struct mux *m, int a, int b) {
    if (a == b || !m->slot[a] || !m->slot[b]) return;
    struct pane *t = m->slot[a];
    m->slot[a] = m->slot[b];
    m->slot[b] = t;
    relayout(m);
    m->dirty = true;
}

/* The active pane goes to window w (its key's), at the end; the view stays
 * where it is. 0 moved; -1 that window is full. */
static int move_pane(struct mux *m, int slot, int w) {
    struct pane *p = m->slot[slot];
    if (!p || w == slot / 4) return 0;
    int n = win_count(m, w);
    if (n >= 4) return -1;
    m->slot[slot] = NULL;
    m->slot[w * 4 + n] = p;
    if (m->full == p) m->full = NULL;
    int view = m->view;
    compact(m);
    if (win_count(m, view) > 0) {
        m->view = view;
        m->focus = view * 4;
    }
    relayout(m);
    return 0;
}

/* ---- the picker: running TTY processes ------------------------------------ */

struct pick_walk {
    struct mux *m;
};

static int pick_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct mux *m = ((struct pick_walk *)ctx)->m;
    if (!r || m->npicks >= MUX_PICK_MAX) return 0;
    /* Only TTY processes can be panes: a pane shows a screen, and a server's
     * output is a log. */
    if (!r->has_console || !r->recipe.mode_tty || r->has_exit_code || !hold_path_exists(r->console_sock) ||
        !hold_console_record_alive(r))
        return 0;
    if (has_pane(m, id)) return 0;
    /* the process this terminal itself runs in: shown inside itself, it would
     * draw itself forever */
    const char *here = getenv("BACKMUX_ID");
    if (here && !strcmp(here, id)) return 0;
    struct pick *p = &m->picks[m->npicks++];
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    if (r->has_name && r->name[0]) snprintf(p->name, sizeof(p->name), "%s", r->name);
    else snprintf(p->name, sizeof(p->name), "%.12s", id);
    snprintf(p->sock, sizeof(p->sock), "%s", r->console_sock);
    if (r->recipe.argc > 0 && r->recipe.argv) snprintf(p->program, sizeof(p->program), "%s", r->recipe.argv[0]);
    if (r->recipe.argc > 1) (void)hold_format_argv_human(p->args, sizeof(p->args), r->recipe.argc - 1, r->recipe.argv + 1);
    snprintf(p->working, sizeof(p->working), "%s",
             r->recipe.has_working ? r->recipe.working : (r->has_observed ? r->observed_cwd : ""));
    if (r->has_started_at) snprintf(p->started, sizeof(p->started), "%s", r->started_at);
    return 0;
}

static void load_picks(struct mux *m) {
    m->npicks = 0;
    m->pick_sel = 0;
    struct pick_walk w = {m};
    (void)hold_for_each_record(m->store->record_dir, pick_cb, &w);
}

/* ---- chat --------------------------------------------------------------
 * Compositor-hosted chat uses the user's chat name and inbox. Opening chat
 * here claims display ownership from other terminals using that identity.
 * The conversation log (chat/<name>.log) preserves history across terminals. */

static int chat_path(const struct mux *m, const char *what, char *out, size_t n) {
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/chat", m->store->base) != 0) return -1;
    if (hold_mkdir_p_mode(dir, 0700) != 0) return -1;
    return hold_checked_snprintf(out, n, "%s/%s.%s", dir, m->me, what);
}

static void write_small(const char *path, const char *text, size_t len) {
    char tmp[HOLD_PATH_MAX + 16];
    if (hold_checked_snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid()) != 0) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    bool ok = hold_write_all(fd, text, len) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

static size_t read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    out[n] = '\0';
    return (size_t)n;
}

static bool chat_mine(const struct mux *m) {
    char path[HOLD_PATH_MAX], text[64];
    if (chat_path(m, "active", path, sizeof(path)) != 0) return true;
    if (!read_small(path, text, sizeof(text))) return false;
    return atoi(text) == (int)getpid();
}

/* Who a name is: a process (its id, so a rename keeps the conversation) or a
 * sender outside any process (its inbox name). */
struct find_walk {
    const char *name;
    char id[ID_STR_LEN];
};

static int find_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct find_walk *f = ctx;
    if (!r || !r->has_name || strcmp(r->name, f->name) != 0) return 0;
    snprintf(f->id, sizeof(f->id), "%s", id);
    return 1;
}

static bool call_id_of(const struct mux *m, const char *name, char *id, size_t n) {
    struct find_walk f = {.name = name};
    (void)hold_for_each_record(m->store->record_dir, find_cb, &f);
    snprintf(id, n, "%s", f.id);
    return f.id[0] != '\0';
}

/* The conversation log: every line in and out. "UNIXTIME DIR PEER HEXTEXT",
 * DIR > sent, < received. */
static void log_line(struct mux *m, bool mine, const char *peer, const char *text, size_t len) {
    char path[HOLD_PATH_MAX];
    if (chat_path(m, "log", path, sizeof(path)) != 0) return;
    size_t cap = 64 + ALIAS_MAX_LEN + ID_STR_LEN + 2 * len;
    char *buf = malloc(cap);
    if (!buf) return;
    int k = snprintf(buf, cap, "%lld %c %s ", (long long)time(NULL), mine ? '>' : '<', peer);
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        buf[k++] = hx[(unsigned char)text[i] >> 4];
        buf[k++] = hx[(unsigned char)text[i] & 15];
    }
    buf[k++] = '\n';
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        (void)hold_write_all(fd, buf, (size_t)k);
        close(fd);
    }
    free(buf);
}

static void on_sent(void *app, const char *label, const char *text, size_t len) {
    log_line(app, true, label, text, len);
}

/* The history, into the chat window, the first time this terminal shows it. */
static void replay_log(struct mux *m) {
    char path[HOLD_PATH_MAX];
    if (m->chat_loaded || chat_path(m, "log", path, sizeof(path)) != 0) return;
    m->chat_loaded = true;
    FILE *f = fopen(path, "re");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        long long at = 0;
        char dir = 0, peer[ALIAS_MAX_LEN + 1];
        int off = 0;
        if (sscanf(line, "%lld %c %64s %n", &at, &dir, peer, &off) < 3 || off <= 0) continue;
        char *hex = line + off;
        size_t hl = strcspn(hex, "\n");
        char *text = malloc(hl / 2 + 1);
        if (!text) break;
        size_t tl = 0;
        for (size_t i = 0; i + 1 < hl; i += 2) {
            unsigned v = 0;
            if (sscanf(hex + i, "%2x", &v) != 1) break;
            text[tl++] = (char)v;
        }
        char id[ID_STR_LEN];
        bool call = call_id_of(m, peer, id, sizeof(id));
        chat_replay(m->chat, dir == '>', call ? id : peer, peer, !call, (time_t)at, text, tl);
        free(text);
    }
    free(line);
    fclose(f);
}

static int inbox_cb(time_t at, const char *from, const char *text, size_t len, void *ctx) {
    struct mux *m = ctx;
    (void)at;
    char id[ID_STR_LEN];
    bool call = call_id_of(m, from, id, sizeof(id));
    log_line(m, false, from, text, len);
    if (m->chat) chat_inbound(m->chat, call ? id : from, from, !call, text, len);
    m->dirty = true;
    return 0;
}

static void chat_look(struct mux *m) {
    if (!m->me[0] || m->now < m->next_look) return;
    m->next_look = m->now + MUX_LOOK_MS;
    hold_inbox_touch(m->store, m->me); /* in use: keep the name */
    off_t end = 0;
    if (hold_inbox_size(m->store, m->me) > m->inbox_off) {
        (void)hold_inbox_read(m->store, m->me, m->inbox_off, &end, inbox_cb, m);
        if (end > m->inbox_off) {
            m->inbox_off = end;
            (void)hold_inbox_mark_seen(m->store, m->me, end);
        }
    }
    /* Another terminal opened chat: it moved there. */
    if (m->chat && chat_visible(m->chat) && !chat_mine(m)) {
        chat_put_away(m->chat);
        m->dirty = true;
    }
}

/* Keys that are not chat's go to the focused pane; a pointer report outside
 * the window is the compositor's to route. */
static void to_pane(struct mux *m, const unsigned char *b, size_t n);
static void handle_mouse(struct mux *m, int b, int x, int y, bool release);
static void chat_to_app(void *app, const unsigned char *bytes, size_t n) {
    to_pane(app, bytes, n);
}
static void chat_pointer(void *app, int b, int x, int y, bool release) {
    handle_mouse(app, b, x, y, release);
}

/* Open chat for the active pane, focus a visible chat window, or hide it
 * when it already has keyboard focus. Claim this identity's chat display. */
static void chat_key_pressed(struct mux *m) {
    if (!m->chat) {
        notice(m, "chat unavailable: could not initialize this terminal's chat identity");
        return;
    }
    char path[HOLD_PATH_MAX], text[32];
    if (chat_path(m, "active", path, sizeof(path)) == 0) {
        int k = snprintf(text, sizeof(text), "%d\n", (int)getpid());
        write_small(path, text, (size_t)k);
    }
    replay_log(m);
    m->fl = FL_NONE; /* one floating window at a time */
    struct pane *f = focused(m);
    if (!chat_visible(m->chat) && f && f->id[0]) chat_open_on(m->chat, f->id, f->name);
    else if (chat_has_keyboard(m->chat)) chat_put_away(m->chat);
    else chat_toggle(m->chat);
    m->dirty = true;
}

/* ---- drawing ----------------------------------------------------------- */

/* Plain gray, not dim: a dim line all but disappears on many terminals.
 * The stretch beside the focused pane is green, to say where keys go. */
static const struct comp_style ST_SEP = {0, 244, -1};
static const struct comp_style ST_SEP_FOCUS = {HOLD_ATTR_BOLD, 2, -1};
static const struct comp_style ST_BOX = {0, 252, 236};
static const struct comp_style ST_BOX_DIM = {0, 245, 236};
static const struct comp_style ST_TITLE = {HOLD_ATTR_BOLD, 231, 24};
static const struct comp_style ST_SEL = {HOLD_ATTR_BOLD, 16, 150};
static const struct comp_style ST_BAD = {HOLD_ATTR_BOLD, 203, 236};
static const struct comp_style ST_LETTER = {HOLD_ATTR_BOLD, 16, 220};

static int put(struct mux *m, int row, int col, int max, const char *text, const struct comp_style *st) {
    return comp_text(m->comp, row, col, max, text, strlen(text), st);
}

static void draw_box(struct mux *m, int y, int x, int h, int w, const char *title) {
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    comp_fill(m->comp, y, x, w, &ST_TITLE);
    put(m, y, x + 1, w - 2, title, &ST_TITLE);
}

static void draw_separators(struct mux *m) {
    int n = win_count(m, m->view);
    if (n < 2 || m->tcols < 3 || m->trows < 3) return;
    int left, ltop, rtop;
    lines_of(m, m->view, m->trows, m->tcols, &left, &ltop, &rtop);
    struct pane *f = focused(m);
    for (int r = 0; r < m->trows; r++) {
        bool near = f && r >= f->row && r < f->row + f->rows;
        comp_put(m->comp, r, left, 0x2502, near ? &ST_SEP_FOCUS : &ST_SEP); /* │ */
    }
    if (n >= 3) { /* under A */
        bool near = f && f->col == 0;
        for (int c = 0; c < left; c++) comp_put(m->comp, ltop, c, 0x2500, near ? &ST_SEP_FOCUS : &ST_SEP); /* ─ */
    }
    if (n == 4) { /* under B */
        bool near = f && f->col > 0;
        for (int c = left + 1; c < m->tcols; c++) comp_put(m->comp, rtop, c, 0x2500, near ? &ST_SEP_FOCUS : &ST_SEP);
    }
    /* where the lines meet: one cross when both columns split at one row */
    if (n == 4 && ltop == rtop) comp_put(m->comp, ltop, left, 0x253c, &ST_SEP); /* ┼ */
    else {
        if (n >= 3) comp_put(m->comp, ltop, left, 0x2524, &ST_SEP); /* ┤ */
        if (n == 4) comp_put(m->comp, rtop, left, 0x251c, &ST_SEP); /* ├ */
    }
}

static void draw_pick(struct mux *m) {
    int w = m->tcols - 8 < 76 ? m->tcols - 8 : 76, h = m->trows - 4 < 20 ? m->trows - 4 : 20;
    if (w < 20 || h < 8) return;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2;
    draw_box(m, y, x, h, w, "Attach Process");
    int list_h = (h - 2) / 2;
    if (m->npicks == 0) put(m, y + 2, x + 2, w - 4, "No other terminal processes available.", &ST_BOX_DIM);
    int first = m->pick_sel >= list_h ? m->pick_sel - list_h + 1 : 0;
    m->pick_x = x, m->pick_w = w, m->pick_top = y + 1, m->pick_first = first, m->pick_rows = list_h;
    for (int i = first; i < m->npicks && i - first < list_h; i++) {
        const struct comp_style *st = i == m->pick_sel ? &ST_SEL : &ST_BOX;
        comp_fill(m->comp, y + 1 + i - first, x + 1, w - 2, st);
        put(m, y + 1 + i - first, x + 2, w - 4, m->picks[i].name, st);
    }
    int iy = y + 1 + list_h;
    for (int c = x; c < x + w; c++) comp_put(m->comp, iy, c, 0x2500, &ST_BOX_DIM);
    if (m->npicks > 0) {
        const struct pick *p = &m->picks[m->pick_sel];
        char line[HOLD_PATH_MAX + 32];
        snprintf(line, sizeof(line), "Program            %s", p->program);
        put(m, iy + 1, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "Arguments          %s", p->args[0] ? p->args : "-");
        put(m, iy + 2, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "Working directory  %s", p->working[0] ? p->working : "-");
        put(m, iy + 3, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "Started            %s", p->started[0] ? p->started : "-");
        put(m, iy + 4, x + 2, w - 4, line, &ST_BOX);
    }
    put(m, y + h - 1, x + 2, w - 4, "Up/Down: select  Enter: attach  Esc: cancel", &ST_BOX_DIM);
}

/* Keyboard help is contextual: prefix commands or the active mode's keys. */
static const char *const keys_prefix[] = {
    "Backmux Controls",
    "", /* prefix instruction, filled in when drawn */
    "Navigation  p  Pane mode        t  Window mode",
    "Layout      r  Resize mode      m  Move mode",
    "            s  Swap panes       z  Toggle full screen",
    "            w  Distribute panes across windows",
    "            a  Sort by name     d  Pack windows",
    "Display     f  Toggle pane footers",
    "Profiles    n  Rename process   ;  Save profile",
    "Input       \\  Toggle pane lock (forward Alt-arrows)",
    "", /* detach command */
    "Chat        c  Open/focus chat; repeat to hide",
    "Help        ?  Keyboard help",
    "", /* repeat prefix to send it to the program */
    "Alt-arrows     Focus adjacent pane, unless locked",
    "Esc            Close help",
    "Details        backmux help panes",
};
static const char *const keys_pane[] = {
    "Pane Navigation",
    "Arrows, h j k l   Focus adjacent pane",
    "a                 Open attach picker",
    "g                 Select pane by name",
    "z                 Toggle active pane full screen",
    "?                 Toggle mode help",
    "Esc               Close help; repeat to leave mode",
    "Enter             Leave mode",
};
static const char *const keys_tab[] = {
    "Window Navigation",
    "1-9               Select occupied window; leave mode",
    "Left/Right, h l   Previous/next occupied window",
    "?                 Toggle mode help",
    "Esc               Close help; repeat to leave mode",
    "Enter             Leave mode",
};
static const char *const keys_resize[] = {
    "Pane Resizing",
    "Arrows, h j k l       Expand toward selected edge",
    "Shift-arrows, H J K L Shrink from selected edge",
    "=                     Reset window dividers to 50%",
    "Each step moves a divider by 5 percentage points.",
    "?                     Toggle mode help",
    "Esc                   Close help; repeat to leave mode",
    "Enter                 Leave mode",
};
static const char *const keys_move[] = {
    "Pane Placement",
    "Arrows, h j k l   Swap with adjacent pane",
    "1-9               Move pane to window and leave mode",
    "View stays unless the source window becomes empty.",
    "?                 Toggle mode help",
    "Esc               Close help; repeat to leave mode",
    "Enter             Leave mode",
};

/* A key as a person reads it: NUL is Ctrl-Space, a control byte Ctrl-X, a
 * printable byte itself. */
static void key_label(unsigned char k, char out[16]) {
    if (k == 0) snprintf(out, 16, "Ctrl-Space");
    else if (k < 0x20) snprintf(out, 16, "Ctrl-%c", k | 0x40);
    else snprintf(out, 16, "%c", k);
}

static void draw_help(struct mux *m) {
    const char *const *lines = keys_prefix;
    int n = (int)(sizeof(keys_prefix) / sizeof(keys_prefix[0]));
#define KEYS(arr) (lines = (arr), n = (int)(sizeof(arr) / sizeof((arr)[0])))
    switch (m->mode) {
    case MODE_PANE: KEYS(keys_pane); break;
    case MODE_TAB: KEYS(keys_tab); break;
    case MODE_RESIZE: KEYS(keys_resize); break;
    case MODE_MOVE: KEYS(keys_move); break;
    default: break;
    }
#undef KEYS
    char pk[16], dk[16], filled[3][72];
    key_label(m->prefix, pk);
    key_label(m->detach_key, dk);
    snprintf(filled[0], sizeof(filled[0]), "Prefix: %s, then a command key", pk);
    snprintf(filled[1], sizeof(filled[1]), "Detach      %-2s Detach active pane; process continues", dk);
    snprintf(filled[2], sizeof(filled[2]), "Literal key %s again sends the prefix", pk);
    int w = 66 < m->tcols - 2 ? 66 : m->tcols - 2, h = n + 2 < m->trows ? n + 2 : m->trows;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2;
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    for (int i = 0, blank = 0; i < n && i + 1 < h; i++) {
        const char *line = lines[i][0] || blank > 2 ? lines[i] : filled[blank++];
        put(m, y + 1 + i, x + 2, w - 4, line, i == 0 ? &ST_TITLE : &ST_BOX);
    }
}

/* Jump: the panes of this terminal by name, with their window's number. */
static void draw_jump(struct mux *m) {
    int rows = 0;
    for (int i = 0; i < MUX_MAX_PANES; i++) rows += m->slot[i] != NULL;
    int w = 48 < m->tcols - 4 ? 48 : m->tcols - 4, h = rows + 2 < m->trows - 2 ? rows + 2 : m->trows - 2;
    if (w < 12 || h < 3) return;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2, k = 0;
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    for (int i = 0; i < MUX_MAX_PANES && k < h - 2; i++) {
        if (!m->slot[i]) continue;
        const struct comp_style *st = k == m->pick_sel ? &ST_SEL : &ST_BOX;
        char line[ALIAS_MAX_LEN + 24];
        snprintf(line, sizeof(line), " %-*s window %d", w - 14, m->slot[i]->name, window_key(i / 4));
        comp_fill(m->comp, y + 1 + k, x + 1, w - 2, st);
        put(m, y + 1 + k, x + 1, w - 2, line, st);
        k++;
    }
}

/* Show the pane's slot letter, name, status, and active prompt in its footer.
 * With footers disabled, temporary notices and prompts overlay the last row. */
static void draw_footer(struct mux *m, struct pane *p, int slot, struct comp_want *want) {
    bool focus = p == focused(m);
    int row = p->frow, col = p->col, w = p->cols;
    if (row < 0 || row >= m->trows || w < 2) return;
    const struct comp_style *st = focus ? &ST_TITLE : &ST_BOX_DIM;
    comp_fill(m->comp, row, col, w, st);
    char letter = slot_letter(slot);
    int x = col;
    if (letter) {
        char lt[4] = {' ', letter, ' ', '\0'};
        bool chosen = m->swapping && m->swap_first == slot;
        x += put(m, row, x, w, lt, m->swapping ? (chosen ? &ST_SEL : &ST_LETTER) : st);
    }
    if (m->prompt != PROMPT_NONE && m->prompt_pane == p) {
        char q[ALIAS_MAX_LEN + 32];
        snprintf(q, sizeof(q), " %s: %s", m->prompt == PROMPT_SAVE ? "Save profile" : "Rename process", m->in);
        int used = put(m, row, x, col + w - x, q, st);
        want->cursor_visible = true;
        want->cursor_row = row;
        want->cursor_col = x + used < col + w ? x + used : col + w - 1;
        return;
    }
    char name[ALIAS_MAX_LEN + 4];
    snprintf(name, sizeof(name), " %s ", p->name);
    x += put(m, row, x, col + w - x, name, st);
    char status[200] = "";
    size_t k = 0;
#define ADD(...) (k += (size_t)snprintf(status + k, k < sizeof(status) ? sizeof(status) - k : 0, __VA_ARGS__))
    if (focus && m->now < m->notice_until) ADD(" %s ", m->notice);
    else {
        if (pane_locked(p)) ADD(" LOCKED ");
        if (m->full == p) ADD(" FULL ");
        if (focus) {
            const char *mode = m->mode == MODE_PANE     ? "PANE"
                               : m->mode == MODE_TAB    ? "WINDOW"
                               : m->mode == MODE_RESIZE ? "RESIZE"
                               : m->mode == MODE_MOVE   ? "MOVE"
                               : m->swapping            ? "SWAP"
                                                        : NULL;
            if (mode) ADD(" %s ", mode);
            int unread = m->chat && !chat_visible(m->chat) ? chat_unread(m->chat) : 0;
            if (unread) ADD(" chat: %d new ", unread);
        }
    }
#undef ADD
    int sl = (int)strlen(status);
    if (sl && col + w - sl > x) put(m, row, col + w - sl, sl, status, st);
}

/* chat/mux.<pid>: which panes this terminal shows, for backmux swap from
 * any shell or agent. "view N" (0-based) then one "SLOT ID NAME" per pane. */
static void advertise(struct mux *m) {
    if (!m->advertise) return;
    m->advertise = false;
    char dir[HOLD_PATH_MAX], path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/chat", m->store->base) != 0 || hold_mkdir_p_mode(dir, 0700) != 0 ||
        hold_checked_snprintf(path, sizeof(path), "%s/mux.%d", dir, (int)getpid()) != 0)
        return;
    char buf[MUX_MAX_PANES * (ID_STR_LEN + ALIAS_MAX_LEN + 8) + 32];
    size_t k = (size_t)snprintf(buf, sizeof(buf), "view %d\n", m->view);
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && k < sizeof(buf))
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%d %s %s\n", i + 1, m->slot[i]->id[0] ? m->slot[i]->id : "-",
                                  m->slot[i]->name);
    write_small(path, buf, k < sizeof(buf) ? k : sizeof(buf) - 1);
}

static void render(struct mux *m) {
    advertise(m);
    if (!m->dirty) return;
    m->dirty = false;
    comp_begin(m->comp, NULL);
    struct pane *f = focused(m);
    int n = win_count(m, m->view);
    bool full = m->full && m->full == f;
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        if (full && p != m->full) continue;
        struct hold_snapshot snap;
        if (hold_screen_snapshot(p->scr, &snap) == 0) {
            comp_blit(m->comp, &snap, p->row, p->col, p->rows, p->cols);
            hold_snapshot_free(&snap);
        }
    }
    if (!full) draw_separators(m);
    /* Pane names and status use footers; help appears only on request. */
    struct comp_want want = {0};
    struct hold_screen_modes modes = {0};
    if (f) hold_screen_modes(f->scr, &modes);
    want.app_cursor_keys = modes.app_cursor_keys;
    want.app_keypad = modes.app_keypad;
    want.bracketed_paste = modes.bracketed_paste;
    /* clicks and drags come here (click to move between panes, chat's
     * window dragged); plain movement only when the focused program asked
     * for it, which it then gets */
    want.mouse = modes.mouse == HOLD_MOUSE_MOVE ? HOLD_MOUSE_MOVE : HOLD_MOUSE_DRAG;
    want.cursor_visible = false;
    if (f) {
        struct hold_snapshot snap;
        if (hold_screen_snapshot(f->scr, &snap) == 0) {
            want.cursor_row = f->row + snap.cursor_row;
            want.cursor_col = f->col + snap.cursor_col;
            want.cursor_visible = snap.modes.cursor_visible;
            hold_snapshot_free(&snap);
        }
        want.cursor_style = hold_screen_cursor_style(f->scr);
        want.title = hold_screen_title(f->scr);
    }
    /* footers: every pane's while they are on; else only a pane with
     * something to show right now */
    bool footers = g_footers && pane_total(m) > 1;
    for (int k = 0; k < n; k++) {
        int slot = m->view * 4 + k;
        struct pane *p = m->slot[slot];
        if (full && p != m->full) continue;
        bool needed = m->swapping || (m->prompt != PROMPT_NONE && m->prompt_pane == p) ||
                      (p == f && m->now < m->notice_until);
        if (footers || needed) draw_footer(m, p, slot, &want);
    }
    /* chat over the panes, then any other floating window over chat: only
     * one is up at a time anyway */
    if (m->chat) chat_draw(m->chat, m->comp, m->now, &want);
    if (m->fl == FL_ATTACH) {
        draw_pick(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_JUMP) {
        draw_jump(m);
        want.cursor_visible = false;
    }
    if (m->help) {
        draw_help(m);
        want.cursor_visible = false;
    }
    if (!f && m->now < m->notice_until) put(m, m->trows - 1, 0, m->tcols, m->notice, &ST_BAD); /* no pane to say it in */
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (comp_flush(m->comp, &want, &buf, &len, &cap) == 0 && len) (void)hold_write_all(STDOUT_FILENO, buf, len);
    free(buf);
}

/* ---- input ------------------------------------------------------------- */

static int slot_at(const struct mux *m, int row, int col) {
    int n = win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        const struct pane *p = m->slot[m->view * 4 + k];
        if (m->full && m->full == focused(m) && p != m->full) continue;
        int bottom = p->frow > p->row + p->rows - 1 ? p->frow : p->row + p->rows - 1; /* its footer too */
        if (row >= p->row && row <= bottom && col >= p->col && col < p->col + p->cols) return m->view * 4 + k;
    }
    return -1;
}

static void floater_key(struct mux *m, const unsigned char *seq, size_t n);
static void swap_key(struct mux *m, const unsigned char *k, size_t n);

/* A pointer report, SGR form: b;x;y then M (press, motion) or m (release). */
static void handle_mouse(struct mux *m, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1;
    bool motion = (b & 32) != 0;
    bool wheel = (b & 64) != 0;
    int button = b & 3;
    if (m->prompt != PROMPT_NONE) return; /* the name is typed, not clicked */
    if (m->swapping) {
        /* a click on a pane picks it, as its letter would */
        int s = slot_at(m, row, col);
        char letter = slot_letter(s);
        if (!motion && !release && !wheel && letter) {
            unsigned char lk = (unsigned char)letter;
            swap_key(m, &lk, 1);
        }
        return;
    }
    if (m->fl == FL_JUMP) {
        /* a click anywhere puts the list away */
        if (!motion && !release) {
            m->fl = FL_NONE;
            m->dirty = true;
        }
        return;
    } else if (m->fl == FL_ATTACH) {
        /* a click on a name attaches it; anywhere else closes the picker */
        if (motion || release) return;
        int k = row - m->pick_top + m->pick_first;
        if (row >= m->pick_top && row < m->pick_top + m->pick_rows && col >= m->pick_x && col < m->pick_x + m->pick_w &&
            k >= 0 && k < m->npicks) {
            static const unsigned char enter = '\r';
            m->pick_sel = k;
            floater_key(m, &enter, 1);
        } else {
            m->fl = FL_NONE;
            m->dirty = true;
        }
        return;
    } else if (m->fl != FL_NONE) {
        return;
    }
    /* A click changes focus; pointer motion alone does not. */
    int s = slot_at(m, row, col);
    if (!motion && !release && !wheel && s >= 0 && s != m->focus) {
        set_focus(m, s);
        return; /* the click that moves focus is not the program's */
    }
    struct pane *p = s >= 0 ? m->slot[s] : NULL;
    if (!p || s != m->focus) return;
    if (row >= p->row + p->rows) return; /* its footer is not the program's */
    struct hold_screen_modes modes;
    hold_screen_modes(p->scr, &modes);
    if (modes.mouse == HOLD_MOUSE_NONE) return;
    if (motion && modes.mouse == HOLD_MOUSE_CLICK) return;
    if (motion && modes.mouse == HOLD_MOUSE_DRAG && button == 3) return;
    enum hold_mouse_event ev = wheel ? (button == 0 ? HOLD_MOUSE_WHEEL_UP : HOLD_MOUSE_WHEEL_DOWN)
                               : motion ? HOLD_MOUSE_MOTION : release ? HOLD_MOUSE_RELEASE : HOLD_MOUSE_PRESS;
    int mods = ((b & 4) ? HOLD_MOD_SHIFT : 0) | ((b & 8) ? HOLD_MOD_ALT : 0) | ((b & 16) ? HOLD_MOD_CTRL : 0);
    hold_screen_discard_output(p->scr);
    hold_screen_mouse_event(p->scr, ev, button == 3 ? 0 : button + 1, row - p->row, col - p->col, mods);
    char enc[64];
    size_t k = hold_screen_take_output(p->scr, enc, sizeof(enc));
    if (k) pane_send(p, enc, k);
}

static void flush_keys(struct mux *m) {
    struct pane *p = focused(m);
    if (m->out_len && p) pane_send(p, m->out, m->out_len);
    m->out_len = 0;
}

static void to_pane(struct mux *m, const unsigned char *b, size_t n) {
    if (m->out_len + n > sizeof(m->out)) flush_keys(m);
    if (n > sizeof(m->out)) {
        struct pane *p = focused(m);
        if (p) pane_send(p, b, n);
        return;
    }
    memcpy(m->out + m->out_len, b, n);
    m->out_len += n;
}

static void open_floater(struct mux *m, enum floater f);

/* A number key as a window: 1-9. -1 if not one. */
static int window_of_key(unsigned char c) {
    if (c >= '1' && c <= '9') return c - '1';
    return -1;
}

static int slot_of(const struct mux *m, const struct pane *p) {
    for (int i = 0; p && i < MUX_MAX_PANES; i++)
        if (m->slot[i] == p) return i;
    return -1;
}

/* Focus stays with the pane, wherever a rearrangement put it. */
static void follow(struct mux *m, struct pane *p) {
    int s = slot_of(m, p);
    if (s >= 0) set_focus(m, s);
}

/* After a pane was sent somewhere: focus stays with it while it is still on
 * this window; sent to another window, the view stays here. Why: sending a pane
 * away is not going after it. */
static void follow_here(struct mux *m, struct pane *p) {
    int s = slot_of(m, p);
    if (s >= 0 && s / 4 == m->view) set_focus(m, s);
    else if (!m->slot[m->focus] || m->focus / 4 != m->view) m->focus = m->view * 4;
    m->dirty = true;
}

/* ---- running backmux itself --------------------------------------------
 * Rename and save are backmux's own verbs, with their checks and records;
 * this layer never reaches into the store's writers, so it runs them. */
static int run_self(char *const args[], char *err, size_t en) {
    if (err && en) err[0] = '\0';
    char self[HOLD_PATH_MAX];
    if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) return -1;
    /* A file, not a pipe: nothing to drain while waiting, and no broker is
     * started that could hold a pipe open. */
    FILE *fe = tmpfile();
    if (!fe) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        fclose(fe);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
        }
        dup2(fileno(fe), STDERR_FILENO);
        int argc = 0;
        while (args[argc]) argc++;
        char **argv = calloc((size_t)argc + 2, sizeof(char *));
        if (!argv) _exit(126);
        argv[0] = "backmux";
        for (int i = 0; i < argc; i++) argv[i + 1] = args[i];
        execv(self, argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
    if (err && en) {
        rewind(fe);
        size_t got = fread(err, 1, en - 1, fe);
        err[got] = '\0';
        err[strcspn(err, "\n")] = '\0';
        const char *strip = "backmux: error: ";
        if (!strncmp(err, strip, strlen(strip))) memmove(err, err + strlen(strip), strlen(err + strlen(strip)) + 1);
    }
    fclose(fe);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---- the prompt: a name for the active pane ----------------------------- */

static void prompt_open(struct mux *m, enum prompt kind) {
    struct pane *p = focused(m);
    if (!p || !p->id[0]) return;
    m->prompt = kind;
    m->prompt_pane = p;
    snprintf(m->in, sizeof(m->in), "%s", p->name);
    m->in_len = strlen(m->in);
    m->dirty = true;
}

/* A changed name uses rename, which also saves the profile. An unchanged
 * save prompt uses save; an unchanged rename prompt has no effect. */
static void prompt_done(struct mux *m) {
    struct pane *p = m->prompt_pane;
    enum prompt kind = m->prompt;
    m->prompt = PROMPT_NONE;
    m->prompt_pane = NULL;
    if (!p || slot_of(m, p) < 0) return;
    m->in[m->in_len] = '\0';
    bool changed = m->in_len > 0 && strcmp(m->in, p->name) != 0;
    char err[200];
    if (changed) {
        char *args[] = {"rename", p->id, m->in, NULL};
        if (run_self(args, err, sizeof(err)) != 0) {
            notice(m, "not renamed: %s", err[0] ? err : "backmux rename failed");
            return;
        }
        snprintf(p->name, sizeof(p->name), "%s", m->in);
        if (m->chat && p == focused(m)) chat_set_screen(m->chat, p->scr, p->name);
        m->advertise = true;
        notice(m, kind == PROMPT_SAVE ? "saved as %s" : "renamed to %s", p->name);
        return;
    }
    if (kind == PROMPT_SAVE) {
        char *args[] = {"save", p->id, NULL};
        if (run_self(args, err, sizeof(err)) != 0) notice(m, "not saved: %s", err[0] ? err : "backmux save failed");
        else notice(m, "saved");
    }
}

static void prompt_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && k[0] == 0x1b) { /* Esc: nothing renamed, nothing saved */
        m->prompt = PROMPT_NONE;
        m->prompt_pane = NULL;
        return;
    }
    if (n == 1 && (k[0] == '\r' || k[0] == '\n')) {
        prompt_done(m);
        return;
    }
    if (n == 1 && (k[0] == 0x7f || k[0] == 0x08)) {
        if (m->in_len > 0) m->in[--m->in_len] = '\0';
        return;
    }
    if (n == 1 && k[0] == 0x15) { /* Ctrl-U */
        m->in_len = 0;
        m->in[0] = '\0';
        return;
    }
    /* a name is short printable text: anything else is not part of one */
    for (size_t i = 0; i < n; i++)
        if (k[i] < 0x21 || k[i] == 0x7f) return;
    if (m->in_len + n < sizeof(m->in) && m->in_len + n <= ALIAS_MAX_LEN) {
        memcpy(m->in + m->in_len, k, n);
        m->in_len += n;
        m->in[m->in_len] = '\0';
    }
}

/* ---- swap: two letters, or a letter and a number ------------------------ */

static void swap_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && (k[0] == 0x1b || k[0] == '\r' || k[0] == '\n')) {
        m->swapping = false;
        return;
    }
    if (n != 1) return;
    int w = window_of_key(k[0]);
    if (w >= 0) { /* a number sends the pane to that window */
        int from = m->swap_first >= 0 ? m->swap_first : m->focus;
        struct pane *p = m->slot[from];
        if (move_pane(m, from, w) != 0) notice(m, "window %d is full", window_key(w));
        else follow_here(m, p);
        m->swapping = false;
        return;
    }
    int s = letter_slot(k[0]);
    if (s < 0) return;
    if (m->swap_first < 0) {
        if (m->slot[s]) m->swap_first = s; /* the first is a pane */
        return;
    }
    struct pane *p = m->slot[m->swap_first];
    if (m->slot[s]) {
        do_swap(m, m->swap_first, s);
    } else if (move_pane(m, m->swap_first, s / 4) != 0) { /* past the panes there: to that window */
        notice(m, "window %d is full", window_key(s / 4));
    }
    follow_here(m, p);
    m->swapping = false;
}

/* ---- rearranging: every pane, in slot order ----------------------------- */

static int by_pane_name(const void *a, const void *b) {
    return strcasecmp((*(struct pane *const *)a)->name, (*(struct pane *const *)b)->name);
}

enum arrange { ARRANGE_SPREAD, ARRANGE_ALPHA, ARRANGE_DENSE };

static void arrange(struct mux *m, enum arrange how) {
    struct pane *all[MUX_MAX_PANES], *active = focused(m);
    int pos[MUX_MAX_PANES], n = 0;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) {
            pos[n] = i;
            all[n++] = m->slot[i];
            m->slot[i] = NULL;
        }
    if (how == ARRANGE_ALPHA) {
        /* the same places, filled in name order */
        qsort(all, (size_t)n, sizeof(all[0]), by_pane_name);
        for (int i = 0; i < n; i++) m->slot[pos[i]] = all[i];
    } else if (how == ARRANGE_DENSE) {
        /* four to a window from the first: 12 fill windows 1-3 */
        for (int i = 0; i < n; i++) m->slot[i] = all[i];
    } else {
        /* every window used before any splits: 12 put two in each of windows 1-3
         * and one in each of windows 4-9 */
        int k = 0;
        for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
            int here = n / MUX_MAX_WINDOWS + (w < n % MUX_MAX_WINDOWS);
            for (int j = 0; j < here && j < 4; j++) m->slot[w * 4 + j] = all[k++];
        }
    }
    relayout(m);
    if (active) follow(m, active);
    m->dirty = true;
}

/* ---- modes ---------------------------------------------------------------- */

/* An arrow, plain or with a modifier: 'A' up 'B' down 'C' right 'D' left,
 * *mod 1 plain, 2 Shift, 3 Alt; h j k l as arrows (H J K L with Shift). 0 if
 * not one. */
static char arrow_of(const unsigned char *k, size_t n, int *mod, bool letters) {
    *mod = 1;
    if (n == 3 && k[0] == 0x1b && (k[1] == '[' || k[1] == 'O') && k[2] >= 'A' && k[2] <= 'D') return (char)k[2];
    if (n == 6 && k[0] == 0x1b && k[1] == '[' && k[2] == '1' && k[3] == ';' && k[5] >= 'A' && k[5] <= 'D') {
        *mod = k[4] - '0';
        return (char)k[5];
    }
    if (n == 4 && k[0] == 0x1b && k[1] == 0x1b && k[2] == '[' && k[3] >= 'A' && k[3] <= 'D') { /* Esc-prefixed: Alt */
        *mod = 3;
        return (char)k[3];
    }
    if (!letters || n != 1) return 0;
    static const char from[] = "hjklHJKL", to[] = "DBACDBAC";
    const char *hit = strchr(from, k[0]);
    if (!hit || !k[0]) return 0;
    *mod = hit - from >= 4 ? 2 : 1;
    return to[hit - from];
}

/* Adjacent pane in the requested direction; -1 if none. */
static int neighbor(const struct mux *m, char dir) {
    struct pane *f = focused(m);
    if (!f || m->full) return -1;
    int best = -1, bestd = INT_MAX, n = win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        int s = m->view * 4 + k;
        const struct pane *p = m->slot[s];
        if (p == f) continue;
        bool rows = p->row < f->row + f->rows && f->row < p->row + p->rows;
        bool cols = p->col < f->col + f->cols && f->col < p->col + p->cols;
        int d = INT_MAX;
        if (dir == 'D' && rows && p->col + p->cols <= f->col) d = f->col - (p->col + p->cols);
        if (dir == 'C' && rows && p->col >= f->col + f->cols) d = p->col - (f->col + f->cols);
        if (dir == 'A' && cols && p->row + p->rows <= f->row) d = f->row - (p->row + p->rows);
        if (dir == 'B' && cols && p->row >= f->row + f->rows) d = p->row - (f->row + f->rows);
        if (d < bestd) bestd = d, best = s;
    }
    return best;
}

static int prev_window(const struct mux *m, int w) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = (w - i + MUX_MAX_WINDOWS) % MUX_MAX_WINDOWS;
        if (win_count(m, c) > 0) return c;
    }
    return w;
}

static int clamp_pct(int v) {
    return v < MUX_PCT_MIN ? MUX_PCT_MIN : v > MUX_PCT_MAX ? MUX_PCT_MAX : v;
}

/* Resize: the active pane grows toward a side, or shrinks from it, by
 * moving the line on that side; a side with no line (the screen's edge) does
 * nothing. */
static void resize_toward(struct mux *m, char dir, bool grow) {
    struct pane *f = focused(m);
    int w = m->view, n = win_count(m, w);
    if (!f || m->full || n < 2) return;
    int step = grow ? MUX_RESIZE_STEP : -MUX_RESIZE_STEP;
    bool left_col = f->col == 0;
    if (dir == 'C' && left_col) m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) + step);
    if (dir == 'D' && !left_col) m->colpct[w] = clamp_pct(pct_or_half(m->colpct[w]) - step);
    int *col = left_col ? &m->lpct[w] : &m->rpct[w];
    bool split = left_col ? n >= 3 : n == 4;
    bool top = f->row == 0;
    if (split && dir == 'B' && top) *col = clamp_pct(pct_or_half(*col) + step);
    if (split && dir == 'A' && !top) *col = clamp_pct(pct_or_half(*col) - step);
    relayout(m);
}

static void leave_mode(struct mux *m) {
    m->mode = MODE_NONE;
    m->help = false;
    m->dirty = true;
}

static void mode_key(struct mux *m, const unsigned char *k, size_t n) {
    m->dirty = true;
    if (n == 1 && (k[0] == 0x1b || k[0] == '\r' || k[0] == '\n')) {
        if (m->help && k[0] == 0x1b) m->help = false; /* Close help before leaving the mode. */
        else leave_mode(m);
        return;
    }
    if (n == 1 && k[0] == '?') {
        m->help = !m->help;
        return;
    }
    m->help = false;
    int mod = 1;
    char dir = arrow_of(k, n, &mod, true);
    int w = n == 1 ? window_of_key(k[0]) : -1;
    struct pane *f = focused(m);
    switch (m->mode) {
    case MODE_PANE:
        if (dir) {
            int s = neighbor(m, dir);
            if (s >= 0) set_focus(m, s);
        } else if (n == 1 && k[0] == 'a') {
            leave_mode(m);
            open_floater(m, FL_ATTACH);
        } else if (n == 1 && k[0] == 'g') {
            leave_mode(m);
            open_floater(m, FL_JUMP);
        } else if (n == 1 && k[0] == 'z' && f && win_count(m, m->view) > 1) {
            m->full = m->full == f ? NULL : f;
            relayout(m);
        }
        return;
    case MODE_TAB:
        if (w >= 0) {
            if (win_count(m, w) == 0) notice(m, "window %d is empty", window_key(w));
            else {
                set_focus(m, w * 4);
                leave_mode(m);
            }
        } else if (dir == 'D' || dir == 'C') {
            int to = dir == 'D' ? prev_window(m, m->view) : next_window(m, m->view);
            set_focus(m, to * 4);
        }
        return;
    case MODE_RESIZE:
        if (dir) resize_toward(m, dir, mod != 2);
        else if (n == 1 && k[0] == '=') {
            m->colpct[m->view] = m->lpct[m->view] = m->rpct[m->view] = 0;
            relayout(m);
        }
        return;
    case MODE_MOVE:
        if (dir) {
            int s = neighbor(m, dir);
            if (s >= 0 && f) {
                do_swap(m, m->focus, s);
                follow(m, f);
            }
        } else if (w >= 0 && f) {
            if (move_pane(m, m->focus, w) != 0) notice(m, "window %d is full", window_key(w));
            else {
                follow_here(m, f);
                leave_mode(m);
            }
        }
        return;
    default:
        return;
    }
}

/* Handle input for the attach or pane-name picker. seq is one byte or one
 * complete escape sequence. */
static void floater_key(struct mux *m, const unsigned char *seq, size_t n) {
    bool esc = n == 1 && seq[0] == 0x1b;
    bool up = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'A';
    bool down = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'B';
    bool enter = n == 1 && (seq[0] == '\r' || seq[0] == '\n');
    m->dirty = true;
    if (esc) {
        m->fl = FL_NONE;
        return;
    }
    switch (m->fl) {
    case FL_ATTACH:
        if (up && m->pick_sel > 0) {
            m->pick_sel--;
        } else if (down && m->pick_sel + 1 < m->npicks) {
            m->pick_sel++;
        } else if (enter && m->npicks > 0) {
            const struct pick *p = &m->picks[m->pick_sel];
            const char *why = NULL;
            if (pane_total(m) >= MUX_MAX_PANES) {
                notice(m, "pane limit reached (%d)", MUX_MAX_PANES);
            } else {
                struct pane *np = pane_open(p->id, p->name, p->sock, m->trows, m->tcols, &why);
                if (!np) notice(m, "cannot attach %s: %s", p->name, why ? why : "console unavailable");
                else (void)add_pane(m, np);
            }
            m->fl = FL_NONE;
        }
        return;
    case FL_JUMP: {
        int rows = 0;
        for (int i = 0; i < MUX_MAX_PANES; i++) rows += m->slot[i] != NULL;
        if (up && m->pick_sel > 0) m->pick_sel--;
        else if (down && m->pick_sel + 1 < rows) m->pick_sel++;
        else if (enter) {
            for (int i = 0, k = 0; i < MUX_MAX_PANES; i++) {
                if (!m->slot[i]) continue;
                if (k++ == m->pick_sel) set_focus(m, i);
            }
            m->fl = FL_NONE;
        }
        return;
    }
    default:
        return;
    }
}

static void open_floater(struct mux *m, enum floater f) {
    if (m->chat && f != FL_NONE) chat_put_away(m->chat); /* one floating window at a time */
    /* The attach picker is the way to a split, and a split takes the whole
     * screen: inline, it goes to the alternate screen before it draws, so
     * the person's own screen - their lines above the launch, which no pane
     * holds - is never drawn over and comes back as it was on leaving. */
    if (f == FL_ATTACH && m->assumed) go_alt(m);
    m->pick_sel = 0;
    if (f == FL_ATTACH) load_picks(m);
    m->fl = f;
    m->dirty = true;
}

/* Dispatch a key following the prefix. Return 1 to detach the last pane.
 * Unrecognized command keys are consumed; repeating the prefix forwards it. */
static int command(struct mux *m, unsigned char key) {
    struct pane *f = focused(m);
    m->help = false;
    m->dirty = true;
    if (key == m->prefix) { /* the prefix twice: one is the program's, locked or not */
        to_pane(m, &key, 1);
        return 0;
    }
    if (key == m->detach_key) {
        /* Detach acts on the active pane, as every pane key does: it lets
         * go and the rest close up (one left is plain attach again). Only
         * the last one leaves the terminal. */
        if (pane_total(m) > 1) {
            if (!f) return 0;
            (void)frame(f->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            m->slot[m->focus] = NULL;
            pane_free(f);
            settle(m);
            return 0;
        }
        return 1;
    }
    switch (key) {
    case 'c':
        chat_key_pressed(m);
        return 0;
    case '?':
        m->help = true;
        return 0;
    case 's':
        m->swapping = true;
        m->swap_first = -1;
        return 0;
    case 'z':
        if (f && win_count(m, m->view) > 1) {
            m->full = m->full == f ? NULL : f;
            relayout(m);
        }
        return 0;
    case 'f':
        g_footers = !g_footers;
        relayout(m);
        return 0;
    case 'n':
        prompt_open(m, PROMPT_RENAME);
        return 0;
    case ';':
        prompt_open(m, PROMPT_SAVE);
        return 0;
    case '\\':
        if (f) set_locked(f, !pane_locked(f));
        return 0;
    case 'w':
        arrange(m, ARRANGE_SPREAD);
        return 0;
    case 'a':
        arrange(m, ARRANGE_ALPHA);
        return 0;
    case 'd':
        arrange(m, ARRANGE_DENSE);
        return 0;
    case 'p':
        m->mode = MODE_PANE;
        return 0;
    case 't':
        m->mode = MODE_TAB;
        return 0;
    case 'r':
        m->mode = MODE_RESIZE;
        return 0;
    case 'm':
        m->mode = MODE_MOVE;
        return 0;
    default:
        return 0;
    }
}

/* Length of a recognized escape sequence, or 0 when incomplete. A lone
 * ESC at a read boundary is treated as the Esc key. */
static size_t esc_len(const unsigned char *s, size_t n, bool at_end) {
    if (n < 2) return at_end ? 1 : 0;
    if (s[1] == '[') {
        for (size_t i = 2; i < n; i++)
            if (s[i] >= 0x40 && s[i] <= 0x7e) return i + 1;
        return n >= 32 ? n : 0;
    }
    if (s[1] == 'O') return n >= 3 ? 3 : 0;
    if (s[1] == 0x1b && n >= 3 && s[2] == '[') { /* Esc-prefixed arrow: Alt on some terminals */
        size_t k = esc_len(s + 1, n - 1, at_end);
        return k ? k + 1 : 0;
    }
    return 2; /* Alt and a key */
}

/* One key, not the prefix's: whoever has the keyboard gets it. */
static void key_in(struct mux *m, const unsigned char *k, size_t n) {
    if (m->prompt != PROMPT_NONE) {
        prompt_key(m, k, n);
    } else if (m->swapping) {
        swap_key(m, k, n);
    } else if (m->fl != FL_NONE) {
        floater_key(m, k, n);
    } else if (m->mode != MODE_NONE) {
        mode_key(m, k, n);
    } else if (m->help) {
        m->help = false; /* any key puts the keys away */
        m->dirty = true;
    } else if (m->chat && chat_has_keyboard(m->chat)) {
        chat_input(m->chat, k, n, m->now);
    } else {
        /* Alt-arrows move between panes, without the prefix - unless the
         * pane is locked (its program gets them), or there is no pane that
         * way (then they are the program's too). */
        int mod = 1;
        char dir = arrow_of(k, n, &mod, false);
        int s = dir && mod == 3 && !pane_locked(focused(m)) ? neighbor(m, dir) : -1;
        if (s >= 0) {
            flush_keys(m);
            set_focus(m, s);
        } else {
            to_pane(m, k, n);
        }
    }
}

/* Returns 1 to detach. */
static int handle_input(struct mux *m, const unsigned char *buf, size_t n) {
    unsigned char all[4096 + 64];
    size_t total = 0;
    memcpy(all, m->esc, m->esc_len);
    total = m->esc_len;
    size_t take = n < sizeof(all) - total ? n : sizeof(all) - total;
    memcpy(all + total, buf, take);
    total += take;
    m->esc_len = 0;
    int rc = 0;
    for (size_t i = 0; i < total && !rc;) {
        unsigned char b = all[i];
        if (b == 0x1b) {
            size_t k = esc_len(all + i, total - i, true);
            if (k == 0) {
                size_t left = total - i < sizeof(m->esc) ? total - i : sizeof(m->esc);
                memcpy(m->esc, all + i, left);
                m->esc_len = left;
                break;
            }
            const unsigned char *s = all + i;
            i += k;
            /* a pointer report: always ours to route, and never the key the
             * prefix waits for */
            if (k > 3 && s[1] == '[' && s[2] == '<') {
                int mb = 0, mx = 0, my = 0;
                char tmp[40];
                size_t tl = k - 3 < sizeof(tmp) - 1 ? k - 3 : sizeof(tmp) - 1;
                memcpy(tmp, s + 3, tl);
                tmp[tl] = '\0';
                if (sscanf(tmp, "%d;%d;%d", &mb, &mx, &my) == 3) {
                    flush_keys(m);
                    /* chat picks out what is its own (its window, a drag
                     * of it) and hands the rest back */
                    if (m->fl == FL_NONE && !m->swapping && m->prompt == PROMPT_NONE && m->chat)
                        chat_input(m->chat, s, k, m->now);
                    else handle_mouse(m, mb, mx, my, s[k - 1] == 'm');
                }
                continue;
            }
            if (m->lead) {
                m->lead = false; /* a sequence after the prefix: cancelled */
                m->dirty = true;
                continue;
            }
            key_in(m, s, k);
            continue;
        }
        size_t k = 1;
        if (b >= 0xc0) { /* a UTF-8 character, whole */
            k = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
            if (k > total - i) k = total - i;
        }
        const unsigned char *s = all + i;
        i += k;
        if (m->lead) {
            m->lead = false;
            m->dirty = true;
            flush_keys(m);
            rc = k == 1 ? command(m, b) : 0;
            continue;
        }
        if (k == 1 && b == m->prefix) {
            m->lead = true; /* waits for the next key, however long it takes */
            continue;
        }
        key_in(m, s, k);
    }
    flush_keys(m);
    if (m->chat && chat_wants_frame(m->chat)) m->dirty = true;
    return rc;
}

/* ---- the loop ---------------------------------------------------------- */

static void drop_ended(struct mux *m) {
    bool any = false;
    for (int i = 0; i < MUX_MAX_PANES; i++) {
        struct pane *p = m->slot[i];
        if (p && p->ended) {
            /* A missing endpoint confirms console closure, not process exit.
             * An existing endpoint may belong to a replacement attach. */
            if (!hold_path_exists(p->sock)) notice(m, "%s: console closed", p->name);
            pane_free(p);
            m->slot[i] = NULL;
            any = true;
        }
    }
    if (any) settle(m);
}

int hold_mux_run(const struct hold_store *store, const char *chat_name, const char *first_sock,
                 unsigned char cmd, const unsigned char *rest, size_t rest_len, char *handback, size_t hn,
                 bool *handed_back) {
    *handed_back = false;
    struct mux *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    m->store = store;
    snprintf(m->me, sizeof(m->me), "%s", chat_name ? chat_name : "");
    unsigned char keys[8];
    bool two = hold_console_get_detach_keys(keys, sizeof(keys)) == 2;
    m->prefix = two ? keys[0] : KEY_PREFIX;
    m->detach_key = two ? keys[1] : 'q';
    m->comp = comp_new();
    m->swap_first = -1;
    m->now = mono_ms();
    struct winsize ws;
    if (hold_term_size(&ws) == 0 && ws.ws_row && ws.ws_col) {
        m->trows = ws.ws_row;
        m->tcols = ws.ws_col;
    } else {
        m->trows = 24;
        m->tcols = 80;
    }
    comp_set_size(m->comp, m->trows, m->tcols);
    comp_invalidate(m->comp);
    if (m->me[0]) m->inbox_off = hold_inbox_size(store, m->me);
    if (m->me[0]) {
        struct chat_host host = {
            .screen = NULL, .master = -1, .store = store, .run_id = NULL, .program = true,
            .person = m->me, .app = m, .to_app = chat_to_app, .app_pointer = chat_pointer,
            .lead = m->prefix, .key = 'c', /* what its tips name: Ctrl-Space c */
        };
        m->chat = chat_new(&host);
        if (m->chat) {
            chat_set_on_sent(m->chat, on_sent);
            chat_ready(m->chat, m->trows, m->tcols, m->now);
        }
    }
    int rc = 0;

    /* the process this started on: its record names it */
    struct find_first {
        const char *sock;
        char id[ID_STR_LEN], name[ALIAS_MAX_LEN + 1];
    } ff = {.sock = first_sock};
    struct hold_run_record r;
    char dir_path[HOLD_PATH_MAX];
    (void)dir_path;
    {
        DIR *d = opendir(store->record_dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".json") || l - 5 >= sizeof(ff.id)) continue;
            char id[ID_STR_LEN];
            snprintf(id, sizeof(id), "%.*s", (int)(l - 5), e->d_name);
            char path[HOLD_PATH_MAX];
            if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) continue;
            if (r.has_console && !strcmp(r.console_sock, first_sock)) {
                snprintf(ff.id, sizeof(ff.id), "%s", r.id);
                snprintf(ff.name, sizeof(ff.name), "%s", r.has_name ? r.name : "");
                if (!ff.name[0]) snprintf(ff.name, sizeof(ff.name), "%.12s", r.id);
            }
            hold_free_run_record(&r);
            if (ff.id[0]) break;
        }
        if (d) closedir(d);
    }
    const char *why = NULL;
    struct pane *first = pane_open(ff.id[0] ? ff.id : "", ff.name[0] ? ff.name : "process", first_sock, m->trows, m->tcols, &why);
    if (!first) {
        free(m);
        return 3;
    }
    (void)add_pane(m, first);
    if (m->chat) chat_set_screen(m->chat, first->scr, first->name);
    /* Inline, the terminal shows this pane's screen already (and the
     * person's lines above where the launch began): start from that, so
     * chat paints only its own window instead of wiping the screen. */
    if (hold_console_was_inline()) {
        struct hold_snapshot snap;
        if (hold_screen_snapshot(first->scr, &snap) == 0) {
            comp_assume(m->comp, &snap);
            hold_snapshot_free(&snap);
            m->assumed = true;
        }
    }
    struct hold_term_signals sig;
    hold_term_signals_install(&sig, true);
    if (command(m, cmd) == 1) goto detach;
    if (rest_len && handle_input(m, rest, rest_len) == 1) goto detach;

    for (;;) {
        m->now = mono_ms();
        if (hold_term_stop_signal()) {
            rc = 128 + hold_term_stop_signal();
            break;
        }
        if (hold_term_take_resized() && hold_term_size(&ws) == 0 && ws.ws_row && ws.ws_col) {
            m->trows = ws.ws_row;
            m->tcols = ws.ws_col;
            comp_set_size(m->comp, m->trows, m->tcols);
            if (m->chat) chat_terminal_resized(m->chat, m->trows, m->tcols);
            relayout(m);
        }
        (void)hold_term_take_interrupt(); /* Ctrl-C arrives as a key for the focused pane */
        drop_ended(m);
        if (pane_total(m) == 0) break; /* every pane has ended */
        /* One pane left and nothing floating: back to plain attach, the
         * screen exactly as it is without panes. */
        if (pane_total(m) == 1 && m->fl == FL_NONE && !m->lead && m->mode == MODE_NONE && !m->help && !m->swapping &&
            m->prompt == PROMPT_NONE && m->now >= m->notice_until && !m->full &&
            !(m->chat && chat_on_screen(m->chat, m->now))) {
            /* the one pane left: windows keep their numbers, so it need not
             * be in the first */
            struct pane *p = NULL;
            for (int i = 0; i < MUX_MAX_PANES && !p; i++) p = m->slot[i];
            /* the last frame, with nothing over the program: inline nothing
             * repaints after this, so nothing of chat may be left on it */
            m->dirty = true;
            render(m);
            snprintf(handback, hn, "%s", p->sock);
            hold_console_set_attached_name(p->name); /* a detach names it */
            /* The mouse reporting this compositor asked for goes off before
             * plain attach takes over: until the program's own repaint
             * arrives, every movement would be typed into it. */
            (void)hold_write_all(STDOUT_FILENO, "\033[?1000l\033[?1002l\033[?1003l\033[?1006l", 32);
            (void)frame(p->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            *handed_back = true;
            break;
        }
        /* backmux swap, from anywhere: a request file names two panes */
        char req_path[HOLD_PATH_MAX], req[256];
        if (hold_checked_snprintf(req_path, sizeof(req_path) - 8, "%s/chat/mux.%d.swap", store->base, (int)getpid()) == 0 &&
            read_small(req_path, req, sizeof(req))) {
            unlink(req_path);
            char a[96] = "", b[96] = "", why2[160] = "bad request";
            int ok = -1;
            /* by name or id only: whoever asks cannot see this screen */
            if (sscanf(req, "%95s %95s", a, b) == 2) {
                int sa = slot_of_name(m, a), sb = slot_of_name(m, b);
                if (sa < 0 || sb < 0) snprintf(why2, sizeof(why2), "%s is not a pane in that terminal", sa < 0 ? a : b);
                else {
                    do_swap(m, sa, sb);
                    ok = 0;
                }
            }
            if (ok != 0) notice(m, "swap: %s", why2);
            /* the answer, for the backmux swap waiting on it */
            char res_path[HOLD_PATH_MAX], res[200];
            int rl = ok == 0 ? snprintf(res, sizeof(res), "ok\n") : snprintf(res, sizeof(res), "error %s\n", why2);
            if (hold_checked_snprintf(res_path, sizeof(res_path) - 8, "%s/chat/mux.%d.result", store->base, (int)getpid()) == 0)
                write_small(res_path, res, (size_t)rl);
        }
        chat_look(m);
        render(m);

        struct pollfd pfds[1 + MUX_MAX_PANES + CHAT_MAX_OUTBOUND];
        int idx[MUX_MAX_PANES];
        nfds_t nf = 0;
        pfds[nf++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        for (int i = 0; i < MUX_MAX_PANES; i++) {
            idx[i] = -1;
            if (m->slot[i]) {
                idx[i] = (int)nf;
                pfds[nf++] = (struct pollfd){.fd = m->slot[i]->rfd, .events = POLLIN};
            }
        }
        /* chat's sends in flight, and its timers (a lone Esc, a hint) */
        int sidx = (int)nf, snf = m->chat ? chat_poll_fds(m->chat, pfds + nf, CHAT_MAX_OUTBOUND) : 0;
        nf += (nfds_t)snf;
        int timeout = m->dirty ? MUX_FRAME_MS : MUX_LOOK_MS;
        if (m->chat) {
            int64_t due = chat_due_in(m->chat, m->now);
            if (due >= 0 && due < timeout) timeout = (int)due;
        }
        int pr = poll(pfds, nf, timeout);
        if (pr < 0 && errno != EINTR) {
            rc = 1;
            break;
        }
        m->now = mono_ms();
        if (m->chat) {
            if (pr > 0 && snf > 0) chat_poll_done(m->chat, pfds + sidx, snf, m->now);
            chat_tick(m->chat, m->now);
            if (chat_wants_frame(m->chat)) m->dirty = true;
        }
        bool probe = m->now >= m->fifo_probe_at;
        if (probe) m->fifo_probe_at = m->now + MUX_FIFO_PROBE_MS;
        if (pr <= 0) m->dirty = true; /* timers: names that expire, notices */
        for (int i = 0; i < MUX_MAX_PANES; i++) {
            if (idx[i] < 0) continue;
            struct pane *p = m->slot[i];
            /* a FIFO pane (separate read and write ends) is read on the probe
             * too: the rfd is non-blocking, so nothing there is EAGAIN */
            bool fifo_probe = probe && p->rfd != p->wfd;
            if (!(pfds[idx[i]].revents & (POLLIN | POLLHUP | POLLERR)) && !fifo_probe) continue;
            char buf[16384];
            ssize_t k = read(p->rfd, buf, sizeof(buf));
            if (k > 0) {
                hold_screen_feed(p->scr, buf, (size_t)k);
                hold_screen_discard_output(p->scr); /* its broker answers queries */
                (void)hold_screen_take_bells(p->scr);
                if (i / 4 == m->view) m->dirty = true;
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                p->ended = true;
            }
        }
        if (pr <= 0) continue;
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buf[4096];
            ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
            if (k > 0) {
                if (handle_input(m, buf, (size_t)k) == 1) goto detach;
            } else if (k == 0) {
                goto detach;
            }
        }
    }
    goto out;
detach:
    /* Detach: every pane lets go; every process keeps running. Which ones
     * is said once the terminal is the person's again. */
    hold_console_note_detached(NULL);
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) {
            (void)frame(m->slot[i]->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            hold_console_note_detached(m->slot[i]->name);
        }
    rc = 0;
out:
    hold_term_signals_restore(&sig);
    for (int i = 0; i < MUX_MAX_PANES; i++) pane_free(m->slot[i]);
    chat_free(m->chat);
    comp_free(m->comp);
    char mux_path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(mux_path, sizeof(mux_path), "%s/chat/mux.%d", store->base, (int)getpid()) == 0)
        unlink(mux_path);
    free(m);
    if (rc > 128 && !*handed_back) {
        hold_console_term_release();
        hold_term_reraise_stop();
    }
    return rc;
}
