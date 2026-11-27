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
 * a third pane is added. The letters above illustrate window 1.
 *
 * The bar along the bottom (prefix f hides it) is the mouse's way to
 * everything the keys do: counters that open the process launcher, the
 * person's chat, Info, the windows, toggles, and the machine's load. Info
 * (prefix i) gives each pane a name bar - its name in a box in the middle,
 * what it runs and its load at the right - drawn on the line above the pane
 * (the split line, where there is one), so it costs a row only at the top
 * of the screen. Both show while the screen is the compositor's for good -
 * backmux shell, or more than one pane - never for a moment's chat over one
 * program, whose size would change twice for nothing. */
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
#define MUX_STATS_MS 1000 /* how often the counts, the load and the panes' stats are read */
#define MUX_BAR_ITEMS 40

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
    bool ended;
    char held[96];            /* opening bytes that may be a refusal */
    size_t held_len;
    bool heard;
    int brow;                 /* its name bar's row: the line above it, or its own first row at the top */
    int64_t detach_ms;        /* the detach keys went to its broker then (0 never): its closing is the detach */
    int box_col, box_w;       /* the name box in that bar, as last drawn (a click there renames) */
    /* what it runs, for its name bar and chat: its record's pid, the
     * harness it is (agents.c) or its program's name, and its load */
    pid_t pid;
    char agent[AGENT_NAME_MAX];
    char program[64];
    uint64_t cpu_prev;
    int64_t cpu_at;
    int cpu_pct;  /* -1 not known */
    uint64_t rss; /* bytes, 0 not known */
};

/* FL_PROCS: the process launcher - running terminals to attach as panes,
 * consoles that scroll to open in a viewer pane, ended processes to start
 * again. FL_JUMP: the panes by name. */
enum floater { FL_NONE, FL_PROCS, FL_JUMP };
enum pick_cat { PK_TTY, PK_SCROLL, PK_ENDED, PK_CATS };

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
    char agent[AGENT_NAME_MAX];
    bool tty, pipe, shell; /* how it ran: on a PTY, on pipes (-i), a backmux shell */
    int exit_code;         /* ended: how, -1 not known */
};

/* A clickable stretch of the bar, as last drawn. */
enum bar_act { BA_NONE, BA_TTY, BA_SCROLL, BA_ENDED, BA_CHAT, BA_INFO, BA_WINDOW, BA_ZOOM, BA_LOCK, BA_HELP };
struct bar_hit {
    int x0, x1;
    enum bar_act act;
    int arg;
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
    /* the launcher's list, as last drawn (a click picks a row, or a category) */
    int pick_x, pick_w, pick_top, pick_first, pick_rows, pick_tabs_row;
    int pick_tab_x[PK_CATS + 1];
    /* the floating window that is not chat */
    enum floater fl;
    enum pick_cat pick_cat;
    struct pick picks[MUX_PICK_MAX];
    int npicks, pick_sel;
    /* backmux shell: the compositor from the start, for good (never handed
     * back to plain attach), with the bar and chat */
    bool resident;
    int arows; /* the rows the panes share: the terminal's, less the bar's */
    /* the bar: its counters, the machine's load, and where its buttons are */
    int n_tty, n_scroll, n_ended;
    uint64_t sys_busy, sys_total;
    int sys_cpu;                 /* percent, -1 not known */
    uint64_t mem_total, mem_used; /* bytes, 0 not known */
    int64_t next_stats;
    struct bar_hit hits[MUX_BAR_ITEMS];
    int nhits;
    bool in_fresh; /* the name prompt shows the name as it is: the first key typed replaces it */
    struct pane *app; /* the pane Assist asks about: the last one in focus that is not an agent */
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
static bool g_bar = true;  /* prefix f: the bar along the bottom */
static bool g_info = true; /* prefix i, or Info on the bar: every pane's name bar and stats */
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

/* Whether the screen is the compositor's for good: backmux shell, or more
 * than one pane. Only then do the bar and the name bars show. Why: with one
 * program and chat up for a moment, a row taken and given back would make
 * the program redraw twice for nothing. */
static bool decorated(const struct mux *m) {
    return m->resident || pane_total(m) > 1;
}

static bool bar_on(const struct mux *m) {
    return g_bar && decorated(m) && m->trows > 3;
}

static bool info_on(const struct mux *m) {
    return g_info && decorated(m);
}

static void relayout(struct mux *m) {
    /* An inline terminal still shows the person's lines above the launch,
     * which no pane holds: once a second pane splits the screen it is all
     * ours - on the alternate screen, so leaving gives theirs back - and
     * the next frame paints all of it. (Before the panes are resized: the
     * last frame on the person's screen is the pane as it was.) */
    if (m->assumed && pane_total(m) > 1) go_alt(m);
    m->arows = m->trows - (bar_on(m) ? 1 : 0);
    if (m->chat) chat_terminal_resized(m->chat, m->arows, m->tcols); /* chat stays clear of the bar */
    /* A name bar sits on the line above its pane: the split line under the
     * panes above, or - at the top of the screen - the pane's own first row,
     * which Info takes from it. */
    bool info = info_on(m);
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        int n = win_count(m, w);
        for (int k = 0; k < n; k++) {
            struct pane *p = m->slot[w * 4 + k];
            int r, c, h, wd;
            if (p == m->full) r = 0, c = 0, h = m->arows, wd = m->tcols;
            else place(m, w, n, k, m->arows, m->tcols, &r, &c, &h, &wd);
            p->brow = r > 0 ? r - 1 : r;
            if (info && r == 0 && h > 1) {
                r++;
                h--;
            }
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
    /* chat talks to the agent you are in: moving to an agent's pane while
     * chat is up moves the conversation with you (Tab still reaches anyone
     * else). Any other pane is an app: the one Assist asks about. */
    struct pane *to = m->slot[slot];
    if (m->chat && m->focus != slot) {
        if (to->agent[0]) chat_show_call(m->chat, to->id, to->name);
        else {
            m->app = to;
            chat_set_screen(m->chat, to->scr, to->name);
        }
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
    bool app_here = false;
    for (int i = 0; i < MUX_MAX_PANES && !app_here; i++) app_here = m->app && m->slot[i] == m->app;
    if (!app_here) m->app = f && !f->agent[0] ? f : NULL;
    if (m->chat) {
        struct pane *a = m->app ? m->app : f;
        if (a) chat_set_screen(m->chat, a->scr, a->name);
    }
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

/* ---- the launcher, and the bar's counters -------------------------------- */

/* Whether a record's process runs now. A console's broker and program must
 * both be there (hold_console_record_alive); a process without a console is
 * taken at its pid's word. Ended: it says so, or it is gone. */
static bool record_running(const struct hold_run_record *r) {
    if (r->has_exit_code || r->has_ended_at || (r->has_state && strcmp(r->state, "running") != 0)) return false;
    if (r->has_console) return hold_path_exists(r->console_sock) && hold_console_record_alive(r);
    return r->pid > 1 && (kill(r->pid, 0) == 0 || errno == EPERM);
}

/* Which list of the launcher a record belongs on, -1 none. Terminals can be
 * panes (a pane shows a screen); a console that scrolls - a program on pipes,
 * or a server's log - is opened in a viewer pane; an ended process can be
 * started again. */
static int record_cat(const struct hold_run_record *r) {
    if (!record_running(r)) return PK_ENDED;
    if (r->has_console && r->recipe.mode_tty) return PK_TTY;
    return PK_SCROLL;
}

struct pick_walk {
    struct mux *m;
    bool fill; /* fill m->picks for m->pick_cat, besides counting */
};

static int pick_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct pick_walk *wk = ctx;
    struct mux *m = wk->m;
    if (!r) return 0;
    int cat = record_cat(r);
    /* the process this terminal itself runs in: shown inside itself, it would
     * draw itself forever */
    const char *here = getenv("BACKMUX_ID");
    if (here && !strcmp(here, id)) return 0;
    if (cat == PK_TTY) m->n_tty++;
    else if (cat == PK_SCROLL) m->n_scroll++;
    else m->n_ended++;
    if (!wk->fill || cat != (int)m->pick_cat || m->npicks >= MUX_PICK_MAX) return 0;
    if (cat == PK_TTY && has_pane(m, id)) return 0; /* already on this terminal */
    struct pick *p = &m->picks[m->npicks++];
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    if (r->has_name && r->name[0]) snprintf(p->name, sizeof(p->name), "%s", r->name);
    else snprintf(p->name, sizeof(p->name), "%.12s", id);
    if (r->has_console) snprintf(p->sock, sizeof(p->sock), "%s", r->console_sock);
    if (r->recipe.argc > 0 && r->recipe.argv) snprintf(p->program, sizeof(p->program), "%s", r->recipe.argv[0]);
    if (r->recipe.argc > 1) (void)hold_format_argv_human(p->args, sizeof(p->args), r->recipe.argc - 1, r->recipe.argv + 1);
    snprintf(p->working, sizeof(p->working), "%s",
             r->recipe.has_working ? r->recipe.working : (r->has_observed ? r->observed_cwd : ""));
    if (r->has_started_at) snprintf(p->started, sizeof(p->started), "%s", r->started_at);
    (void)hold_agent_of_record(r, p->agent, sizeof(p->agent));
    p->tty = r->recipe.mode_tty;
    p->pipe = r->has_console && !r->recipe.mode_tty;
    p->shell = r->recipe.mode_shell;
    p->exit_code = r->has_exit_code ? r->exit_code : -1;
    return 0;
}

static int pick_order(const void *a, const void *b) {
    return strcasecmp(((const struct pick *)a)->name, ((const struct pick *)b)->name);
}

/* One walk of the records: the bar's counts, and (fill) the launcher's list. */
static void scan_records(struct mux *m, bool fill) {
    m->n_tty = m->n_scroll = m->n_ended = 0;
    if (fill) m->npicks = 0;
    struct pick_walk w = {m, fill};
    (void)hold_for_each_record(m->store->record_dir, pick_cb, &w);
    if (fill) {
        qsort(m->picks, (size_t)m->npicks, sizeof(m->picks[0]), pick_order);
        if (m->pick_sel >= m->npicks) m->pick_sel = m->npicks > 0 ? m->npicks - 1 : 0;
    }
}

static void load_picks(struct mux *m) {
    m->pick_sel = 0;
    scan_records(m, true);
}

/* A pane's process, from its record: the pid its load is read from, and
 * what it runs - the harness (chat talks only to agents) or the program. */
static void pane_identify(struct mux *m, struct pane *p) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (!p->id[0] || hold_load_record_by_id(m->store->record_dir, p->id, &r, path, sizeof(path)) != 0) return;
    p->pid = r.pid;
    p->agent[0] = '\0';
    (void)hold_agent_of_record(&r, p->agent, sizeof(p->agent));
    if (r.recipe.argc > 0 && r.recipe.argv && r.recipe.argv[0]) {
        const char *b = strrchr(r.recipe.argv[0], '/');
        snprintf(p->program, sizeof(p->program), "%s", b ? b + 1 : r.recipe.argv[0]);
    }
    if (r.has_name && r.name[0]) snprintf(p->name, sizeof(p->name), "%s", r.name); /* renamed elsewhere */
    hold_free_run_record(&r);
}

/* A pane's load: its terminal's foreground job when it has one (claude run
 * from a shell is what is busy, not the shell), else its program. */
static void pane_load(struct pane *p, int64_t now) {
    pid_t at = p->pid, fg = 0;
    if (at <= 0) return;
    if (hold_proc_foreground_group(at, &fg) == 0 && fg > 0) at = fg;
    uint64_t ticks = 0, rss = 0;
    if (hold_proc_read_cpu_rss(at, &ticks, &rss) != 0) {
        p->cpu_pct = -1;
        p->rss = 0;
        return;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (p->cpu_at && now > p->cpu_at && ticks >= p->cpu_prev && hz > 0)
        p->cpu_pct = (int)((ticks - p->cpu_prev) * 100000 / (uint64_t)hz / (uint64_t)(now - p->cpu_at));
    else p->cpu_pct = -1;
    p->cpu_prev = ticks;
    p->cpu_at = now;
    p->rss = rss;
}

/* Once a second while the bar or the name bars are up: the counts, the
 * machine's load, every pane's. */
static void refresh_stats(struct mux *m) {
    if (m->now < m->next_stats) return;
    m->next_stats = m->now + MUX_STATS_MS;
    scan_records(m, m->fl == FL_PROCS);
    uint64_t busy = 0, total = 0;
    if (hold_system_cpu_times(&busy, &total) == 0) {
        m->sys_cpu = m->sys_total && total > m->sys_total ? (int)((busy - m->sys_busy) * 100 / (total - m->sys_total)) : -1;
        m->sys_busy = busy;
        m->sys_total = total;
    } else {
        m->sys_cpu = -1;
    }
    uint64_t mt = 0, ma = 0;
    if (hold_system_memory(&mt, &ma) == 0) {
        m->mem_total = mt;
        m->mem_used = mt > ma ? mt - ma : 0;
    }
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) {
            pane_identify(m, m->slot[i]);
            pane_load(m->slot[i], m->now);
        }
    m->dirty = true;
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
    /* on the agent in focus, if it is one; chat talks only to agents */
    if (!chat_visible(m->chat) && f && f->id[0] && f->agent[0]) chat_open_on(m->chat, f->id, f->name);
    else if (chat_has_keyboard(m->chat)) chat_put_away(m->chat);
    else chat_toggle(m->chat);
    m->dirty = true;
}

/* ---- drawing ----------------------------------------------------------- */

/* Plain gray, not dim: a dim line all but disappears on many terminals.
 * The stretch beside the focused pane is green, to say where keys go. */
static const struct comp_style ST_SEP = {0, 244, -1};
static const struct comp_style ST_SEP_FOCUS = {HOLD_ATTR_BOLD, 2, -1};
/* floating windows: a framed panel, keys in amber, headings in blue */
static const struct comp_style ST_BOX = {0, 252, 236};
static const struct comp_style ST_BOX_DIM = {0, 245, 236};
static const struct comp_style ST_FRAME = {0, 67, 236};
static const struct comp_style ST_FRAME_TITLE = {HOLD_ATTR_BOLD, 231, 236};
static const struct comp_style ST_KEY = {HOLD_ATTR_BOLD, 179, 236};
static const struct comp_style ST_HEAD = {HOLD_ATTR_BOLD, 110, 236};
static const struct comp_style ST_SEL = {HOLD_ATTR_BOLD, 16, 150};
static const struct comp_style ST_BAD = {HOLD_ATTR_BOLD, 203, 236};
static const struct comp_style ST_LETTER = {HOLD_ATTR_BOLD, 16, 220};
/* name bars: the line, the boxed name (green when focused), flags, stats */
static const struct comp_style ST_NB_LINE = {0, 240, -1};
static const struct comp_style ST_NB_NAME = {HOLD_ATTR_BOLD, 252, 237};
static const struct comp_style ST_NB_NAME_F = {HOLD_ATTR_BOLD, 231, 22};
static const struct comp_style ST_NB_EDIT = {HOLD_ATTR_UNDERLINE, 231, 24};
static const struct comp_style ST_NB_FRESH = {HOLD_ATTR_BOLD, 16, 152};
static const struct comp_style ST_NB_LETTER = {0, 250, 238};
static const struct comp_style ST_NB_FLAG = {HOLD_ATTR_BOLD, 16, 179};
static const struct comp_style ST_NB_STAT = {0, 245, -1};
static const struct comp_style ST_NB_AGENT = {0, 179, -1};
/* the bar: dark, its counters in green, blue and red, its toggles in two states */
static const struct comp_style ST_BAR = {0, 250, 235};
static const struct comp_style ST_BAR_DIM = {0, 242, 235};
static const struct comp_style ST_CNT_TTY = {HOLD_ATTR_BOLD, 114, 235};
static const struct comp_style ST_CNT_SCROLL = {HOLD_ATTR_BOLD, 75, 235};
static const struct comp_style ST_CNT_ENDED = {HOLD_ATTR_BOLD, 203, 235};
static const struct comp_style ST_BTN_OFF = {0, 248, 238};
static const struct comp_style ST_BTN_CHAT = {HOLD_ATTR_BOLD, 231, 25};
static const struct comp_style ST_BTN_INFO = {HOLD_ATTR_BOLD, 231, 29};
static const struct comp_style ST_BTN_ON = {HOLD_ATTR_BOLD, 16, 179};
static const struct comp_style ST_BTN_UNREAD = {HOLD_ATTR_BOLD | HOLD_ATTR_BLINK, 231, 124};
static const struct comp_style ST_WIN = {0, 250, 237};
static const struct comp_style ST_WIN_CUR = {HOLD_ATTR_BOLD, 16, 150};
static const struct comp_style ST_MODE = {HOLD_ATTR_BOLD, 16, 220};
static const struct comp_style ST_NOTE = {HOLD_ATTR_BOLD, 230, 235};

static int put(struct mux *m, int row, int col, int max, const char *text, const struct comp_style *st) {
    return max > 0 ? comp_text(m->comp, row, col, max, text, strlen(text), st) : 0;
}

/* Display columns of UTF-8 text (every character here is one column). */
static int text_cols(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

/* A floating window: a filled panel in a rounded frame, its title set into
 * the top edge and a hint into the bottom one. */
static void draw_frame(struct mux *m, int y, int x, int h, int w, const char *title, const char *hint) {
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    for (int c = x + 1; c < x + w - 1; c++) {
        comp_put(m->comp, y, c, 0x2500, &ST_FRAME);
        comp_put(m->comp, y + h - 1, c, 0x2500, &ST_FRAME);
    }
    for (int r = y + 1; r < y + h - 1; r++) {
        comp_put(m->comp, r, x, 0x2502, &ST_FRAME);
        comp_put(m->comp, r, x + w - 1, 0x2502, &ST_FRAME);
    }
    comp_put(m->comp, y, x, 0x256d, &ST_FRAME);
    comp_put(m->comp, y, x + w - 1, 0x256e, &ST_FRAME);
    comp_put(m->comp, y + h - 1, x, 0x2570, &ST_FRAME);
    comp_put(m->comp, y + h - 1, x + w - 1, 0x256f, &ST_FRAME);
    if (title && *title) {
        int used = put(m, y, x + 2, w - 4, " ", &ST_FRAME_TITLE);
        used += put(m, y, x + 2 + used, w - 4 - used, title, &ST_FRAME_TITLE);
        put(m, y, x + 2 + used, w - 4 - used, " ", &ST_FRAME_TITLE);
    }
    if (hint && *hint) {
        int hl = text_cols(hint) + 2;
        if (hl < w - 4) {
            int hx = x + w - 2 - hl;
            put(m, y + h - 1, hx, hl, " ", &ST_BOX_DIM);
            put(m, y + h - 1, hx + 1, hl - 1, hint, &ST_BOX_DIM);
            put(m, y + h - 1, hx + hl - 1, 1, " ", &ST_BOX_DIM);
        }
    }
}

static void draw_separators(struct mux *m) {
    int n = win_count(m, m->view);
    if (n < 2 || m->tcols < 3 || m->arows < 3) return;
    int left, ltop, rtop;
    lines_of(m, m->view, m->arows, m->tcols, &left, &ltop, &rtop);
    struct pane *f = focused(m);
    for (int r = 0; r < m->arows; r++) {
        bool near = f && r >= f->row && r < f->row + f->rows;
        comp_put(m->comp, r, left, 0x2502, near ? &ST_SEP_FOCUS : &ST_SEP); /* │ */
    }
    if (info_on(m)) comp_put(m->comp, 0, left, 0x252c, &ST_SEP); /* ┬: the line meets the name bars */
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

static const char *const cat_name[PK_CATS] = {"Terminals", "Consoles", "Ended"};

/* The launcher: three lists - running terminals (attach one as a pane),
 * consoles that scroll (open one in a viewer pane), ended processes (start
 * one again) - and the chosen entry's details. */
static void draw_procs(struct mux *m) {
    int w = m->tcols - 6 < 84 ? m->tcols - 6 : 84, h = m->arows - 2 < 22 ? m->arows - 2 : 22;
    if (w < 30 || h < 10) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2;
    static const char *const verb[PK_CATS] = {"attach as a pane", "open in a pane", "start again"};
    char hint[96];
    snprintf(hint, sizeof(hint), "Tab: list  Up/Down: select  Enter: %s  Esc: close",
             verb[m->pick_cat]);
    draw_frame(m, y, x, h, w, "Processes", hint);
    /* the three lists, as tabs: their counts in the bar's colors */
    const int counts[PK_CATS] = {m->n_tty, m->n_scroll, m->n_ended};
    const struct comp_style *cst[PK_CATS] = {&ST_CNT_TTY, &ST_CNT_SCROLL, &ST_CNT_ENDED};
    int tx = x + 2;
    m->pick_tabs_row = y + 1;
    for (int k = 0; k < PK_CATS; k++) {
        char tab[48];
        snprintf(tab, sizeof(tab), " %s %d ", cat_name[k], counts[k]);
        m->pick_tab_x[k] = tx;
        struct comp_style on = *cst[k];
        on.bg = 238;
        tx += put(m, y + 1, tx, x + w - 2 - tx, tab, k == (int)m->pick_cat ? &on : &ST_BOX_DIM);
        tx += put(m, y + 1, tx, x + w - 2 - tx, "  ", &ST_BOX);
    }
    m->pick_tab_x[PK_CATS] = tx;
    for (int c = x + 1; c < x + w - 1; c++) comp_put(m->comp, y + 2, c, 0x2500, &ST_FRAME);
    int list_h = (h - 4) / 2;
    if (list_h < 3) list_h = 3;
    if (m->npicks == 0) {
        static const char *const none[PK_CATS] = {"No other terminal processes are running.",
                                                  "No consoles or servers are running.", "No ended processes."};
        put(m, y + 3, x + 3, w - 6, none[m->pick_cat], &ST_BOX_DIM);
    }
    int first = m->pick_sel >= list_h ? m->pick_sel - list_h + 1 : 0;
    m->pick_x = x, m->pick_w = w, m->pick_top = y + 3, m->pick_first = first, m->pick_rows = list_h;
    for (int i = first; i < m->npicks && i - first < list_h; i++) {
        const struct pick *p = &m->picks[i];
        bool sel = i == m->pick_sel;
        const struct comp_style *st = sel ? &ST_SEL : &ST_BOX;
        int row = y + 3 + i - first;
        comp_fill(m->comp, row, x + 1, w - 2, st);
        int u = put(m, row, x + 2, w - 4, sel ? "\xe2\x96\xb8 " : "  ", st);
        u += put(m, row, x + 2 + u, 26, p->name, st);
        int col2 = x + 2 + 2 + 26 + 1;
        const char *what = p->agent[0] ? p->agent : (strrchr(p->program, '/') ? strrchr(p->program, '/') + 1 : p->program);
        int v = put(m, row, col2, x + w - 2 - col2, what, sel ? st : (p->agent[0] ? &ST_KEY : &ST_BOX_DIM));
        char tail[48] = "";
        if (m->pick_cat == PK_ENDED) {
            if (p->exit_code >= 0) snprintf(tail, sizeof(tail), "exit %d", p->exit_code);
            else snprintf(tail, sizeof(tail), "ended");
        } else if (m->pick_cat == PK_SCROLL) {
            snprintf(tail, sizeof(tail), "%s", p->pipe ? "console" : "log");
        }
        if (tail[0]) {
            int tl = text_cols(tail);
            if (col2 + v + 2 < x + w - 3 - tl) put(m, row, x + w - 3 - tl, tl, tail, sel ? st : &ST_BOX_DIM);
        }
    }
    int iy = y + 3 + list_h;
    for (int c = x + 1; c < x + w - 1; c++) comp_put(m->comp, iy, c, 0x2500, &ST_FRAME);
    if (m->npicks > 0 && iy + 4 < y + h - 1) {
        const struct pick *p = &m->picks[m->pick_sel];
        static const char *const label[] = {"Program", "Arguments", "Working directory", "Started"};
        const char *val[] = {p->program, p->args[0] ? p->args : "-", p->working[0] ? p->working : "-",
                             p->started[0] ? p->started : "-"};
        for (int k = 0; k < 4 && iy + 1 + k < y + h - 1; k++) {
            put(m, iy + 1 + k, x + 3, 19, label[k], &ST_HEAD);
            put(m, iy + 1 + k, x + 3 + 19, w - 6 - 19, val[k], &ST_BOX);
        }
    }
}

/* Keyboard help: the keys after the prefix, or the active mode's, in a
 * framed panel - sections in two columns when the terminal is wide enough. */
struct help_line {
    const char *key, *what; /* key NULL: a section heading (what) */
};
static const struct help_line keys_prefix[] = {
    {NULL, "Panes"},
    {"o", "Open a process (launcher)"},
    {"p", "Pane mode: arrows, a attach, g go to"},
    {"s", "Swap two panes (letters)"},
    {"z", "Zoom the pane, or back"},
    {"Alt-arrows", "Move to the next pane"},
    {NULL, "Layout"},
    {"t", "Window mode: 1-9, Left, Right"},
    {"r", "Resize mode"},
    {"m", "Move mode"},
    {"w", "Spread across windows"},
    {"d", "Pack windows"},
    {"a", "Sort panes by name"},
    {NULL, "Display"},
    {"f", "Bottom bar on/off"},
    {"i", "Info: name bars on/off"},
    {"\\", "Lock pane (keys to its program)"},
    {NULL, "Chat and names"},
    {"c", "Chat (Esc closes it)"},
    {"n", "Rename the process"},
    {";", "Save its profile"},
    {NULL, "Session"},
    {"", ""}, /* the detach key, filled in when drawn */
    {"", ""}, /* the prefix twice */
    {"?", "These keys"},
};
static const struct help_line keys_pane[] = {
    {NULL, "Pane mode"},
    {"arrows h j k l", "Move to the next pane"},
    {"a", "Attach a process as a pane"},
    {"g", "Go to a pane by name"},
    {"z", "Zoom the pane, or back"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_tab[] = {
    {NULL, "Window mode"},
    {"1-9", "Go to that window; leave the mode"},
    {"Left Right h l", "Previous or next window"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_resize[] = {
    {NULL, "Resize mode"},
    {"arrows h j k l", "Grow the pane that way"},
    {"Shift-arrows H J K L", "Shrink it from that side"},
    {"=", "Even the lines again"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};
static const struct help_line keys_move[] = {
    {NULL, "Move mode"},
    {"arrows h j k l", "Swap with the pane that way"},
    {"1-9", "Send it to that window; leave the mode"},
    {"?", "These keys"},
    {"Esc Enter", "Leave the mode"},
};

/* A key as a person reads it: NUL is Ctrl-Space, a control byte Ctrl-X, a
 * printable byte itself. */
static void key_label(unsigned char k, char out[16]) {
    if (k == 0) snprintf(out, 16, "Ctrl-Space");
    else if (k < 0x20) snprintf(out, 16, "Ctrl-%c", k | 0x40);
    else snprintf(out, 16, "%c", k);
}

static void draw_help_lines(struct mux *m, const struct help_line *l, int n, int y, int x, int w, int keyw) {
    for (int i = 0; i < n; i++) {
        if (!l[i].key) {
            if (i > 0) y++; /* a blank line before each section but the first */
            put(m, y++, x, w, l[i].what, &ST_HEAD);
            continue;
        }
        put(m, y, x + 1, keyw, l[i].key, &ST_KEY);
        put(m, y++, x + 1 + keyw, w - 1 - keyw, l[i].what, &ST_BOX);
    }
}

static int help_height(const struct help_line *l, int n) {
    int h = 0;
    for (int i = 0; i < n; i++) h += !l[i].key && i > 0 ? 2 : 1;
    return h;
}

static void draw_help(struct mux *m) {
    struct help_line lines[sizeof(keys_prefix) / sizeof(keys_prefix[0])];
    const struct help_line *l = lines;
    int n = (int)(sizeof(keys_prefix) / sizeof(keys_prefix[0]));
    char pk[16], dk[16], twice[40], detach[48];
    key_label(m->prefix, pk);
    key_label(m->detach_key, dk);
    memcpy(lines, keys_prefix, sizeof(keys_prefix));
    snprintf(twice, sizeof(twice), "%s", pk);
    snprintf(detach, sizeof(detach), "Detach the pane; it runs on");
    for (int i = 0, k = 0; i < n; i++)
        if (lines[i].key && !lines[i].key[0]) {
            if (k++ == 0) lines[i] = (struct help_line){dk, detach};
            else lines[i] = (struct help_line){twice, "Send it to the program"};
        }
#define KEYS(arr) (l = (arr), n = (int)(sizeof(arr) / sizeof((arr)[0])))
    switch (m->mode) {
    case MODE_PANE: KEYS(keys_pane); break;
    case MODE_TAB: KEYS(keys_tab); break;
    case MODE_RESIZE: KEYS(keys_resize); break;
    case MODE_MOVE: KEYS(keys_move); break;
    default: break;
    }
#undef KEYS
    char title[64];
    if (m->mode == MODE_NONE) snprintf(title, sizeof(title), "Keys after %s", pk);
    else snprintf(title, sizeof(title), "Keys");
    int keyw = 13;
    /* two columns when it is tall and the terminal is wide: split at the
     * section heading nearest the middle */
    int split = n, total = help_height(l, n);
    if (m->mode == MODE_NONE && m->tcols >= 84 && total + 4 > m->arows - 2) {
        int best = n, bestd = INT_MAX;
        for (int i = 1; i < n; i++)
            if (!l[i].key) {
                int d = abs(help_height(l, i) - help_height(l + i, n - i));
                if (d < bestd) bestd = d, best = i;
            }
        split = best;
    }
    int colw = 13 + 38, cols = split < n ? 2 : 1;
    int inner_h = split < n ? (help_height(l, split) > help_height(l + split, n - split) ? help_height(l, split)
                                                                                       : help_height(l + split, n - split))
                            : total;
    int w = cols * colw + (cols - 1) * 3 + 4, h = inner_h + 4;
    if (w > m->tcols - 2) w = m->tcols - 2;
    if (h > m->arows) h = m->arows;
    if (w < 20 || h < 5) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2;
    draw_frame(m, y, x, h, w, title, "any key closes  \xc2\xb7  backmux help panes");
    draw_help_lines(m, l, split, y + 2, x + 2, colw, keyw);
    if (split < n) draw_help_lines(m, l + split, n - split, y + 2, x + 2 + colw + 3, w - 4 - colw - 3, keyw);
}

/* Jump: the panes of this terminal by name, with their window's number. */
static void draw_jump(struct mux *m) {
    int rows = 0;
    for (int i = 0; i < MUX_MAX_PANES; i++) rows += m->slot[i] != NULL;
    int w = 52 < m->tcols - 4 ? 52 : m->tcols - 4, h = rows + 4 < m->arows - 2 ? rows + 4 : m->arows - 2;
    if (w < 16 || h < 5) return;
    int x = (m->tcols - w) / 2, y = (m->arows - h) / 2, k = 0;
    draw_frame(m, y, x, h, w, "Go to pane", "Up/Down  Enter  Esc");
    for (int i = 0; i < MUX_MAX_PANES && k < h - 4; i++) {
        if (!m->slot[i]) continue;
        const struct comp_style *st = k == m->pick_sel ? &ST_SEL : &ST_BOX;
        char line[ALIAS_MAX_LEN + 128];
        const struct pane *p = m->slot[i];
        snprintf(line, sizeof(line), " %-*s %-10s win %d", w - 22 > 4 ? w - 22 : 4, p->name,
                 p->agent[0] ? p->agent : p->program, window_key(i / 4));
        comp_fill(m->comp, y + 2 + k, x + 1, w - 2, st);
        put(m, y + 2 + k, x + 1, w - 2, line, st);
        k++;
    }
}

/* Bytes as a person reads them: 512K, 3.1G. */
static void human_bytes(uint64_t b, char *out, size_t n) {
    if (b >= (uint64_t)10 << 30) snprintf(out, n, "%lluG", (unsigned long long)(b >> 30));
    else if (b >= (uint64_t)1 << 30) snprintf(out, n, "%.1fG", (double)b / (double)(1u << 30));
    else if (b >= (uint64_t)1 << 20) snprintf(out, n, "%lluM", (unsigned long long)(b >> 20));
    else snprintf(out, n, "%lluK", (unsigned long long)(b >> 10));
}

/* A pane's name bar: its letter and flags at the left, its name boxed in the
 * middle (click it, or the prefix then n, to type a new one over it), and at
 * the right what it runs and its load. The line is green beside the focused
 * pane, as the split lines are. */
static void draw_name_bar(struct mux *m, struct pane *p, int slot, struct comp_want *want) {
    bool focus = p == focused(m);
    int row = p->brow, col = p->col, w = p->cols;
    if (row < 0 || row >= m->arows || w < 4) return;
    const struct comp_style *line = focus ? &ST_SEP_FOCUS : &ST_NB_LINE;
    for (int c = col; c < col + w; c++) comp_put(m->comp, row, c, 0x2500, line);
    bool editing = m->prompt != PROMPT_NONE && m->prompt_pane == p;
    /* the box: ┤ name ├, centered */
    char shown[ALIAS_MAX_LEN + 1];
    snprintf(shown, sizeof(shown), "%s", editing ? m->in : p->name);
    int nl = text_cols(shown), bw = nl + 4;
    if (editing && bw < 14) bw = 14; /* room to type */
    if (bw > w - 2) bw = w - 2;
    int bx = col + (w - bw) / 2;
    p->box_col = bx;
    p->box_w = bw;
    const struct comp_style *nst = editing ? (m->in_fresh ? &ST_NB_FRESH : &ST_NB_EDIT) : focus ? &ST_NB_NAME_F : &ST_NB_NAME;
    comp_put(m->comp, row, bx, 0x2524, line); /* ┤ */
    comp_fill(m->comp, row, bx + 1, bw - 2, editing ? &ST_NB_EDIT : nst);
    int used = put(m, row, bx + 2, bw - 4, shown, nst);
    comp_put(m->comp, row, bx + bw - 1, 0x251c, line); /* ├ */
    if (editing) {
        want->cursor_visible = true;
        want->cursor_row = row;
        want->cursor_col = bx + 2 + used < bx + bw - 1 ? bx + 2 + used : bx + bw - 2;
    }
    /* left: the letter, then what is true of it now */
    int lx = col + 1, lend = bx - 1;
    char letter = slot_letter(slot);
    if (letter && lx + 3 < lend) {
        char lt[4] = {' ', letter, ' ', '\0'};
        bool chosen = m->swapping && m->swap_first == slot;
        lx += put(m, row, lx, lend - lx, lt, m->swapping ? (chosen ? &ST_SEL : &ST_LETTER) : &ST_NB_LETTER);
        lx++;
    }
    char flags[4][40];
    int nf = 0;
    if (editing) snprintf(flags[nf++], 40, " %s ", m->prompt == PROMPT_SAVE ? "Save as" : "Rename");
    if (pane_locked(p)) snprintf(flags[nf++], 40, " LOCKED ");
    if (m->full == p) snprintf(flags[nf++], 40, " ZOOM ");
    if (focus && !bar_on(m)) {
        const char *mode = m->mode == MODE_PANE     ? "PANE"
                           : m->mode == MODE_TAB    ? "WINDOW"
                           : m->mode == MODE_RESIZE ? "RESIZE"
                           : m->mode == MODE_MOVE   ? "MOVE"
                           : m->swapping            ? "SWAP"
                                                    : NULL;
        if (mode && nf < 4) snprintf(flags[nf++], 40, " %s ", mode);
        int unread = m->chat && !chat_visible(m->chat) ? chat_unread(m->chat) : 0;
        if (unread && nf < 4) snprintf(flags[nf++], 40, " chat %d ", unread);
    }
    for (int i = 0; i < nf; i++) {
        int fl = text_cols(flags[i]);
        if (lx + fl >= lend) break;
        lx += put(m, row, lx, lend - lx, flags[i], &ST_NB_FLAG);
        lx++;
    }
    if (focus && !bar_on(m) && m->now < m->notice_until && lx + 4 < lend) {
        lx += put(m, row, lx, lend - lx, " ", &ST_NOTE);
        lx += put(m, row, lx, lend - lx - 1, m->notice, &ST_NOTE);
        put(m, row, lx, lend - lx, " ", &ST_NOTE);
    }
    /* right: what it runs, its CPU and memory - the least needed go first */
    int rstart = bx + bw + 1, rend = col + w - 1;
    char cpu[16] = "", mem[16] = "";
    if (p->cpu_pct >= 0) snprintf(cpu, sizeof(cpu), "%d%%", p->cpu_pct);
    if (p->rss) human_bytes(p->rss, mem, sizeof(mem));
    const char *what = p->agent[0] ? p->agent : p->program;
    for (int drop = 0; drop <= 3; drop++) {
        char text[96] = "";
        size_t k = 0;
        bool has_what = what[0] && drop < 3, has_cpu = cpu[0] && drop < 2, has_mem = mem[0] && drop < 1;
        if (!has_what && !has_cpu && !has_mem) break;
        k += (size_t)snprintf(text + k, sizeof(text) - k, " ");
        if (has_what) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s", what);
        if (has_cpu) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", has_what ? "  " : "", cpu);
        if (has_mem) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", has_what || has_cpu ? "  " : "", mem);
        snprintf(text + k, sizeof(text) - k, " ");
        int tl = text_cols(text);
        if (rend - tl < rstart) continue;
        int tx = rend - tl;
        int wl = has_what ? text_cols(what) : 0;
        put(m, row, tx, 1, " ", &ST_NB_STAT);
        if (has_what) put(m, row, tx + 1, wl, what, p->agent[0] ? &ST_NB_AGENT : &ST_NB_STAT);
        put(m, row, tx + 1 + wl, tl - 1 - wl, text + 1 + (has_what ? strlen(what) : 0), &ST_NB_STAT);
        break;
    }
}

/* ---- the bar -------------------------------------------------------------
 * Every item has a full and a short form and a rank. When the terminal is
 * too narrow, items go to their short forms, least useful first, and then
 * the least useful go altogether - so what is left is always whole. */
struct bar_item {
    char full[176], brief[48];
    int rank;  /* higher stays longer */
    int group; /* 0 left, 1 middle, 2 right */
    const struct comp_style *st;
    enum bar_act act;
    int arg;
    bool use_brief, gone;
};

static int bar_width(const struct bar_item *it, int n) {
    int w = 2, last = -1; /* a margin each end */
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        w += text_cols(it[i].use_brief ? it[i].brief : it[i].full);
        if (last >= 0) w += it[i].group == last ? 1 : 3; /* a space within a group, more between */
        last = it[i].group;
    }
    return w;
}

static void draw_bar(struct mux *m) {
    int row = m->trows - 1;
    comp_fill(m->comp, row, 0, m->tcols, &ST_BAR);
    struct bar_item it[MUX_BAR_ITEMS];
    int n = 0;
#define ITEM(g, r, s, a, x) (memset(&it[n], 0, sizeof(it[n])), it[n].group = (g), it[n].rank = (r), it[n].st = (s), \
                             it[n].act = (a), it[n].arg = (x), &it[n++])
    struct bar_item *b;
    /* left: the counters (each opens its list in the launcher), the person's
     * chat, Info */
    b = ITEM(0, 9, &ST_CNT_TTY, BA_TTY, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_tty);
    b = ITEM(0, 7, &ST_CNT_SCROLL, BA_SCROLL, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_scroll);
    b = ITEM(0, 6, &ST_CNT_ENDED, BA_ENDED, 0);
    snprintf(b->full, sizeof(b->full), "[%d]", m->n_ended);
    if (m->chat && m->me[0]) {
        bool open = chat_visible(m->chat);
        int unread = open ? 0 : chat_unread(m->chat);
        b = ITEM(0, 10, open ? &ST_BTN_CHAT : unread ? &ST_BTN_UNREAD : &ST_BTN_OFF, BA_CHAT, 0);
        if (unread) {
            snprintf(b->full, sizeof(b->full), " %s \xe2\x97\x8f%d ", m->me, unread);
            snprintf(b->brief, sizeof(b->brief), " %.8s \xe2\x97\x8f%d ", m->me, unread);
        } else {
            snprintf(b->full, sizeof(b->full), " %s ", m->me);
            snprintf(b->brief, sizeof(b->brief), " %.8s ", m->me);
        }
    }
    b = ITEM(0, 8, g_info ? &ST_BTN_INFO : &ST_BTN_OFF, BA_INFO, 0);
    snprintf(b->full, sizeof(b->full), " Info ");
    snprintf(b->brief, sizeof(b->brief), " i ");
    /* middle: what is happening now, then the windows */
    const char *mode = m->mode == MODE_PANE     ? "PANE"
                       : m->mode == MODE_TAB    ? "WINDOW"
                       : m->mode == MODE_RESIZE ? "RESIZE"
                       : m->mode == MODE_MOVE   ? "MOVE"
                       : m->swapping            ? "SWAP"
                                                : NULL;
    if (m->lead) {
        char pk[16];
        key_label(m->prefix, pk);
        b = ITEM(1, 12, &ST_MODE, BA_HELP, 0);
        snprintf(b->full, sizeof(b->full), " %s \xe2\x80\xa6 ? keys ", pk);
        snprintf(b->brief, sizeof(b->brief), " %s ", pk);
    } else if (mode) {
        b = ITEM(1, 12, &ST_MODE, BA_HELP, 0);
        snprintf(b->full, sizeof(b->full), " %s  ? keys  Esc ", mode);
        snprintf(b->brief, sizeof(b->brief), " %s ", mode);
    }
    if (m->now < m->notice_until) {
        b = ITEM(1, 11, &ST_NOTE, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "%s", m->notice);
        snprintf(b->brief, sizeof(b->brief), "%.30s", m->notice);
    }
    int last_used = -1;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++)
        if (win_count(m, w) > 0) last_used = w;
    for (int w = 0; w <= last_used && n < MUX_BAR_ITEMS - 8; w++) {
        if (win_count(m, w) == 0 && w != m->view) continue;
        b = ITEM(1, w == m->view ? 8 : 5, w == m->view ? &ST_WIN_CUR : &ST_WIN, BA_WINDOW, w);
        snprintf(b->full, sizeof(b->full), " %d ", window_key(w));
    }
    /* right: the toggles that act on the focused pane, help, the machine */
    if (win_count(m, m->view) > 1 || m->full) {
        b = ITEM(2, 4, m->full ? &ST_BTN_ON : &ST_BTN_OFF, BA_ZOOM, 0);
        snprintf(b->full, sizeof(b->full), " Zoom ");
        snprintf(b->brief, sizeof(b->brief), " Z ");
    }
    b = ITEM(2, 3, pane_locked(focused(m)) ? &ST_BTN_ON : &ST_BTN_OFF, BA_LOCK, 0);
    snprintf(b->full, sizeof(b->full), " Lock ");
    snprintf(b->brief, sizeof(b->brief), " L ");
    b = ITEM(2, 6, &ST_BTN_OFF, BA_HELP, 0);
    snprintf(b->full, sizeof(b->full), " ? ");
    if (m->sys_cpu >= 0) {
        b = ITEM(2, 5, &ST_BAR, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "CPU %2d%%", m->sys_cpu);
        snprintf(b->brief, sizeof(b->brief), "%d%%", m->sys_cpu);
    }
    if (m->mem_total) {
        char used[16], total[16];
        human_bytes(m->mem_used, used, sizeof(used));
        human_bytes(m->mem_total, total, sizeof(total));
        b = ITEM(2, 5, &ST_BAR, BA_NONE, 0);
        snprintf(b->full, sizeof(b->full), "MEM %s/%s", used, total);
        snprintf(b->brief, sizeof(b->brief), "%s", used);
    }
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        b = ITEM(2, 2, &ST_BAR_DIM, BA_NONE, 0);
        strftime(b->full, sizeof(b->full), "%H:%M", &tmv);
    }
#undef ITEM
    /* fit: short forms first, least useful first; then drop the least useful */
    for (int rank = 0; rank <= 12 && bar_width(it, n) > m->tcols; rank++)
        for (int i = 0; i < n && bar_width(it, n) > m->tcols; i++)
            if (it[i].rank == rank && it[i].brief[0]) it[i].use_brief = true;
    for (int rank = 0; rank <= 12 && bar_width(it, n) > m->tcols; rank++)
        for (int i = n - 1; i >= 0 && bar_width(it, n) > m->tcols; i--)
            if (it[i].rank == rank) it[i].gone = true;
    /* left from the left edge, right against the right one, the middle
     * centered between them */
    int wl = 0, wm = 0, wr = 0, cl = 0, cm = 0, cr = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        int w = text_cols(it[i].use_brief ? it[i].brief : it[i].full);
        if (it[i].group == 0) wl += w + (cl++ ? 1 : 0);
        else if (it[i].group == 1) wm += w + (cm++ ? 1 : 0);
        else wr += w + (cr++ ? 1 : 0);
    }
    int xl = 1, xr = m->tcols - 1 - wr;
    int xm = xl + wl + ((xr - (xl + wl)) - wm) / 2;
    if (xm < xl + wl + (cl ? 2 : 0)) xm = xl + wl + (cl ? 2 : 0);
    m->nhits = 0;
    int x[3] = {xl, xm, xr};
    for (int i = 0; i < n; i++) {
        if (it[i].gone) continue;
        const char *t = it[i].use_brief ? it[i].brief : it[i].full;
        int g = it[i].group, w = text_cols(t);
        put(m, row, x[g], w, t, it[i].st);
        if (it[i].act != BA_NONE && m->nhits < MUX_BAR_ITEMS)
            m->hits[m->nhits++] = (struct bar_hit){x[g], x[g] + w, it[i].act, it[i].arg};
        x[g] += w + 1;
    }
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
    /* name bars: every pane's while Info is on; else only a pane with
     * something to show right now (swap's letters, a name being typed, a
     * notice with no bar to say it in) */
    bool info = info_on(m);
    for (int k = 0; k < n; k++) {
        int slot = m->view * 4 + k;
        struct pane *p = m->slot[slot];
        if (full && p != m->full) continue;
        bool needed = m->swapping || (m->prompt != PROMPT_NONE && m->prompt_pane == p) ||
                      (p == f && m->now < m->notice_until && !bar_on(m));
        if (info || needed) draw_name_bar(m, p, slot, &want);
    }
    /* chat over the panes, then any other floating window over chat: only
     * one is up at a time anyway */
    if (m->chat) chat_draw(m->chat, m->comp, m->now, &want);
    if (m->fl == FL_PROCS) {
        draw_procs(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_JUMP) {
        draw_jump(m);
        want.cursor_visible = false;
    }
    if (m->help) {
        draw_help(m);
        want.cursor_visible = false;
    }
    if (bar_on(m)) draw_bar(m);
    else if (!f && m->now < m->notice_until) put(m, m->trows - 1, 0, m->tcols, m->notice, &ST_BAD); /* no pane to say it in */
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
        int top = info_on(m) && p->brow < p->row ? p->brow : p->row; /* its name bar too */
        if (row >= top && row < p->row + p->rows && col >= p->col && col < p->col + p->cols) return m->view * 4 + k;
    }
    return -1;
}

static void floater_key(struct mux *m, const unsigned char *seq, size_t n);
static void swap_key(struct mux *m, const unsigned char *k, size_t n);
static void bar_click(struct mux *m, enum bar_act act, int arg);
static void prompt_open(struct mux *m, enum prompt kind);
static void set_focus(struct mux *m, int slot);

/* A pointer report, SGR form: b;x;y then M (press, motion) or m (release). */
static void handle_mouse(struct mux *m, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1;
    bool motion = (b & 32) != 0;
    bool wheel = (b & 64) != 0;
    int button = b & 3;
    bool press = !motion && !release && !wheel;
    /* the bar: a press on a button does what its key does, whatever else is up */
    if (bar_on(m) && row == m->trows - 1) {
        if (!press) return;
        for (int i = 0; i < m->nhits; i++)
            if (col >= m->hits[i].x0 && col < m->hits[i].x1) {
                m->prompt = PROMPT_NONE; /* a click elsewhere keeps the name as it was */
                bar_click(m, m->hits[i].act, m->hits[i].arg);
                return;
            }
        return;
    }
    if (m->prompt != PROMPT_NONE) {
        /* typing a name: a click in its box keeps typing; anywhere else keeps
         * the name as it was */
        struct pane *pp = m->prompt_pane;
        bool in_box = pp && row == pp->brow && col >= pp->box_col && col < pp->box_col + pp->box_w;
        if (press && !in_box) {
            m->prompt = PROMPT_NONE;
            m->prompt_pane = NULL;
            m->dirty = true;
        }
        return;
    }
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
    } else if (m->fl == FL_PROCS) {
        /* a click on a list's name shows that list; on an entry, opens it;
         * anywhere else closes the launcher */
        if (motion || release) return;
        if (row == m->pick_tabs_row && col >= m->pick_tab_x[0] && col < m->pick_tab_x[PK_CATS]) {
            for (int c = 0; c < PK_CATS; c++)
                if (col >= m->pick_tab_x[c] && col < m->pick_tab_x[c + 1]) {
                    m->pick_cat = (enum pick_cat)c;
                    load_picks(m);
                }
            m->dirty = true;
            return;
        }
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
    /* A click changes focus; pointer motion alone does not. A click on a
     * name box types a new name over it. */
    int s = slot_at(m, row, col);
    struct pane *p = s >= 0 ? m->slot[s] : NULL;
    if (press && p && info_on(m) && row == p->brow && p->brow == p->row - 1) {
        if (s != m->focus) set_focus(m, s);
        if (col >= p->box_col && col < p->box_col + p->box_w && p->id[0]) prompt_open(m, PROMPT_RENAME);
        return; /* the name bar is not the program's */
    }
    if (press && s >= 0 && s != m->focus) {
        set_focus(m, s);
        return; /* the click that moves focus is not the program's */
    }
    if (!p || s != m->focus) return;
    if (row < p->row || row >= p->row + p->rows) return; /* its name bar is not the program's */
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
/* out (when given) gets the first line it printed: a launch's process id. */
static int run_self(char *const args[], char *err, size_t en, char *out, size_t on) {
    if (err && en) err[0] = '\0';
    if (out && on) out[0] = '\0';
    char self[HOLD_PATH_MAX];
    if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) return -1;
    /* Files, not pipes: nothing to drain while waiting, and no broker is
     * started that could hold a pipe open. */
    FILE *fe = tmpfile(), *fo = out ? tmpfile() : NULL;
    if (!fe || (out && !fo)) {
        if (fe) fclose(fe);
        if (fo) fclose(fo);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fclose(fe);
        if (fo) fclose(fo);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
        }
        if (fo) dup2(fileno(fo), STDOUT_FILENO);
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
    if (fo) {
        rewind(fo);
        size_t got = fread(out, 1, on - 1, fo);
        out[got] = '\0';
        out[strcspn(out, "\n")] = '\0';
        fclose(fo);
    }
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
    m->in_fresh = true; /* the first key typed replaces the name; Enter or Esc keeps it */
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
        if (run_self(args, err, sizeof(err), NULL, 0) != 0) {
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
        if (run_self(args, err, sizeof(err), NULL, 0) != 0) notice(m, "not saved: %s", err[0] ? err : "backmux save failed");
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
        m->in_fresh = false; /* Backspace edits the name rather than replacing it */
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
    if (m->in_fresh) {
        m->in_fresh = false;
        m->in_len = 0;
        m->in[0] = '\0';
    }
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
            m->pick_cat = PK_TTY;
            open_floater(m, FL_PROCS);
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

/* A process as a pane, by its record's console. */
static bool open_pane_id(struct mux *m, const char *id, const char *fallback_name) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(m->store->record_dir, id, &r, path, sizeof(path)) != 0) {
        notice(m, "%s has no record", fallback_name);
        return false;
    }
    char name[ALIAS_MAX_LEN + 1], sock[HOLD_PATH_MAX];
    snprintf(name, sizeof(name), "%s", r.has_name && r.name[0] ? r.name : fallback_name);
    snprintf(sock, sizeof(sock), "%s", r.has_console ? r.console_sock : "");
    hold_free_run_record(&r);
    if (!sock[0]) {
        notice(m, "%s has no console", name);
        return false;
    }
    const char *why = NULL;
    struct pane *np = pane_open(id, name, sock, m->arows, m->tcols, &why);
    if (!np) {
        notice(m, "cannot attach %s: %s", name, why ? why : "console unavailable");
        return false;
    }
    if (add_pane(m, np) != 0) {
        pane_free(np);
        notice(m, "every window is full");
        return false;
    }
    pane_identify(m, np);
    return true;
}

/* What Enter does in the launcher. A terminal is attached as a pane. A
 * console that scrolls is opened in a pane of its own: a terminal process
 * that runs backmux attach (a program on pipes: its input too) or backmux
 * logs -f (a server), with --rm so it leaves no record once closed. An ended
 * process is started again in the background, and shown as a pane when it
 * has a terminal. */
static void launch_pick(struct mux *m, const struct pick *p) {
    if (pane_total(m) >= MUX_MAX_PANES && (m->pick_cat != PK_ENDED || p->tty)) {
        notice(m, "pane limit reached (%d)", MUX_MAX_PANES);
        return;
    }
    char err[200], id[ID_STR_LEN + 8];
    if (m->pick_cat == PK_TTY) {
        const char *why = NULL;
        struct pane *np = pane_open(p->id, p->name, p->sock, m->arows, m->tcols, &why);
        if (!np) notice(m, "cannot attach %s: %s", p->name, why ? why : "console unavailable");
        else if (add_pane(m, np) != 0) pane_free(np);
        else pane_identify(m, np);
        return;
    }
    if (m->pick_cat == PK_SCROLL) {
        char self[HOLD_PATH_MAX];
        if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) {
            notice(m, "cannot find backmux itself");
            return;
        }
        /* named for what it shows (srv-log, pipey-view), so its name bar and
         * the list say so; a name already taken gets a generated one */
        char vname[ALIAS_MAX_LEN + 1];
        snprintf(vname, sizeof(vname), "%.*s-%s", ALIAS_MAX_LEN - 5, p->name, p->pipe ? "view" : "log");
        char *args[] = {"-dt", "--rm", "--name", vname, "--", self, p->pipe ? "attach" : "logs", (char *)p->id,
                        p->pipe ? NULL : "-f", NULL};
        char *anon[] = {"-dt", "--rm", "--", self, p->pipe ? "attach" : "logs", (char *)p->id, p->pipe ? NULL : "-f", NULL};
        if ((run_self(args, err, sizeof(err), id, sizeof(id)) != 0 || !id[0]) &&
            (run_self(anon, err, sizeof(err), id, sizeof(id)) != 0 || !id[0])) {
            notice(m, "cannot open %s: %s", p->name, err[0] ? err : "the viewer did not start");
            return;
        }
        (void)open_pane_id(m, id, p->name);
        return;
    }
    if (p->shell) {
        notice(m, "a shell comes back attached: backmux %s", p->name);
        return;
    }
    char *args[] = {p->tty ? "-dt" : p->pipe ? "-di" : "-d", (char *)p->id, NULL};
    if (run_self(args, err, sizeof(err), id, sizeof(id)) != 0) {
        notice(m, "cannot start %s: %s", p->name, err[0] ? err : "backmux failed");
        return;
    }
    if (p->tty) (void)open_pane_id(m, p->id, p->name);
    else notice(m, "started %s", p->name);
}

/* Handle input for the launcher or pane-name picker. seq is one byte or one
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
    bool left = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'D';
    bool right = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'C';
    bool tab = n == 1 && seq[0] == '\t';
    switch (m->fl) {
    case FL_PROCS:
        if (left || right || tab) {
            m->pick_cat = (enum pick_cat)((m->pick_cat + (left ? PK_CATS - 1 : 1)) % PK_CATS);
            load_picks(m);
        } else if (up && m->pick_sel > 0) {
            m->pick_sel--;
        } else if (down && m->pick_sel + 1 < m->npicks) {
            m->pick_sel++;
        } else if (enter && m->npicks > 0) {
            struct pick p = m->picks[m->pick_sel];
            m->fl = FL_NONE;
            launch_pick(m, &p);
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
    if (f == FL_PROCS && m->assumed) go_alt(m);
    m->pick_sel = 0;
    if (f == FL_PROCS) load_picks(m);
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
        /* Detach acts on the active pane, as every pane key does: the keys go
         * to its broker, as plain attach sends them. The broker closes the
         * connection - the pane lets go and the rest close up; the last one
         * leaves the terminal - or, when an attach runs nested inside that
         * process (backmux attach typed in a shell), hands the keys inward:
         * the innermost attach, the one on screen, lets go first. */
        if (!f) return 0;
        (void)frame(f->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
        f->detach_ms = m->now ? m->now : 1;
        return 0;
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
    case 'f': /* the bar */
        g_bar = !g_bar;
        relayout(m);
        return 0;
    case 'i': /* Info: the name bars */
        g_info = !g_info;
        relayout(m);
        return 0;
    case 'o': /* the launcher */
        m->pick_cat = PK_TTY;
        open_floater(m, FL_PROCS);
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

/* A button on the bar: what its key does. */
static void bar_click(struct mux *m, enum bar_act act, int arg) {
    m->help = false;
    m->dirty = true;
    switch (act) {
    case BA_TTY: case BA_SCROLL: case BA_ENDED:
        m->fl = FL_NONE;
        m->pick_cat = act == BA_TTY ? PK_TTY : act == BA_SCROLL ? PK_SCROLL : PK_ENDED;
        open_floater(m, FL_PROCS);
        return;
    case BA_CHAT: /* the person's name: chat open, or put away */
        m->fl = FL_NONE;
        if (m->chat && chat_visible(m->chat)) chat_put_away(m->chat);
        else chat_key_pressed(m);
        return;
    case BA_INFO: (void)command(m, 'i'); return;
    case BA_WINDOW:
        if (win_count(m, arg) > 0) set_focus(m, arg * 4);
        return;
    case BA_ZOOM: (void)command(m, 'z'); return;
    case BA_LOCK: (void)command(m, '\\'); return;
    case BA_HELP:
        m->fl = FL_NONE;
        m->help = true;
        return;
    default: return;
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

/* The connection closing just after the detach keys is the detach (as in
 * plain attach, attach.c). */
#define MUX_DETACH_MS 2000

static void drop_ended(struct mux *m) {
    bool any = false;
    for (int i = 0; i < MUX_MAX_PANES; i++) {
        struct pane *p = m->slot[i];
        if (p && p->ended) {
            bool detached = p->detach_ms && m->now - p->detach_ms < MUX_DETACH_MS;
            if (detached && pane_total(m) == 1) {
                /* the last pane let go: the terminal is left, and says so */
                hold_console_note_detached(NULL);
                hold_console_note_detached(p->name);
            }
            /* A missing endpoint confirms console closure, not process exit.
             * An existing endpoint may belong to a replacement attach. */
            else if (!detached && !hold_path_exists(p->sock)) notice(m, "%s: console closed", p->name);
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
    m->resident = hold_console_is_resident();
    m->sys_cpu = -1;
    struct winsize ws;
    if (hold_term_size(&ws) == 0 && ws.ws_row && ws.ws_col) {
        m->trows = ws.ws_row;
        m->tcols = ws.ws_col;
    } else {
        m->trows = 24;
        m->tcols = 80;
    }
    m->arows = m->trows;
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
    pane_identify(m, first);
    refresh_stats(m);
    relayout(m); /* resident: the bar and the name bar from the first frame */
    if (!first->agent[0]) m->app = first;
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
    /* backmux shell starts with chat up but the keyboard on the shell: it is
     * the person's shell they came to type in */
    if (m->resident && m->chat && chat_has_keyboard(m->chat)) chat_toggle(m->chat);
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
            relayout(m); /* chat too, clear of the bar */
        }
        (void)hold_term_take_interrupt(); /* Ctrl-C arrives as a key for the focused pane */
        drop_ended(m);
        if (pane_total(m) == 0) break; /* every pane has ended */
        /* One pane left and nothing floating: back to plain attach, the
         * screen exactly as it is without panes - unless the compositor is
         * here for good (backmux shell). */
        if (!m->resident && pane_total(m) == 1 && m->fl == FL_NONE && !m->lead && m->mode == MODE_NONE && !m->help && !m->swapping &&
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
        if (decorated(m) && (bar_on(m) || info_on(m))) refresh_stats(m);
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
