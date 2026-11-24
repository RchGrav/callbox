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

#define MUX_MAX_WINDOWS 16
#define MUX_MAX_PANES (MUX_MAX_WINDOWS * 4)
#define MUX_NAME_MS 2000     /* a name shown on arrival, focus or hover */
#define MUX_FRAME_MS 16      /* output is drawn at most this often */
#define MUX_LOOK_MS 250      /* how often chat looks at the inbox and its owner */
#define MUX_PICK_MAX 64
#define MUX_CHAT_LINES 200
#define MUX_DRAFT_MAX 4000

#define KEY_LEAD 0x10   /* Ctrl-P */
#define KEY_DETACH 0x11 /* Ctrl-Q */
#define KEY_ATTACH 0x01 /* Ctrl-A */
#define KEY_CHAT 0x14   /* Ctrl-T */
#define KEY_HELP 0x08   /* Ctrl-H */
#define KEY_NAMES 0x0e  /* Ctrl-N */
#define KEY_WINDOW 0x17 /* Ctrl-W */

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

enum floater { FL_NONE, FL_ATTACH, FL_SWAP, FL_HELP, FL_CHAT };

struct pick {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char sock[HOLD_PATH_MAX];
    char program[256];
    char args[512];
    char working[HOLD_PATH_MAX];
    char started[64];
};

struct chat_line {
    int64_t at;
    bool mine;
    char peer[ALIAS_MAX_LEN + 1];
    char *text;
    int state; /* 0 received, 1 sending, 2 delivered, 3 failed */
    char why[ALIAS_MAX_LEN + 96];
};

struct sending {
    pid_t pid;
    int fd;
    int line; /* index into lines */
    char result[200];
    size_t rlen;
};

struct mux {
    const struct hold_store *store;
    char me[ALIAS_MAX_LEN + 1];
    struct pane *slot[MUX_MAX_PANES]; /* window w holds slot[w*4 .. w*4+3], dense from its start */
    int nwin;
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
    bool pasting;   /* inside a bracketed paste, in the chat draft */
    /* the floating window */
    enum floater fl;
    struct pick picks[MUX_PICK_MAX];
    int npicks, pick_sel;
    int swap_first; /* swap: the first slot chosen, -1 none */
    char notice[160];
    int64_t notice_until;
    /* chat */
    char peer[ALIAS_MAX_LEN + 1];
    char draft[MUX_DRAFT_MAX];
    size_t draft_len;
    struct chat_line lines[MUX_CHAT_LINES];
    int nlines;
    int unread;
    char unread_from[ALIAS_MAX_LEN + 1];
    off_t inbox_off;
    int64_t next_look;
    struct sending send[4];
    int chat_x, chat_y, chat_w, chat_h; /* where the window is; x < 0: not placed yet */
    bool dragging;
    int drag_dx, drag_dy;
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
    for (int w = 0; w < m->nwin; w++) n += win_count(m, w);
    return n;
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

static void relayout(struct mux *m) {
    for (int w = 0; w < m->nwin; w++) {
        int n = win_count(m, w);
        for (int k = 0; k < n; k++) {
            struct pane *p = m->slot[w * 4 + k];
            int r, c, h, wd;
            place(n, k, m->trows, m->tcols, &r, &c, &h, &wd);
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

/* Remove empty windows and keep every window's panes dense from its start:
 * panes after a gap move up in arrival order. */
static void compact(struct mux *m) {
    struct pane *keep[MUX_MAX_PANES];
    int nw = 0;
    for (int w = 0; w < m->nwin; w++) {
        struct pane *in[4];
        int k = 0;
        for (int i = 0; i < 4; i++)
            if (m->slot[w * 4 + i]) in[k++] = m->slot[w * 4 + i];
        if (!k) {
            if (m->view > nw && m->view >= w) m->view--;
            continue;
        }
        for (int i = 0; i < 4; i++) keep[nw * 4 + i] = i < k ? in[i] : NULL;
        nw++;
    }
    memset(m->slot, 0, sizeof(m->slot));
    memcpy(m->slot, keep, (size_t)nw * 4 * sizeof(keep[0]));
    m->nwin = nw;
    if (m->view >= nw) m->view = nw > 0 ? nw - 1 : 0;
    int n = nw ? win_count(m, m->view) : 0;
    if (m->focus < m->view * 4 || m->focus >= m->view * 4 + n) m->focus = m->view * 4;
}

static struct pane *focused(const struct mux *m) {
    return m->nwin ? m->slot[m->focus] : NULL;
}

static void show_name(struct mux *m, struct pane *p) {
    if (p) p->name_until = m->now + MUX_NAME_MS;
    m->dirty = true;
}

static void set_focus(struct mux *m, int slot) {
    if (slot < 0 || slot >= MUX_MAX_PANES || !m->slot[slot]) return;
    if (m->focus != slot) show_name(m, m->slot[slot]);
    m->focus = slot;
    m->view = slot / 4;
    m->dirty = true;
    m->advertise = true;
}

/* A new pane takes the first free slot: the end of the first window that
 * has room, else a new window. Focus goes to it. */
static int add_pane(struct mux *m, struct pane *p) {
    for (int w = 0; w < MUX_MAX_WINDOWS; w++) {
        if (w >= m->nwin) m->nwin = w + 1;
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

/* A slot from a letter (A-D, on the window on screen) or a number (1-based,
 * across all windows). -1 if neither. */
static int slot_of_word(const struct mux *m, const char *w) {
    if (strlen(w) == 1 && toupper((unsigned char)w[0]) >= 'A' && toupper((unsigned char)w[0]) <= 'D')
        return m->view * 4 + (toupper((unsigned char)w[0]) - 'A');
    char *end = NULL;
    long n = strtol(w, &end, 10);
    if (end && !*end && n >= 1 && n <= MUX_MAX_PANES) return (int)n - 1;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] && (!strcmp(m->slot[i]->name, w) || !strncmp(m->slot[i]->id, w, strlen(w)))) return i;
    return -1;
}

/* Two slots trade places. A target that is empty must be the next free slot
 * of its window (there is no 3 until there are a 1 and a 2) or the first
 * slot of the window after the last: the pane moves there, and the window it
 * left closes up. */
static int do_swap(struct mux *m, int a, int b, char *why, size_t n) {
    if (a < 0 || b < 0 || a >= MUX_MAX_PANES || b >= MUX_MAX_PANES) {
        snprintf(why, n, "no such pane");
        return -1;
    }
    if (a == b) return 0;
    if (!m->slot[a] && m->slot[b]) {
        int t = a;
        a = b;
        b = t;
    }
    if (!m->slot[a]) {
        snprintf(why, n, "both panes are empty");
        return -1;
    }
    if (!m->slot[b]) {
        int w = b / 4;
        bool next_in_window = w < m->nwin && b == w * 4 + win_count(m, w);
        bool new_window = w == m->nwin && b == w * 4 && w < MUX_MAX_WINDOWS;
        if (!next_in_window && !new_window) {
            snprintf(why, n, "pane %d cannot be filled yet: a window fills in order (1-4, 5-8, ...)", b + 1);
            return -1;
        }
        if (new_window) m->nwin = w + 1;
    }
    struct pane *t = m->slot[a];
    m->slot[a] = m->slot[b];
    m->slot[b] = t;
    struct pane *moved = m->slot[b];
    compact(m);
    relayout(m);
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i] == moved) set_focus(m, i);
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
    if (!r->has_console || !r->recipe.mode_tty || r->has_exit_code || !hold_path_exists(r->console_sock)) return 0;
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
 * Chat belongs to the person, not to any call: one window, over whatever
 * they are attached to, under their own name (an inbox, as for any sender
 * outside a call). Only one terminal shows it: opening it in another moves
 * it there, with its draft and conversation. */

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

static void chat_save_draft(struct mux *m) {
    char path[HOLD_PATH_MAX];
    if (chat_path(m, "draft", path, sizeof(path)) == 0) write_small(path, m->draft, m->draft_len);
    if (chat_path(m, "peer", path, sizeof(path)) == 0) write_small(path, m->peer, strlen(m->peer));
}

static void add_line(struct mux *m, bool mine, const char *peer, const char *text, size_t len, int state) {
    if (m->nlines == MUX_CHAT_LINES) {
        free(m->lines[0].text);
        memmove(m->lines, m->lines + 1, (MUX_CHAT_LINES - 1) * sizeof(m->lines[0]));
        m->nlines--;
        for (int i = 0; i < 4; i++)
            if (m->send[i].pid) m->send[i].line--;
    }
    struct chat_line *l = &m->lines[m->nlines++];
    memset(l, 0, sizeof(*l));
    l->at = (int64_t)time(NULL);
    l->mine = mine;
    snprintf(l->peer, sizeof(l->peer), "%s", peer);
    l->text = strndup(text, len);
    l->state = state;
    m->dirty = true;
}

/* The conversation log: every line in and out, so whichever terminal shows
 * chat next has it. "UNIXTIME DIR PEER HEXTEXT", DIR > sent, < received. */
static void log_line(struct mux *m, bool mine, const char *peer, const char *text, size_t len) {
    char path[HOLD_PATH_MAX];
    if (chat_path(m, "log", path, sizeof(path)) != 0) return;
    size_t cap = 64 + ALIAS_MAX_LEN + 2 * len;
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

static void load_log(struct mux *m) {
    for (int i = 0; i < m->nlines; i++) free(m->lines[i].text);
    m->nlines = 0;
    char path[HOLD_PATH_MAX];
    if (chat_path(m, "log", path, sizeof(path)) != 0) return;
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
        add_line(m, dir == '>', peer, text, tl, dir == '>' ? 2 : 0);
        m->lines[m->nlines - 1].at = at;
        free(text);
    }
    free(line);
    fclose(f);
}

static int inbox_cb(time_t at, const char *from, const char *text, size_t len, void *ctx) {
    struct mux *m = ctx;
    add_line(m, false, from, text, len, 0);
    m->lines[m->nlines - 1].at = (int64_t)at;
    log_line(m, false, from, text, len);
    if (m->fl != FL_CHAT || strcmp(m->peer, from) != 0) {
        m->unread++;
        snprintf(m->unread_from, sizeof(m->unread_from), "%s", from);
    }
    if (!m->peer[0]) snprintf(m->peer, sizeof(m->peer), "%s", from);
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
    if (m->fl == FL_CHAT && !chat_mine(m)) {
        m->fl = FL_NONE;
        m->dirty = true;
    }
}

static void chat_open(struct mux *m) {
    char path[HOLD_PATH_MAX], text[32];
    if (!m->me[0]) {
        notice(m, "chat needs a name: this terminal could not get one");
        return;
    }
    if (chat_path(m, "active", path, sizeof(path)) == 0) {
        int k = snprintf(text, sizeof(text), "%d\n", (int)getpid());
        write_small(path, text, (size_t)k);
    }
    /* what the last terminal to show it left: the draft and who to */
    if (chat_path(m, "draft", path, sizeof(path)) == 0) m->draft_len = read_small(path, m->draft, sizeof(m->draft));
    char peer[ALIAS_MAX_LEN + 2] = "";
    if (chat_path(m, "peer", path, sizeof(path)) == 0 && read_small(path, peer, sizeof(peer)) && hold_valid_alias(peer))
        snprintf(m->peer, sizeof(m->peer), "%s", peer);
    load_log(m);
    if (!m->peer[0] && focused(m)) snprintf(m->peer, sizeof(m->peer), "%s", focused(m)->name);
    m->unread = 0;
    m->fl = FL_CHAT;
    m->dirty = true;
}

static void chat_close(struct mux *m) {
    chat_save_draft(m);
    m->fl = FL_NONE;
    m->dragging = false;
    m->dirty = true;
}

static _Noreturn void send_helper(const char *sock, const char *me, const char *text, size_t len, int wfd) {
    for (int fd = 3; fd < 1024; fd++)
        if (fd != wfd) close(fd);
    signal(SIGPIPE, SIG_IGN);
    size_t ml = strlen(me);
    size_t cap = 64 + 2 * ml + 2 * len;
    char *req = malloc(cap);
    static const char hx[] = "0123456789abcdef";
    char line[200] = "error out of memory\n";
    if (req) {
        /* from an operator: its inbox name, marked op=1 (what backmux msg
         * sends from outside any call), so replies come to this chat */
        size_t k = (size_t)snprintf(req, cap, "msg op=1 from=");
        for (size_t i = 0; i < ml; i++) {
            req[k++] = hx[(unsigned char)me[i] >> 4];
            req[k++] = hx[(unsigned char)me[i] & 15];
        }
        req[k++] = '\n';
        for (size_t i = 0; i < len; i++) {
            req[k++] = hx[(unsigned char)text[i] >> 4];
            req[k++] = hx[(unsigned char)text[i] & 15];
        }
        unsigned char *reply = NULL;
        size_t rl = 0;
        if (hold_console_request(sock, req, k, &reply, &rl) != 0)
            snprintf(line, sizeof(line), "error %s\n", errno == ENOENT ? "it has ended" : strerror(errno));
        else
            snprintf(line, sizeof(line), "%.*s\n", (int)strcspn((char *)reply, "\n"), (char *)reply);
        free(reply);
    }
    (void)hold_write_all(wfd, line, strlen(line));
    _exit(0);
}

struct find_walk {
    const char *name;
    char sock[HOLD_PATH_MAX];
    bool found, live;
};

static int find_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)id;
    (void)path;
    struct find_walk *f = ctx;
    if (!r || !r->has_name || strcmp(r->name, f->name) != 0) return 0;
    f->found = true;
    f->live = r->has_console && !r->has_exit_code && hold_path_exists(r->console_sock);
    if (f->live) snprintf(f->sock, sizeof(f->sock), "%s", r->console_sock);
    return f->live ? 1 : 0;
}

static void chat_send(struct mux *m) {
    if (!m->draft_len) return;
    if (!m->peer[0]) {
        notice(m, "Tab chooses who to talk to");
        return;
    }
    add_line(m, true, m->peer, m->draft, m->draft_len, 1);
    int li = m->nlines - 1;
    log_line(m, true, m->peer, m->draft, m->draft_len);
    struct find_walk f = {.name = m->peer};
    (void)hold_for_each_record(m->store->record_dir, find_cb, &f);
    if (!f.found) {
        /* not a call: another person or agent outside any call, by inbox */
        if (hold_inbox_deliver(m->store, m->peer, m->me, m->draft, m->draft_len) == 0) m->lines[li].state = 2;
        else {
            m->lines[li].state = 3;
            snprintf(m->lines[li].why, sizeof(m->lines[li].why), "%s is not a running call or an inbox", m->peer);
        }
    } else if (!f.live) {
        m->lines[li].state = 3;
        snprintf(m->lines[li].why, sizeof(m->lines[li].why), "%s is not running", m->peer);
    } else {
        struct sending *s = NULL;
        for (int i = 0; i < 4 && !s; i++)
            if (!m->send[i].pid) s = &m->send[i];
        int fds[2];
        if (!s || pipe(fds) != 0) {
            m->lines[li].state = 3;
            snprintf(m->lines[li].why, sizeof(m->lines[li].why), "too many messages on their way");
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                close(fds[0]);
                send_helper(f.sock, m->me, m->draft, m->draft_len, fds[1]);
            }
            close(fds[1]);
            if (pid < 0) {
                close(fds[0]);
                m->lines[li].state = 3;
                snprintf(m->lines[li].why, sizeof(m->lines[li].why), "could not send");
            } else {
                (void)fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
                memset(s, 0, sizeof(*s));
                s->pid = pid;
                s->fd = fds[0];
                s->line = li;
            }
        }
    }
    m->draft_len = 0;
    m->dirty = true;
}

static void send_done(struct mux *m, struct sending *s) {
    s->result[s->rlen < sizeof(s->result) ? s->rlen : sizeof(s->result) - 1] = '\0';
    if (s->line >= 0 && s->line < m->nlines) {
        struct chat_line *l = &m->lines[s->line];
        if (!strncmp(s->result, "ok", 2)) l->state = 2;
        else {
            l->state = 3;
            snprintf(l->why, sizeof(l->why), "%.*s", (int)strcspn(s->result + (strncmp(s->result, "error ", 6) ? 0 : 6), "\n"),
                     s->result + (strncmp(s->result, "error ", 6) ? 0 : 6));
        }
    }
    close(s->fd);
    (void)waitpid(s->pid, NULL, 0);
    memset(s, 0, sizeof(*s));
    m->dirty = true;
}

/* Tab: the next one to talk to - the panes first, then anyone in the
 * conversation. */
static void chat_next_peer(struct mux *m) {
    const char *names[MUX_MAX_PANES + MUX_CHAT_LINES];
    int n = 0;
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) names[n++] = m->slot[i]->name;
    for (int i = 0; i < m->nlines; i++) {
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(names[k], m->lines[i].peer);
        if (!dup) names[n++] = m->lines[i].peer;
    }
    if (!n) return;
    int at = -1;
    for (int k = 0; k < n; k++)
        if (!strcmp(names[k], m->peer)) at = k;
    snprintf(m->peer, sizeof(m->peer), "%s", names[(at + 1) % n]);
    m->dirty = true;
}

/* ---- drawing ----------------------------------------------------------- */

static const struct comp_style ST_SEP = {HOLD_ATTR_DIM, -1, -1};
static const struct comp_style ST_SEP_FOCUS = {0, 2, -1};
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
    draw_box(m, y, x, h, w, "attach a running call beside this one");
    int list_h = (h - 2) / 2;
    if (m->npicks == 0) put(m, y + 2, x + 2, w - 4, "No other running TTY calls.", &ST_BOX_DIM);
    int first = m->pick_sel >= list_h ? m->pick_sel - list_h + 1 : 0;
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
    put(m, y + h - 1, x + 2, w - 4, "Up/Down choose  Enter attach  s swap panes  Esc close", &ST_BOX_DIM);
}

static void draw_swap(struct mux *m) {
    int n = win_count(m, m->view);
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        char text[8];
        bool chosen = m->swap_first == m->view * 4 + k;
        snprintf(text, sizeof(text), " %c%s ", 'A' + k, chosen ? "*" : "");
        put(m, p->row, p->col, 6, text, &ST_LETTER);
    }
    char hint[160];
    snprintf(hint, sizeof(hint), " swap: %s  (A-D, or a number: %d is window %d's first)  Esc cancels ",
             m->swap_first < 0 ? "press two letters" : "now the second", m->nwin * 4 + 1, m->nwin + 1);
    put(m, m->trows - 1, 0, m->tcols, hint, &ST_LETTER);
}

static const char *const help_lines[] = {
    "Ctrl-P Ctrl-Q   detach; everything keeps running",
    "Ctrl-P Ctrl-T   chat (yours, over any pane; Esc hides it)",
    "Ctrl-P Ctrl-A   attach another running call beside this one",
    "                (in that window: s swaps two panes)",
    "Ctrl-P Ctrl-N   show every pane's name",
    "Ctrl-P Ctrl-W   next window (panes 5-8 are window 2, ...)",
    "Ctrl-P Ctrl-H   this help",
    "Ctrl-P Ctrl-P   a Ctrl-P for the program",
    "Ctrl-P, any other key: cancelled, nothing is sent",
    "",
    "Click a pane to move to it. Panes fill B A / C D, in order.",
    "backmux swap A,B  (A-D on this window, or numbers) from anywhere.",
    "",
    "Esc closes this.",
};

static void draw_help(struct mux *m) {
    int n = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
    int w = 66 < m->tcols - 2 ? 66 : m->tcols - 2, h = n + 3 < m->trows ? n + 3 : m->trows;
    int x = (m->tcols - w) / 2, y = (m->trows - h) / 2;
    draw_box(m, y, x, h, w, "backmux keys");
    for (int i = 0; i < n && i + 2 < h; i++) put(m, y + 2 + i, x + 2, w - 4, help_lines[i], &ST_BOX);
}

/* One message, wrapped to width: how many rows it takes, drawn from row
 * y when draw is set. */
static int wrap_rows(struct mux *m, const char *text, int width, int y, int x, int bottom, bool draw,
                     const struct comp_style *st) {
    int rows = 0;
    size_t len = strlen(text), i = 0;
    if (width < 4) return 1;
    while (i <= len) {
        size_t start = i;
        int cols = 0;
        while (i < len && text[i] != '\n') {
            uint32_t cp;
            size_t k = comp_utf8_next(text + i, len - i, &cp);
            int cw = hold_char_width(cp);
            if (cw < 0) cw = 1;
            if (cols + cw > width) break;
            cols += cw;
            i += k;
        }
        if (draw && y + rows >= 0 && y + rows < bottom)
            comp_text(m->comp, y + rows, x, width, text + start, i - start, st);
        rows++;
        if (i < len && text[i] == '\n') i++;
        else if (i >= len) break;
    }
    return rows;
}

static void draw_chat(struct mux *m, int *cur_row, int *cur_col) {
    int w = m->tcols - 4 < 64 ? m->tcols - 4 : 64, h = m->trows - 2 < 16 ? m->trows - 2 : 16;
    if (w < 24 || h < 6) return;
    if (m->chat_x < 0 || m->chat_w != w || m->chat_h != h) {
        if (m->chat_x < 0) {
            m->chat_x = m->tcols - w - 2;
            m->chat_y = m->trows - h - 1;
        }
        m->chat_w = w, m->chat_h = h;
    }
    if (m->chat_x + w > m->tcols) m->chat_x = m->tcols - w;
    if (m->chat_y + h > m->trows) m->chat_y = m->trows - h;
    if (m->chat_x < 0) m->chat_x = 0;
    if (m->chat_y < 0) m->chat_y = 0;
    int x = m->chat_x, y = m->chat_y;
    char title[ALIAS_MAX_LEN * 2 + 32];
    snprintf(title, sizeof(title), "%s chat -> %s", m->me, m->peer[0] ? m->peer : "(Tab: who to)");
    draw_box(m, y, x, h, w, title);
    comp_put(m->comp, y, x + w - 2, 0xd7, &ST_TITLE); /* × */
    /* the conversation with this peer, newest at the bottom */
    int top = y + 1, bottom = y + h - 2, inner = w - 2;
    int row = bottom;
    for (int i = m->nlines - 1; i >= 0 && row > top; i--) {
        struct chat_line *l = &m->lines[i];
        if (strcmp(l->peer, m->peer) != 0) continue;
        char head[ALIAS_MAX_LEN + 64];
        struct tm tmv;
        time_t at = (time_t)l->at;
        char when[8] = "";
        if (localtime_r(&at, &tmv)) strftime(when, sizeof(when), "%H:%M", &tmv);
        const char *state = l->state == 1 ? " (sending)" : l->state == 3 ? " NOT DELIVERED" : "";
        snprintf(head, sizeof(head), "%s %s%s", when, l->mine ? m->me : l->peer, state);
        int body = wrap_rows(m, l->text ? l->text : "", inner - 2, 0, 0, 0, false, &ST_BOX);
        int extra = l->state == 3 && l->why[0] ? 1 : 0;
        int need = 1 + body + extra;
        int start = row - need;
        put(m, start, x + 1, inner, head, l->state == 3 ? &ST_BAD : &ST_BOX_DIM);
        wrap_rows(m, l->text ? l->text : "", inner - 2, start + 1, x + 3, row, true, &ST_BOX);
        if (extra) put(m, start + 1 + body, x + 3, inner - 2, l->why, &ST_BAD);
        row = start;
        (void)top;
    }
    /* repaint the title over anything that ran up into it */
    draw_box(m, y, x, 1, w, title);
    comp_put(m->comp, y, x + w - 2, 0xd7, &ST_TITLE);
    /* the draft, last line of it */
    int iy = y + h - 1;
    comp_fill(m->comp, iy, x, w, &ST_SEL);
    size_t from = 0;
    for (size_t i = 0; i < m->draft_len; i++)
        if (m->draft[i] == '\n') from = i + 1;
    int room = w - 4;
    size_t vis = m->draft_len - from;
    /* keep the cursor in view: show the tail */
    while (vis > (size_t)room) {
        uint32_t cp;
        size_t k = comp_utf8_next(m->draft + from, m->draft_len - from, &cp);
        from += k;
        vis -= k;
    }
    put(m, iy, x + 1, 2, ">", &ST_SEL);
    int used = comp_text(m->comp, iy, x + 3, room, m->draft + from, m->draft_len - from, &ST_SEL);
    *cur_row = iy;
    *cur_col = x + 3 + used;
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
    int n = m->nwin ? win_count(m, m->view) : 0;
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        struct hold_snapshot snap;
        if (hold_screen_snapshot(p->scr, &snap) == 0) {
            comp_blit(m->comp, &snap, p->row, p->col, p->rows, p->cols);
            hold_snapshot_free(&snap);
        }
    }
    draw_separators(m);
    /* names: on arrival, focus, hover, or Ctrl-P Ctrl-N */
    for (int k = 0; k < n; k++) {
        struct pane *p = m->slot[m->view * 4 + k];
        if (m->now < p->name_until || m->now < m->names_until) {
            char text[ALIAS_MAX_LEN + 32];
            if (m->now < m->names_until)
                snprintf(text, sizeof(text), " %c %d %s ", 'A' + k, m->view * 4 + k + 1, p->name);
            else
                snprintf(text, sizeof(text), " %s ", p->name);
            draw_badge(m, p->row, p->col + p->cols, text, &ST_NAME);
        }
    }
    if (m->nwin > 1 && m->now < m->window_note_until) {
        char text[48];
        snprintf(text, sizeof(text), " window %d of %d ", m->view + 1, m->nwin);
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
    int crow = -1, ccol = -1;
    if (m->fl == FL_ATTACH) {
        draw_pick(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_SWAP) {
        draw_swap(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_HELP) {
        draw_help(m);
        want.cursor_visible = false;
    } else if (m->fl == FL_CHAT) {
        draw_chat(m, &crow, &ccol);
        if (crow >= 0) {
            want.cursor_row = crow;
            want.cursor_col = ccol;
            want.cursor_visible = true;
            want.bracketed_paste = true;
        }
    } else if (m->unread > 0) {
        char text[ALIAS_MAX_LEN + 64];
        snprintf(text, sizeof(text), " chat: %d new from %s | Ctrl-P Ctrl-T ", m->unread, m->unread_from);
        draw_badge(m, 0, m->tcols, text, &ST_NAME);
    }
    if (m->lead)
        draw_badge(m, m->trows - 1, m->tcols,
                   " Ctrl-P: Q detach  T chat  A attach  N names  W window  H help  P itself ", &ST_NAME);
    if (m->now < m->notice_until) put(m, m->trows - 1, 0, m->tcols, m->notice, &ST_BAD);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (comp_flush(m->comp, &want, &buf, &len, &cap) == 0 && len) (void)hold_write_all(STDOUT_FILENO, buf, len);
    free(buf);
}

/* ---- input ------------------------------------------------------------- */

static int slot_at(const struct mux *m, int row, int col) {
    int n = m->nwin ? win_count(m, m->view) : 0;
    for (int k = 0; k < n; k++) {
        const struct pane *p = m->slot[m->view * 4 + k];
        if (row >= p->row && row < p->row + p->rows && col >= p->col && col < p->col + p->cols) return m->view * 4 + k;
    }
    return -1;
}

/* A pointer report, SGR form: b;x;y then M (press, motion) or m (release). */
static void handle_mouse(struct mux *m, int b, int x, int y, bool release) {
    int row = y - 1, col = x - 1;
    bool motion = (b & 32) != 0;
    bool wheel = (b & 64) != 0;
    int button = b & 3;
    if (m->fl == FL_CHAT) {
        bool on_title = row == m->chat_y && col >= m->chat_x && col < m->chat_x + m->chat_w;
        if (!motion && !release && on_title && col == m->chat_x + m->chat_w - 2) {
            chat_close(m);
            return;
        }
        if (!motion && !release && on_title && button == 0) {
            m->dragging = true;
            m->drag_dx = col - m->chat_x;
            m->drag_dy = row - m->chat_y;
            return;
        }
        if (m->dragging && motion) {
            m->chat_x = col - m->drag_dx;
            m->chat_y = row - m->drag_dy;
            m->dirty = true;
            return;
        }
        if (m->dragging && release) {
            m->dragging = false;
            return;
        }
        bool inside = row >= m->chat_y && row < m->chat_y + m->chat_h && col >= m->chat_x && col < m->chat_x + m->chat_w;
        if (inside) return;
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
static void floater_chat_key(struct mux *m, const unsigned char *seq, size_t n, bool esc, bool enter);

/* Keys for the floating window that has the keyboard. seq is one key: a
 * byte, or a whole escape sequence. */
static void floater_key(struct mux *m, const unsigned char *seq, size_t n) {
    bool esc = n == 1 && seq[0] == 0x1b;
    bool up = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'A';
    bool down = n == 3 && seq[0] == 0x1b && (seq[1] == '[' || seq[1] == 'O') && seq[2] == 'B';
    bool enter = n == 1 && (seq[0] == '\r' || seq[0] == '\n');
    switch (m->fl) {
    case FL_HELP:
        if (esc || enter || (n == 1 && seq[0] == 'q')) {
            m->fl = FL_NONE;
            m->dirty = true;
        }
        return;
    case FL_ATTACH:
        if (esc) {
            m->fl = FL_NONE;
        } else if (up && m->pick_sel > 0) {
            m->pick_sel--;
        } else if (down && m->pick_sel + 1 < m->npicks) {
            m->pick_sel++;
        } else if (n == 1 && (seq[0] == 's' || seq[0] == 'S')) {
            open_floater(m, FL_SWAP);
        } else if (enter && m->npicks > 0) {
            const struct pick *p = &m->picks[m->pick_sel];
            const char *why = NULL;
            if (pane_total(m) >= MUX_MAX_PANES) {
                notice(m, "%d panes is the most", MUX_MAX_PANES);
            } else {
                struct pane *np = pane_open(p->id, p->name, p->sock, m->trows, m->tcols, &why);
                if (!np) notice(m, "cannot attach %s: %s", p->name, why ? why : "unknown");
                else (void)add_pane(m, np);
                if (np && win_count(m, m->view) == 1 && m->nwin > 1) m->window_note_until = m->now + MUX_NAME_MS;
            }
            m->fl = FL_NONE;
        }
        m->dirty = true;
        return;
    case FL_SWAP: {
        if (esc) {
            m->fl = FL_NONE;
            m->dirty = true;
            return;
        }
        if (n != 1) return;
        char word[2] = {(char)seq[0], 0};
        int s = slot_of_word(m, word);
        if (s < 0) return;
        if (m->swap_first < 0) {
            m->swap_first = s;
        } else {
            char why[160];
            if (do_swap(m, m->swap_first, s, why, sizeof(why)) != 0) notice(m, "%s", why);
            m->fl = FL_NONE;
        }
        m->dirty = true;
        return;
    }
    case FL_CHAT:
        floater_chat_key(m, seq, n, esc, enter);
        /* Saved as it changes, not only on close: another terminal that
         * opens chat takes over at once, with this draft and peer. */
        if (m->fl == FL_CHAT) chat_save_draft(m);
        return;
    default:
        return;
    }
}

static void floater_chat_key(struct mux *m, const unsigned char *seq, size_t n, bool esc, bool enter) {
    {
        if (esc) {
            chat_close(m);
        } else if (enter && m->pasting) {
            if (m->draft_len + 1 < sizeof(m->draft)) m->draft[m->draft_len++] = '\n';
        } else if (enter) {
            chat_send(m);
        } else if (n == 1 && seq[0] == '\t') {
            chat_next_peer(m);
        } else if (n == 1 && seq[0] == 0x0a) { /* Ctrl-J: a new line in the draft */
            if (m->draft_len + 1 < sizeof(m->draft)) m->draft[m->draft_len++] = '\n';
        } else if (n == 2 && seq[0] == 0x1b && (seq[1] == '\r' || seq[1] == '\n')) { /* Alt-Enter */
            if (m->draft_len + 1 < sizeof(m->draft)) m->draft[m->draft_len++] = '\n';
        } else if (n == 1 && (seq[0] == 0x7f || seq[0] == 0x08)) {
            while (m->draft_len > 0) {
                unsigned char c = (unsigned char)m->draft[--m->draft_len];
                if ((c & 0xc0) != 0x80) break; /* a whole UTF-8 character */
            }
        } else if (n == 1 && seq[0] == 0x15) { /* Ctrl-U */
            m->draft_len = 0;
        } else if (n == 6 && !memcmp(seq, "\033[200~", 6)) {
            m->pasting = true; /* a paste: its newlines stay in the draft */
        } else if (n == 6 && !memcmp(seq, "\033[201~", 6)) {
            m->pasting = false;
        } else if (n == 1 && seq[0] >= 0x20) {
            if (m->draft_len + 1 < sizeof(m->draft)) m->draft[m->draft_len++] = (char)seq[0];
        }
        m->dirty = true;
    }
}

static void open_floater(struct mux *m, enum floater f) {
    if (m->fl == FL_CHAT && f != FL_CHAT) chat_save_draft(m); /* only one floats */
    switch (f) {
    case FL_ATTACH:
        load_picks(m);
        m->fl = FL_ATTACH;
        break;
    case FL_SWAP:
        m->swap_first = -1;
        m->fl = FL_SWAP;
        break;
    case FL_HELP:
        m->fl = FL_HELP;
        break;
    case FL_CHAT:
        chat_open(m);
        break;
    default:
        m->fl = FL_NONE;
    }
    m->dirty = true;
}

/* The lead key, then: */
static int command(struct mux *m, unsigned char key) {
    switch (key) {
    case KEY_DETACH:
        return 1;
    case KEY_LEAD:
        if (m->fl == FL_CHAT) floater_key(m, &key, 1);
        else to_pane(m, &key, 1);
        return 0;
    case KEY_ATTACH:
        open_floater(m, m->fl == FL_ATTACH ? FL_NONE : FL_ATTACH);
        return 0;
    case KEY_CHAT:
        if (m->fl == FL_CHAT) chat_close(m);
        else open_floater(m, FL_CHAT);
        return 0;
    case KEY_HELP:
        open_floater(m, m->fl == FL_HELP ? FL_NONE : FL_HELP);
        return 0;
    case KEY_NAMES:
        m->names_until = m->now + MUX_NAME_MS;
        m->dirty = true;
        return 0;
    case KEY_WINDOW:
        if (m->nwin > 1) {
            m->view = (m->view + 1) % m->nwin;
            m->focus = m->view * 4;
            show_name(m, focused(m));
        }
        m->window_note_until = m->now + MUX_NAME_MS;
        m->dirty = true;
        m->advertise = true;
        return 0;
    default:
        return 0; /* anything else: cancelled, nothing sent */
    }
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
                    handle_mouse(m, mb, mx, my, s[k - 1] == 'm');
                }
            } else if (m->lead) {
                m->lead = false; /* a key that is not a command: cancelled */
                m->dirty = true;
            } else if (m->fl != FL_NONE) {
                floater_key(m, s, k);
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
        if (m->fl != FL_NONE) floater_key(m, &b, 1);
        else to_pane(m, &b, 1);
    }
    flush_keys(m);
    return rc;
}

/* ---- the loop ---------------------------------------------------------- */

static void drop_ended(struct mux *m) {
    bool any = false;
    for (int i = 0; i < MUX_MAX_PANES; i++) {
        struct pane *p = m->slot[i];
        if (p && p->ended) {
            notice(m, "%s has ended", p->name);
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
    m->chat_x = -1;
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
            relayout(m);
        }
        (void)hold_term_take_interrupt(); /* Ctrl-C arrives as a key for the focused pane */
        drop_ended(m);
        if (pane_total(m) == 0) break; /* every pane has ended */
        /* One pane left and nothing floating: back to plain attach, the
         * screen exactly as it is without panes. */
        if (pane_total(m) == 1 && m->fl == FL_NONE && !m->lead && m->now >= m->names_until) {
            struct pane *p = m->slot[0];
            snprintf(handback, hn, "%s", p->sock);
            (void)frame(p->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
            *handed_back = true;
            break;
        }
        chat_look(m);
        render(m);

        struct pollfd pfds[1 + MUX_MAX_PANES + 4];
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
        int sidx = (int)nf;
        for (int i = 0; i < 4; i++)
            pfds[nf++] = (struct pollfd){.fd = m->send[i].pid ? m->send[i].fd : -1, .events = POLLIN};
        int timeout = m->dirty ? MUX_FRAME_MS : MUX_LOOK_MS;
        int pr = poll(pfds, nf, timeout);
        if (pr < 0 && errno != EINTR) {
            rc = 1;
            break;
        }
        m->now = mono_ms();
        if (pr <= 0) {
            m->dirty = true; /* timers: names that expire, notices */
            continue;
        }
        for (int i = 0; i < MUX_MAX_PANES; i++) {
            if (idx[i] < 0 || !(pfds[idx[i]].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            struct pane *p = m->slot[i];
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
        for (int i = 0; i < 4; i++) {
            struct sending *s = &m->send[i];
            if (!s->pid || !(pfds[sidx + i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t k = read(s->fd, s->result + s->rlen, sizeof(s->result) - 1 - s->rlen);
            if (k > 0) s->rlen += (size_t)k;
            if (k <= 0 || memchr(s->result, '\n', s->rlen)) send_done(m, s);
        }
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buf[4096];
            ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
            if (k > 0) {
                if (handle_input(m, buf, (size_t)k) == 1) goto detach;
            } else if (k == 0) {
                goto detach;
            }
        }
        /* backmux swap, from anywhere: a request file names two panes */
        char req_path[HOLD_PATH_MAX], req[256];
        if (hold_checked_snprintf(req_path, sizeof(req_path) - 8, "%s/chat/mux.%d.swap", store->base, (int)getpid()) == 0 &&
            read_small(req_path, req, sizeof(req))) {
            unlink(req_path);
            char a[96] = "", b[96] = "", why2[160] = "bad request";
            int ok = -1;
            if (sscanf(req, "%95s %95s", a, b) == 2) {
                int sa = slot_of_word(m, a), sb = slot_of_word(m, b);
                if (sa < 0 || sb < 0) snprintf(why2, sizeof(why2), "no pane %s", sa < 0 ? a : b);
                else ok = do_swap(m, sa, sb, why2, sizeof(why2));
            }
            if (ok != 0) notice(m, "swap: %s", why2);
            /* the answer, for the backmux swap waiting on it */
            char res_path[HOLD_PATH_MAX], res[200];
            int rl = ok == 0 ? snprintf(res, sizeof(res), "ok\n") : snprintf(res, sizeof(res), "error %s\n", why2);
            if (hold_checked_snprintf(res_path, sizeof(res_path) - 8, "%s/chat/mux.%d.result", store->base, (int)getpid()) == 0)
                write_small(res_path, res, (size_t)rl);
        }
    }
    goto out;
detach:
    /* Detach: every pane lets go; every call keeps running. */
    for (int i = 0; i < MUX_MAX_PANES; i++)
        if (m->slot[i]) (void)frame(m->slot[i]->wfd, CONSOLE_FRAME_DETACH, NULL, 0);
    rc = 0;
out:
    if (m->fl == FL_CHAT) chat_save_draft(m);
    hold_term_signals_restore(&sig);
    for (int i = 0; i < MUX_MAX_PANES; i++) pane_free(m->slot[i]);
    for (int i = 0; i < 4; i++)
        if (m->send[i].pid) {
            close(m->send[i].fd);
            (void)waitpid(m->send[i].pid, NULL, 0);
        }
    for (int i = 0; i < m->nlines; i++) free(m->lines[i].text);
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
