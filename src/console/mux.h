#pragma once
#ifndef BACKMUX_CONSOLE_MUX_H
#define BACKMUX_CONSOLE_MUX_H
/* console/mux.h - the compositor: pane composition and floating windows,
 * shared by the mux_*.c files (each says what it holds).
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

#define MUX_MAX_WINDOWS 9 /* a 3x3 grid, numbered 1-9 in reading order */
#define MUX_MAX_PANES (MUX_MAX_WINDOWS * 4) /* four to a window */
/* A window's places: A top-left, B right, C bottom-left, D bottom-right, E
 * the bottom across. Which are filled names the window's shape (mux_shape.c). */
enum { POS_A, POS_B, POS_C, POS_D, POS_E, MUX_POS };
#define MUX_SLOTS (MUX_MAX_WINDOWS * MUX_POS)
/* The view's arrangement: the base as it is, spread over every window, or
 * packed four to a window from the first. */
enum arrangement { ARR_BASE, ARR_SPREAD, ARR_PACK };
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
    bool visible; /* on screen in the current view */
    int order;    /* its place in the base's reading order (a stable sort by name) */
};

/* FL_PROCS: the process launcher - running terminals to attach as panes,
 * consoles that scroll to open in a viewer pane, ended processes to start
 * again. FL_JUMP: the panes by name. */
enum floater { FL_NONE, FL_PROCS, FL_JUMP, FL_LAYOUTS };
#define MUX_LAYOUT_MAX 32 /* layouts listed in the panel */
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
enum bar_act { BA_NONE, BA_TTY, BA_SCROLL, BA_ENDED, BA_CHAT, BA_INFO, BA_WINDOW, BA_ZOOM, BA_LOCK, BA_HELP,
               BA_SORT, BA_ARR, BA_LAYOUT, BA_SLIDE };
struct bar_hit {
    int x0, x1;
    enum bar_act act;
    int arg;
};

struct mux {
    const struct hold_store *store;
    char me[ALIAS_MAX_LEN + 1];
    /* The base layout: where panes really are, window w's place p at
     * base[w * MUX_POS + p]. Only attach, split, swap, move and a pane
     * leaving change it (mux_shape.c). shown is the base through the view. */
    struct pane *base[MUX_SLOTS];
    struct pane *shown[MUX_SLOTS];
    bool sort;             /* the view: by name (prefix a) */
    enum arrangement arr;  /* the view: base, spread (prefix w), pack (prefix d) */
    bool layout_dirty;     /* the base differs from the layout last loaded or saved */
    char layout_name[ALIAS_MAX_LEN + 1]; /* the layout last loaded or saved, "" none */
    /* the Layouts panel (mux_layouts.c) */
    char lay_names[MUX_LAYOUT_MAX][ALIAS_MAX_LEN + 1];
    char lay_default[ALIAS_MAX_LEN + 1];
    int nlay, lay_sel;
    bool lay_naming; /* typing a name for "save as" (into in) */
    struct pane *active;   /* the pane keys go to */
    struct pane *full;     /* the pane toggled full screen, NULL none */
    /* the viewport on the 3x3 grid, in quadrants (0..4 each): even = on a
     * window, odd = between two */
    int vx, vy;
    /* the screen as laid out last: the pane owning each quarter (top-left,
     * top-right, bottom-left, bottom-right) and where the lines are */
    struct pane *cell[4];
    int gx, gyl, gyr;
    /* the slide (mux_slide.c): the frame before the view moved, the one
     * after, how far it moved (quadrants) and since when */
    struct hold_snapshot slide_from, slide_to;
    bool slide_pending; /* the frame after is still to be composed */
    int slide_dx, slide_dy;
    int64_t slide_start;
    enum mode mode;
    bool help;   /* keyboard help for the active mode or prefix commands */
    bool swapping;  /* prefix s: places (1A, or A in this window) say what goes where */
    int swap_first; /* the first place chosen (a base slot), -1 none yet */
    char swap_digit; /* a window number typed, waiting for its letter, 0 none */
    enum prompt prompt;
    struct pane *prompt_pane;
    char in[ALIAS_MAX_LEN + 1];
    size_t in_len;
    /* where each window's lines are, in percent: the line between left and
     * right, and the line in each column (resize mode moves them) */
    int colpct[MUX_MAX_WINDOWS], lpct[MUX_MAX_WINDOWS], rpct[MUX_MAX_WINDOWS];
    int64_t fifo_probe_at; /* the next read of FIFO panes poll said nothing of */
    bool assumed;          /* the frame on the terminal was assumed (inline) */
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
extern bool g_bar;
extern bool g_info;
extern bool g_slide; /* the view slides when it moves (config slide; the bar's Slide) */
extern char g_locked[MUX_MAX_PANES][ID_STR_LEN];

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


/* ---- the compositor's functions, by file ---------------------------- */
/* mux_pane.c */
bool mux_pane_locked(const struct pane *p);
void mux_set_locked(const struct pane *p, bool on);
int64_t mux_mono_ms(void);
void mux_notice(struct mux *m, const char *fmt, ...);
int mux_frame(int fd, unsigned char type, const void *payload, uint16_t len);
void mux_pane_send(struct pane *p, const void *bytes, size_t n);
void mux_pane_resize(struct pane *p, int rows, int cols);
void mux_pane_free(struct pane *p);
struct pane *mux_pane_open(const char *id, const char *name, const char *sock, int rows, int cols, const char **why);
void mux_pane_identify(struct mux *m, struct pane *p);
void mux_pane_load(struct pane *p, int64_t now);
int mux_run_self(char *const args[], char *err, size_t en, char *out, size_t on);
int mux_run_self_in(const char *cwd, char *const args[], char *err, size_t en, char *out, size_t on);
/* mux_shape.c */
bool mux_shape_valid(unsigned mask);
unsigned mux_window_mask(struct pane *const *slots, int w);
int mux_window_count(struct pane *const *slots, int w);
void mux_shape_rect(unsigned mask, int pos, int *x, int *y, int *w, int *h);
bool mux_reshape(struct pane *const cur[MUX_POS], struct pane *extra, struct pane *pin, int pin_pos,
                 struct pane *out[MUX_POS]);
int mux_base_slot(const struct mux *m, const struct pane *p);
int mux_shown_slot(const struct mux *m, const struct pane *p);
void mux_position_label(const struct mux *m, const struct pane *p, char out[4]);
int mux_parse_position(const char *w);
bool mux_base_add(struct mux *m, struct pane *p);
void mux_base_remove(struct mux *m, struct pane *p);
const char *mux_base_send(struct mux *m, struct pane *p, int to);
bool mux_can_split(const struct mux *m, char dir);
bool mux_base_split(struct mux *m, struct pane *np, char dir);
const char *mux_base_to_window(struct mux *m, struct pane *p, int w);
void mux_view_compute(struct mux *m);
int mux_pane_total(const struct mux *m);
bool mux_has_pane(const struct mux *m, const char *id);
/* mux_layout.c */
int mux_view_window(const struct mux *m);
bool mux_view_aligned(const struct mux *m);
int mux_window_key(int w);
void mux_go_alt(struct mux *m);
bool mux_decorated(const struct mux *m);
bool mux_bar_on(const struct mux *m);
bool mux_info_on(const struct mux *m);
void mux_relayout(struct mux *m);
void mux_set_focus(struct mux *m, struct pane *p);
void mux_align(struct mux *m, int w);
void mux_settle(struct mux *m);
void mux_focus_in_view(struct mux *m);
int mux_add_pane(struct mux *m, struct pane *p);
struct pane *mux_pane_at(const struct mux *m, int row, int col);
struct pane *mux_neighbor(const struct mux *m, char dir);
bool mux_shift(struct mux *m, char dir);
int mux_window_step(const struct mux *m, int w, int step);
void mux_cycle(struct mux *m, int step);
bool mux_resize_toward(struct mux *m, char dir, bool grow);
/* mux_find.c */
struct pane *mux_find(const struct mux *m, const char *word, int *slot, const char **why);
/* mux_slide.c */
/* mux_layouts.c */
bool mux_layout_save(struct mux *m, const char *name, const char **why);
bool mux_layout_load(struct mux *m, const char *name, struct pane *keep, const char **why);
void mux_layouts_open(struct mux *m);
void mux_draw_layouts(struct mux *m);
void mux_layouts_key(struct mux *m, const unsigned char *k, size_t n);
void mux_serve_requests(struct mux *m);
void mux_slide_begin(struct mux *m, int nx, int ny);
bool mux_slide_frame(struct mux *m, struct comp_want *want);
bool mux_sliding(const struct mux *m);
void mux_slide_stop(struct mux *m);
/* mux_procs.c */
void mux_load_picks(struct mux *m);
void mux_refresh_stats(struct mux *m);
void mux_draw_procs(struct mux *m);
void mux_launch_pick(struct mux *m, const struct pick *p);
void mux_new_terminal(struct mux *m, char split);
struct pane *mux_open_id(struct mux *m, const char *id, const char *fallback_name);
bool mux_start_again(struct mux *m, const char *id, const char *name, bool tty, bool pipe, bool shell);
/* mux_chat.c */
bool mux_write_small(const char *path, const char *text, size_t len); /* false: not written */
size_t mux_read_small(const char *path, char *out, size_t cap);
void mux_on_sent(void *app, const char *label, const char *text, size_t len);
void mux_chat_look(struct mux *m);
void mux_chat_to_app(void *app, const unsigned char *bytes, size_t n);
void mux_chat_pointer(void *app, int b, int x, int y, bool release);
void mux_chat_key_pressed(struct mux *m);
void mux_advertise(struct mux *m);
/* mux_draw.c */
int mux_put(struct mux *m, int row, int col, int max, const char *text, const struct comp_style *st);
int mux_text_cols(const char *s);
void mux_draw_frame(struct mux *m, int y, int x, int h, int w, const char *title, const char *hint);
void mux_key_label(unsigned char k, char out[16]);
void mux_human_bytes(uint64_t b, char *out, size_t n);
void mux_render(struct mux *m);
void mux_compose_panes(struct mux *m, struct comp_want *want);
void mux_finish_frame(struct mux *m, struct comp_want *want);
/* mux_help.c */
void mux_draw_help(struct mux *m);
/* mux_bar.c */
void mux_draw_bar(struct mux *m);
void mux_bar_click(struct mux *m, enum bar_act act, int arg);
/* mux_keys.c */
void mux_prompt_open(struct mux *m, enum prompt kind);
void mux_prompt_key(struct mux *m, const unsigned char *k, size_t n);
void mux_swap_key(struct mux *m, const unsigned char *k, size_t n);
void mux_swap_choose(struct mux *m, int slot);
char mux_arrow_of(const unsigned char *k, size_t n, int *mod, bool letters);
void mux_mode_key(struct mux *m, const unsigned char *k, size_t n);
void mux_floater_key(struct mux *m, const unsigned char *seq, size_t n);
void mux_open_floater(struct mux *m, enum floater f);
int mux_command(struct mux *m, unsigned char key);
/* mux_input.c */
void mux_handle_mouse(struct mux *m, int b, int x, int y, bool release);
void mux_to_pane(struct mux *m, const unsigned char *b, size_t n);
int mux_handle_input(struct mux *m, const unsigned char *buf, size_t n);
/* mux.c */

#endif
