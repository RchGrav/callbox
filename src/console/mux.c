/* console/mux.c - the compositor: panes of TTY calls, and one floating window
 * over them, drawn into the person's terminal.
 *
 * Nothing muxes by default. A call alone on the screen is shown by plain
 * attach (attach.c), byte for byte as always. The compositor runs only when
 * the person asks for more: Ctrl-P Ctrl-A brings another running call in
 * beside the one they are on, and Ctrl-P Ctrl-T / Ctrl-H / Ctrl-N put a
 * floating window or the names over it. When one pane is left and nothing
 * floats, it hands that call back to plain attach.
 *
 * Each pane is an ordinary attach to its call's broker (one connection per
 * call, so a call shown here is attached here and nowhere else). The broker
 * keeps the authoritative screen and answers the program's terminal queries
 * (the connection says NO_TERMINAL); this side mirrors the output into a
 * local screen per pane and composes all of them, plus what floats, into one
 * frame, writing only what changed (compose.c).
 *
 * Panes are numbered in the order they arrive, four to a window: 1-4 are
 * window 1, 5-8 window 2. Within a window they fill in order, so there is no
 * 3 until there are a 1 and a 2. A-D always name the slots of the window on
 * screen. Placement is fixed, no layouts to choose:
 *
 *     1 pane   2 panes   3 panes   4 panes
 *       A       B A       B A       B A
 *                         C         C D
 *
 * Why left first: screens are widescreen and a cell is about twice as tall
 * as wide, so rows are the scarce resource; splitting left/right keeps every
 * pane's full height until the third arrives. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/console.h"
#include "backmux/core.h"
#include "backmux/store.h"
#include "backmux/screen.h"
#include "backmux/term.h"
#include "backmux/console_internal.h"
#include "overlay.h"

#define MUX_MAX_WINDOWS 10 /* one per number key: 1-9, then 0 */
#define MUX_MAX_PANES (MUX_MAX_WINDOWS * 4)
#define MUX_NAME_MS 2000     /* a name shown on arrival, focus or hover */
#define MUX_FRAME_MS 16      /* output is drawn at most this often */
#define MUX_LOOK_MS 250      /* how often chat looks at the inbox and its owner */
/* How often a FIFO pane is read without poll saying so. Why: macOS's poll
 * never reports a FIFO's writer closing (Linux says POLLHUP), so a pane whose
 * process ended would stay on screen; a read returning 0 is the only sign.
 * The same interval as the attach client's probe. */
#define MUX_FIFO_PROBE_MS 250
#define MUX_PICK_MAX 64

#define KEY_LEAD 0x10   /* Ctrl-P */
#define KEY_DETACH 0x11 /* Ctrl-Q */
#define KEY_ATTACH 0x01 /* Ctrl-A */
#define KEY_CHAT 0x14   /* Ctrl-T */
#define KEY_WINDOW 0x17 /* Ctrl-W: the next window with panes */
#define KEY_SWAP 0x13   /* Ctrl-S: swap two panes on this screen */
#define KEY_GOTO 0x07   /* Ctrl-G: go to a window by its number */
#define KEY_MOVE 0x0d   /* Ctrl-M (Enter's byte): move the active pane to a window */
#define KEY_FULL 0x06   /* Ctrl-F: the active pane full screen, or back */
#define KEY_JUMP 0x0a   /* Ctrl-J: jump to a pane by its name */
#define KEY_HOLD 0x80   /* Ctrl-Space held (handed over by plain attach): the keys and names */
#define MUX_TAP_MS 700    /* a second Ctrl-Space within this is a hold (key repeat starts at 250-660 ms) */
#define MUX_LET_GO_MS 150 /* no repeat for this long: let go (repeats come every 30-92 ms) */
#define MUX_HOLD_MS 3000  /* held this long: the keys and the names show */

struct pane {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char sock[HOLD_PATH_MAX];
    int rfd, wfd;
    struct hold_screen *scr;
    int row, col, rows, cols; /* where it is drawn, on its window */
    int64_t name_until;       /* its name is shown until then */
    bool ended;
    char held[96];            /* opening bytes that may be a refusal */
    size_t held_len;
    bool heard;
};

enum floater { FL_NONE, FL_ATTACH, FL_SWAP, FL_GOTO, FL_MOVE, FL_JUMP };

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
    /* Ctrl-Space: a tap goes to the program; held, the keys and names show */
    bool nul_pending, holding;
    int64_t nul_first, nul_last;
    int64_t fifo_probe_at; /* the next read of FIFO panes poll said nothing of */
    int view;  /* the window on screen */
    int focus; /* a slot on the window on screen */
    struct compositor *comp;
    int trows, tcols;
    bool dirty;
    int64_t now;
    bool lead;
    int64_t window_note_until;
    int64_t names_until;
    int hover; /* slot under the pointer, -1 none */
    bool advertise; /* the panes changed: rewrite chat/mux.<pid> for backmux swap */
    /* the attach picker's list, as last drawn (a click picks a row) */
    int pick_x, pick_w, pick_top, pick_first, pick_rows;
    /* the floating window that is not chat */
    enum floater fl;
    struct pick picks[MUX_PICK_MAX];
    int npicks, pick_sel;
    int swap_first; /* swap: the first slot chosen, -1 none */
    char notice[160];
    int64_t notice_until;
    /* chat: the person's, the chat backmux shell always had (chat.c),
     * floating over the panes */
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

/* Attach to a call's console as a pane. A broker serves one attach at a
 * time: one just let go of (the plain attach that handed us the terminal)
 * may still be closing, so a refusal is retried for a moment. */
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
            *why = errno == ENOENT || errno == ECONNREFUSED ? "it has ended" : strerror(errno);
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
            *why = "it has ended";
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
                *why = "it is attached somewhere else";
                continue;
            }
            if (n == 0) {
                pane_free(p);
                *why = "it has ended";
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
    if (!*why) *why = "it is attached somewhere else";
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

static int windows_in_use(const struct mux *m) {
    int n = 0;
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) n += win_count(m, w) > 0;
    return n;
}

/* A window's number is its key: 1-9, then 0 for the tenth. */
static int window_key(int w) {
    return (w + 1) % 10;
}

/* The next window, after w, that has panes; w itself if none other does. */
static int next_window(const struct mux *m, int w) {
    for (int i = 1; i <= MUX_MAX_WINDOWS; i++) {
        int c = (w + i) % MUX_MAX_WINDOWS;
        if (win_count(m, c) > 0) return c;
    }
    return w;
}

/* Where slot k of a window with n panes goes, on a rows x cols screen. One
 * column of line separates left from right, one row top from bottom. */
static void place(int n, int k, int rows, int cols, int *r, int *c, int *h, int *wd) {
    int left = (cols - 1) / 2, right = cols - 1 - left;
    int top = (rows - 1) / 2, bottom = rows - 1 - top;
    if (n <= 1 || cols < 3) {
        *r = 0, *c = 0, *h = rows, *wd = cols;
        return;
    }
    switch (k) {
    case 0: /* A: the right column, or its top half once there are four */
        *r = 0, *c = left + 1, *wd = right, *h = n == 4 ? top : rows;
        break;
    case 1: /* B: the left column, or its top half from three on */
        *r = 0, *c = 0, *wd = left, *h = n >= 3 ? top : rows;
        break;
    case 2: /* C: bottom left */
        *r = top + 1, *c = 0, *wd = left, *h = bottom;
        break;
    default: /* D: bottom right */
        *r = top + 1, *c = left + 1, *wd = right, *h = bottom;
        break;
    }
}

/* Every pane where it goes; a pane toggled full takes its window's whole
 * screen (the others in that window wait behind it, at their sizes). */
static void relayout(struct mux *m) {
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        int n = win_count(m, w);
        for (int k = 0; k < n; k++) {
            struct pane *p = m->slot[w * 4 + k];
            int r, c, h, wd;
            if (p == m->full) r = 0, c = 0, h = m->trows, wd = m->tcols;
            else place(n, k, m->trows, m->tcols, &r, &c, &h, &wd);
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

static void show_name(struct mux *m, struct pane *p) {
    if (p) p->name_until = m->now + MUX_NAME_MS;
    m->dirty = true;
}

static void set_focus(struct mux *m, int slot) {
    if (slot < 0 || slot >= MUX_MAX_PANES || !m->slot[slot]) return;
    if (m->focus != slot) show_name(m, m->slot[slot]);
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
            show_name(m, p);
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

/* ---- the picker: running TTY calls ------------------------------------ */

struct pick_walk {
    struct mux *m;
};

static int pick_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct mux *m = ((struct pick_walk *)ctx)->m;
    if (!r || m->npicks >= MUX_PICK_MAX) return 0;
    /* Only TTY calls can be panes: a pane shows a screen, and a server's
     * output is a log. */
    if (!r->has_console || !r->recipe.mode_tty || r->has_exit_code || !hold_path_exists(r->console_sock) ||
        !hold_console_record_alive(r))
        return 0;
    if (has_pane(m, id)) return 0;
    /* the call this terminal itself runs in: shown inside itself, it would
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
 * Chat belongs to the person, not to any call: the chat window backmux shell
 * always had (chat.c), floating over whatever is on screen, split or not,
 * under the person's chat name (an inbox, as for any sender outside a call).
 * Only one terminal shows it: opening it in another moves it there, and the
 * conversation log (chat/<name>.log) carries the history along. */

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

/* Who a name is: a call (its id, so a rename keeps the conversation) or a
 * sender outside any call (its inbox name). */
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

/* Ctrl-P Ctrl-T: chat here (it leaves any other terminal), opened on the
 * pane you are in; pressed again, the keyboard moves between chat and the
 * program, as it always did. */
static void chat_key_pressed(struct mux *m) {
    if (!m->chat) {
        notice(m, "chat needs a name: this terminal could not get one");
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
    else chat_toggle(m->chat);
    m->dirty = true;
}

/* ---- drawing ----------------------------------------------------------- */

/* Plain gray, not dim: a dim line all but disappears on many terminals.
 * The stretch beside the focused pane is green, to say where keys go. */
static const struct comp_style ST_SEP = {0, 244, -1};
static const struct comp_style ST_SEP_FOCUS = {HOLD_ATTR_BOLD, 2, -1};
static const struct comp_style ST_NAME = {HOLD_ATTR_REVERSE | HOLD_ATTR_BOLD, -1, -1};
static const struct comp_style ST_BOX = {0, 252, 236};
static const struct comp_style ST_BOX_DIM = {0, 245, 236};
static const struct comp_style ST_TITLE = {HOLD_ATTR_BOLD, 231, 24};
static const struct comp_style ST_SEL = {HOLD_ATTR_BOLD, 16, 150};
static const struct comp_style ST_BAD = {HOLD_ATTR_BOLD, 203, 236};
static const struct comp_style ST_LETTER = {HOLD_ATTR_BOLD, 16, 220};

static int put(struct mux *m, int row, int col, int max, const char *text, const struct comp_style *st) {
    return comp_text(m->comp, row, col, max, text, strlen(text), st);
}

static void draw_badge(struct mux *m, int row, int col_right, const char *text, const struct comp_style *st) {
    int w = (int)strlen(text);
    int col = col_right - w;
    if (col < 0) col = 0;
    put(m, row, col, w, text, st);
}

static void draw_box(struct mux *m, int y, int x, int h, int w, const char *title) {
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    comp_fill(m->comp, y, x, w, &ST_TITLE);
    put(m, y, x + 1, w - 2, title, &ST_TITLE);
}

static void draw_separators(struct mux *m) {
    int n = win_count(m, m->view);
    if (n < 2 || m->tcols < 3) return;
    int left = (m->tcols - 1) / 2, top = (m->trows - 1) / 2;
    struct pane *f = focused(m);
    for (int r = 0; r < m->trows; r++) {
        bool near = f && r >= f->row && r < f->row + f->rows;
        comp_put(m->comp, r, left, 0x2502, near ? &ST_SEP_FOCUS : &ST_SEP); /* │ */
    }
    if (n >= 3) {
        for (int c = 0; c < left; c++) {
            bool near = f && f->col == 0;
            comp_put(m->comp, top, c, 0x2500, near ? &ST_SEP_FOCUS : &ST_SEP); /* ─ */
        }
        comp_put(m->comp, top, left, n == 4 ? 0x253c : 0x2524, &ST_SEP); /* ┼ ┤ */
    }
    if (n == 4)
        for (int c = left + 1; c < m->tcols; c++) {
            bool near = f && f->col > 0;
            comp_put(m->comp, top, c, 0x2500, near ? &ST_SEP_FOCUS : &ST_SEP);
        }
}

static void draw_pick(struct mux *m) {
    int w = m->tcols - 8 < 76 ? m->tcols - 8 : 76, h = m->trows - 4 < 20 ? m->trows - 4 : 20;
    if (w < 20 || h < 8) return;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2;
    draw_box(m, y, x, h, w, "attach a running process beside this one");
    int list_h = (h - 2) / 2;
    if (m->npicks == 0) put(m, y + 2, x + 2, w - 4, "No other running TTY processes.", &ST_BOX_DIM);
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
        snprintf(line, sizeof(line), "program  %s", p->program);
        put(m, iy + 1, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "args     %s", p->args[0] ? p->args : "-");
        put(m, iy + 2, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "working  %s", p->working[0] ? p->working : "-");
        put(m, iy + 3, x + 2, w - 4, line, &ST_BOX);
        snprintf(line, sizeof(line), "started  %s", p->started[0] ? p->started : "-");
        put(m, iy + 4, x + 2, w - 4, line, &ST_BOX);
    }
    put(m, y + h - 1, x + 2, w - 4, "Up/Down choose  Enter attach  s swap panes", &ST_BOX_DIM);
}

/* Swap: each pane on screen shows its letter and name; two letters swap. */
static void draw_swap(struct mux *m) {
    int n = win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        char text[ALIAS_MAX_LEN + 16];
        bool chosen = m->swap_first == m->view * 4 + k;
        snprintf(text, sizeof(text), " %c: %s%s ", 'A' + k, p->name, chosen ? " *" : "");
        put(m, p->row, p->col, p->cols, text, &ST_LETTER);
    }
}

/* The keys, while Ctrl-Space is held: what each does, nothing more - a mode
 * says what it wants when it is entered. */
static const char *const help_lines[] = {
    "Ctrl-P Ctrl-Q   detach",
    "Ctrl-P Ctrl-T   chat",
    "Ctrl-P Ctrl-A   attach a running process beside this one",
    "Ctrl-P Ctrl-S   swap panes",
    "Ctrl-P Ctrl-G   go to window",
    "Ctrl-P Ctrl-M   move active pane to another window",
    "Ctrl-P Ctrl-F   toggle full screen",
    "Ctrl-P Ctrl-J   jump by name",
};

static void draw_help(struct mux *m) {
    int n = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
    int w = 60 < m->tcols - 2 ? 60 : m->tcols - 2, h = n + 2 < m->trows ? n + 2 : m->trows;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2;
    for (int r = y; r < y + h; r++) comp_fill(m->comp, r, x, w, &ST_BOX);
    for (int i = 0; i < n && i + 1 < h; i++) put(m, y + 1 + i, x + 2, w - 4, help_lines[i], &ST_BOX);
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
    bool show_keys = m->holding && m->now - m->nul_first >= MUX_HOLD_MS;
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
    /* names: on arrival, focus, hover, or Ctrl-Space held */
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        if (full && p != m->full) continue;
        if (m->now < p->name_until || show_keys) {
            char text[ALIAS_MAX_LEN + 8];
            snprintf(text, sizeof(text), " %s ", p->name);
            draw_badge(m, p->row, p->col + p->cols, text, &ST_NAME);
        }
    }
    /* More than one window in this terminal: the key to the next, and where
     * it goes. Nothing at all with one. */
    if (windows_in_use(m) > 1) {
        char text[64];
        snprintf(text, sizeof(text), " Ctrl-P Ctrl-W  window %d -> %d ", window_key(m->view),
                 window_key(next_window(m, m->view)));
        draw_badge(m, m->trows - 1, m->tcols, text, &ST_NAME);
    }
    struct comp_want want = {0};
    struct hold_screen_modes modes = {0};
    if (f) hold_screen_modes(f->scr, &modes);
    want.app_cursor_keys = modes.app_cursor_keys;
    want.app_keypad = modes.app_keypad;
    want.bracketed_paste = modes.bracketed_paste;
    /* every pointer event comes here: click to move between panes, a name
     * on hover; the focused program gets what it asked for */
    want.mouse = HOLD_MOUSE_MOVE;
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
    /* chat over the panes (or its corner badge), then any other floating
     * window over chat: only one is up at a time anyway */
    if (m->chat) chat_draw(m->chat, m->comp, m->now, &want);
    if (m->fl == FL_ATTACH) {
        draw_pick(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_SWAP) {
        draw_swap(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_JUMP) {
        draw_jump(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_GOTO || m->fl == FL_MOVE) {
        /* the mode says what it wants, bottom left, only while it is on */
        put(m, m->trows - 1, 0, m->tcols, m->fl == FL_GOTO ? " Go to window: press its number " : " Move to window: press a number ",
            &ST_LETTER);
    }
    if (show_keys) {
        draw_help(m);
        want.cursor_visible = false;
    }
    if (m->now < m->notice_until) put(m, m->trows - 1, 0, m->tcols, m->notice, &ST_BAD);
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
        if (row >= p->row && row < p->row + p->rows && col >= p->col && col < p->col + p->cols) return m->view * 4 + k;
    }
    return -1;
}

static void floater_key(struct mux *m, const unsigned char *seq, size_t n);

/* A pointer report, SGR form: b;x;y then M (press, motion) or m (release). */
static void handle_mouse(struct mux *m, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1;
    bool motion = (b & 32) != 0;
    bool wheel = (b & 64) != 0;
    int button = b & 3;
    if (m->fl == FL_SWAP || m->fl == FL_GOTO || m->fl == FL_MOVE || m->fl == FL_JUMP) {
        /* a click anywhere puts the mode away */
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
    int s = slot_at(m, row, col);
    if (motion && s != m->hover) {
        m->hover = s;
        if (s >= 0) show_name(m, m->slot[s]); /* a name on hover, for two seconds */
    }
    if (!motion && !release && !wheel && s >= 0 && s != m->focus) {
        set_focus(m, s);
        return; /* the click that moves focus is not the program's */
    }
    struct pane *p = s >= 0 ? m->slot[s] : NULL;
    if (!p || s != m->focus) return;
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

/* A number key as a window: 1-9, then 0 for the tenth. -1 if not one. */
static int window_of_key(unsigned char c) {
    if (c >= '1' && c <= '9') return c - '1';
    if (c == '0') return 9;
    return -1;
}

/* Keys for the floating window or mode that has the keyboard. seq is one
 * key: a byte, or a whole escape sequence. */
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
                notice(m, "%d panes is the most", MUX_MAX_PANES);
            } else {
                struct pane *np = pane_open(p->id, p->name, p->sock, m->trows, m->tcols, &why);
                if (!np) notice(m, "cannot attach %s: %s", p->name, why ? why : "unknown");
                else (void)add_pane(m, np);
            }
            m->fl = FL_NONE;
        }
        return;
    case FL_SWAP: {
        if (n != 1) return;
        int c = toupper(seq[0]) - 'A';
        if (c < 0 || c >= win_count(m, m->view)) return;
        int s2 = m->view * 4 + c;
        if (m->swap_first < 0) {
            m->swap_first = s2;
        } else {
            do_swap(m, m->swap_first, s2);
            m->fl = FL_NONE;
        }
        return;
    }
    case FL_GOTO: {
        int w = n == 1 ? window_of_key(seq[0]) : -1;
        if (w < 0) return;
        if (win_count(m, w) == 0) {
            notice(m, " window %d is empty: press another key ", window_key(w));
            return;
        }
        set_focus(m, w * 4);
        m->fl = FL_NONE;
        return;
    }
    case FL_MOVE: {
        int w = n == 1 ? window_of_key(seq[0]) : -1;
        if (w < 0) return;
        if (move_pane(m, m->focus, w) != 0) {
            notice(m, " window %d is full: press another key, or Esc ", window_key(w));
            return;
        }
        m->fl = FL_NONE;
        return;
    }
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
    m->pick_sel = 0;
    m->swap_first = -1;
    if (f == FL_ATTACH) load_picks(m);
    m->fl = f;
    m->dirty = true;
}

/* The lead key, then: */
static int command(struct mux *m, unsigned char key) {
    switch (key) {
    case KEY_DETACH:
        return 1;
    case KEY_LEAD:
        if (!(m->chat && chat_has_keyboard(m->chat))) to_pane(m, &key, 1);
        return 0;
    case KEY_ATTACH:
        open_floater(m, m->fl == FL_ATTACH ? FL_NONE : FL_ATTACH);
        return 0;
    case KEY_CHAT:
        chat_key_pressed(m);
        return 0;
    case KEY_SWAP:
        if (win_count(m, m->view) > 1) open_floater(m, m->fl == FL_SWAP ? FL_NONE : FL_SWAP);
        return 0;
    case KEY_GOTO:
        open_floater(m, m->fl == FL_GOTO ? FL_NONE : FL_GOTO);
        return 0;
    case KEY_MOVE:
        if (focused(m)) open_floater(m, m->fl == FL_MOVE ? FL_NONE : FL_MOVE);
        return 0;
    case KEY_JUMP:
        open_floater(m, m->fl == FL_JUMP ? FL_NONE : FL_JUMP);
        return 0;
    case KEY_FULL:
        m->full = m->full == focused(m) ? NULL : focused(m);
        relayout(m);
        return 0;
    case KEY_WINDOW:
        if (windows_in_use(m) > 1) set_focus(m, next_window(m, m->view) * 4);
        return 0;
    case KEY_HOLD: /* handed over by plain attach: Ctrl-Space is being held */
        m->holding = true;
        m->nul_first = m->now - MUX_TAP_MS;
        m->nul_last = m->now;
        m->dirty = true;
        return 0;
    default:
        return 0; /* anything else: cancelled, nothing sent */
    }
}

/* Ctrl-Space (a NUL byte). One on its own is the program's tap, sent once
 * it is clear no repeat follows; a second within the key-repeat delay makes
 * it a hold, and every one of them is swallowed. */
static void nul_key(struct mux *m) {
    if (m->holding) {
        m->nul_last = m->now;
        return;
    }
    if (m->nul_pending && m->now - m->nul_first <= MUX_TAP_MS) {
        m->nul_pending = false;
        m->holding = true;
        m->nul_last = m->now;
        m->dirty = true;
        return;
    }
    m->nul_pending = true;
    m->nul_first = m->now;
}

/* The Ctrl-Space timers: a lone tap goes through; a hold ends when the
 * repeats stop (the key was let go), taking the keys and names away. */
static void nul_tick(struct mux *m) {
    if (m->nul_pending && !m->holding && m->now - m->nul_first > MUX_TAP_MS) {
        static const unsigned char nul = 0;
        m->nul_pending = false;
        if (m->chat && chat_has_keyboard(m->chat)) chat_input(m->chat, &nul, 1, m->now);
        else to_pane(m, &nul, 1);
        flush_keys(m);
    }
    if (m->holding && m->now - m->nul_last > MUX_LET_GO_MS) {
        m->holding = false;
        m->dirty = true;
    }
    if (m->holding && m->now - m->nul_first >= MUX_HOLD_MS) m->dirty = true;
}

/* Length of the escape sequence at s (n bytes there), 0 if not whole yet.
 * A lone ESC at the end of a read is the Esc key: terminals send a
 * sequence in one write. */
static size_t esc_len(const unsigned char *s, size_t n, bool at_end) {
    if (n < 2) return at_end ? 1 : 0;
    if (s[1] == '[') {
        for (size_t i = 2; i < n; i++)
            if (s[i] >= 0x40 && s[i] <= 0x7e) return i + 1;
        return n >= 32 ? n : 0;
    }
    if (s[1] == 'O') return n >= 3 ? 3 : 0;
    return 2; /* Alt and a key */
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
            /* a pointer report: always ours to route, and never the second
             * key of the lead key */
            if (k > 3 && s[1] == '[' && s[2] == '<') {
                int mb = 0, mx = 0, my = 0;
                char tmp[40];
                size_t tl = k - 3 < sizeof(tmp) - 1 ? k - 3 : sizeof(tmp) - 1;
                memcpy(tmp, s + 3, tl);
                tmp[tl] = '\0';
                if (sscanf(tmp, "%d;%d;%d", &mb, &mx, &my) == 3) {
                    flush_keys(m);
                    /* chat picks out what is its own (its window, a drag
                     * of it, its badge) and hands the rest back */
                    if (m->fl == FL_NONE && m->chat) chat_input(m->chat, s, k, m->now);
                    else handle_mouse(m, mb, mx, my, s[k - 1] == 'm');
                }
            } else if (m->lead) {
                m->lead = false; /* a key that is not a command: cancelled */
                m->dirty = true;
            } else if (m->fl != FL_NONE) {
                floater_key(m, s, k);
            } else if (m->chat && chat_has_keyboard(m->chat)) {
                chat_input(m->chat, s, k, m->now);
            } else {
                to_pane(m, s, k);
            }
            i += k;
            continue;
        }
        i++;
        if (m->lead) {
            m->lead = false;
            m->dirty = true;
            flush_keys(m);
            rc = command(m, b);
            continue;
        }
        if (b == KEY_LEAD) {
            m->lead = true; /* waits for the next key, however long it takes */
            m->dirty = true;
            continue;
        }
        if (b == 0x00) {
            flush_keys(m);
            nul_key(m);
            continue;
        }
        if (m->fl != FL_NONE) floater_key(m, &b, 1);
        else if (m->chat && chat_has_keyboard(m->chat)) chat_input(m->chat, &b, 1, m->now);
        else to_pane(m, &b, 1);
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
            /* Its console still there: another terminal took the call; it
             * runs on, so say nothing. Gone: it ended. */
            if (!hold_path_exists(p->sock)) notice(m, "%s has ended", p->name);
            pane_free(p);
            m->slot[i] = NULL;
            any = true;
        }
    }
    if (any) {
        compact(m);
        relayout(m);
    }
}

int hold_mux_run(const struct hold_store *store, const char *chat_name, const char *first_sock,
                 unsigned char cmd, char *handback, size_t hn, bool *handed_back) {
    *handed_back = false;
    struct mux *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    m->store = store;
    snprintf(m->me, sizeof(m->me), "%s", chat_name ? chat_name : "");
    m->comp = comp_new();
    m->hover = -1;
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
        };
        m->chat = chat_new(&host);
        if (m->chat) {
            chat_set_on_sent(m->chat, on_sent);
            chat_ready(m->chat, m->trows, m->tcols, m->now);
        }
    }
    int rc = 0;

    /* the call this started on: its record names it */
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
    struct pane *first = pane_open(ff.id[0] ? ff.id : "", ff.name[0] ? ff.name : "call", first_sock, m->trows, m->tcols, &why);
    if (!first) {
        free(m);
        return 3;
    }
    (void)add_pane(m, first);
    first->name_until = 0;
    if (m->chat) chat_set_screen(m->chat, first->scr, first->name);
    /* Inline, the terminal shows this pane's screen already (and the
     * person's lines above where the launch began): start from that, so
     * chat paints only its own window instead of wiping the screen. */
    if (hold_console_was_inline()) {
        struct hold_snapshot snap;
        if (hold_screen_snapshot(first->scr, &snap) == 0) {
            comp_assume(m->comp, &snap);
            hold_snapshot_free(&snap);
        }
    }
    struct hold_term_signals sig;
    hold_term_signals_install(&sig, true);
    if (command(m, cmd) == 1) goto detach;

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
        nul_tick(m);
        if (pane_total(m) == 1 && m->fl == FL_NONE && !m->lead && !m->holding && !m->nul_pending && !m->full &&
            !(m->chat && chat_on_screen(m->chat, m->now))) {
            struct pane *p = m->slot[0];
            /* the last frame, with nothing over the program: inline nothing
             * repaints after this, so nothing of chat may be left on it */
            m->dirty = true;
            render(m);
            snprintf(handback, hn, "%s", p->sock);
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
        if ((m->nul_pending || m->holding) && timeout > 40) timeout = 40; /* Ctrl-Space timers */
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
    /* Detach: every pane lets go; every call keeps running. */
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) (void)frame(m->slot[i]->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
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
