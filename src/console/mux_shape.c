/* console/mux_shape.c - the base layout and the views of it.
 *
 * Panes live in the base: nine windows of five positions, A B C D E. Which
 * positions are filled IS the window's shape - the letters name it, nothing
 * else is stored:
 *
 *   A     AB     AE     ABC     ABD     ACD     ABE     ABCD
 *   A     A|B    A/E    A|B     A|B     A       A|B     A|B
 *                       C|B     A|D     C|D     E       C|D
 *
 * With E, the bottom is one pane across. Without E but with B, the window
 * is in columns (A over C, B over D; a missing C or D lets the pane above
 * take the column). With neither, A spans the top over C|D, or A is alone.
 * Why letters, not a split tree: a position is then a name for a place
 * ("1A", "2E") that a person, a script or an agent can say and mean the
 * same thing, and one function draws every window.
 *
 * Every change to placement goes through here: an attach, a split, a swap,
 * a pane leaving. The views - sort, spread, pack - are computed from the
 * base into m->shown and never written back. Why: untoggling a view is
 * always the base again, exactly as it was. */
#include "mux.h"

/* Every valid shape, in the order a tie between them is broken. */
static const unsigned shapes[] = {
    1u << POS_A,
    (1u << POS_A) | (1u << POS_B),
    (1u << POS_A) | (1u << POS_E),
    (1u << POS_A) | (1u << POS_B) | (1u << POS_C),
    (1u << POS_A) | (1u << POS_B) | (1u << POS_D),
    (1u << POS_A) | (1u << POS_C) | (1u << POS_D),
    (1u << POS_A) | (1u << POS_B) | (1u << POS_E),
    (1u << POS_A) | (1u << POS_B) | (1u << POS_C) | (1u << POS_D),
};
#define NSHAPES (int)(sizeof(shapes) / sizeof(shapes[0]))

static int popcount5(unsigned m) {
    int n = 0;
    for (int i = 0; i < MUX_POS; i++) n += (m >> i) & 1u;
    return n;
}

bool mux_shape_valid(unsigned mask) {
    if (!mask) return true; /* an empty window */
    for (int i = 0; i < NSHAPES; i++)
        if (shapes[i] == mask) return true;
    return false;
}

unsigned mux_window_mask(struct pane *const *slots, int w) {
    unsigned mask = 0;
    for (int p = 0; p < MUX_POS; p++)
        if (slots[w * MUX_POS + p]) mask |= 1u << p;
    return mask;
}

int mux_window_count(struct pane *const *slots, int w) {
    return popcount5(mux_window_mask(slots, w));
}

/* Where a position sits in its window, in quadrants (x, y in 0..1, w, h in
 * 1..2), for the shape it is part of. */
void mux_shape_rect(unsigned mask, int pos, int *x, int *y, int *w, int *h) {
    bool has_b = mask & (1u << POS_B), has_c = mask & (1u << POS_C), has_d = mask & (1u << POS_D),
         has_e = mask & (1u << POS_E);
    *x = 0, *y = 0, *w = 1, *h = 1;
    switch (pos) {
    case POS_A:
        if (!has_b) *w = 2;           /* A, AE, ACD: across the top */
        if (!has_c && !has_e) *h = 2; /* A, AB, ABD: the whole left (or all of it) */
        (void)has_d;
        break;
    case POS_B:
        *x = 1;
        if (!has_d && !has_e) *h = 2; /* AB, ABC: the whole right */
        break;
    case POS_C: *y = 1; break;
    case POS_D: *x = 1, *y = 1; break;
    case POS_E: *y = 1, *w = 2; break;
    default: break;
    }
}

/* Twice a position's middle, for how far a pane moves when a window
 * changes shape: A (0,0) B (2,0) C (0,2) D (2,2) E (1,2). */
static void center2(int pos, int *x, int *y) {
    static const int cx[] = {0, 2, 0, 2, 1}, cy[] = {0, 0, 2, 2, 2};
    *x = cx[pos], *y = cy[pos];
}

/* The panes of one window into a shape of n panes: pin (if any) at
 * pin_pos, the rest kept where they are when the shape has their place,
 * the others filling its free places in reading order. The shape chosen
 * keeps the most panes in place, then moves them least, then comes first
 * in shapes[]. extra joins with no place yet. false: no shape fits. */
bool mux_reshape(struct pane *const cur[MUX_POS], struct pane *extra, struct pane *pin, int pin_pos,
                 struct pane *out[MUX_POS]) {
    int n = extra ? 1 : 0;
    for (int p = 0; p < MUX_POS; p++) n += cur[p] != NULL;
    if (n == 0) {
        memset(out, 0, sizeof(*out) * MUX_POS);
        return true;
    }
    int best = -1, best_kept = -1, best_dist = INT_MAX;
    for (int s = 0; s < NSHAPES; s++) {
        unsigned sh = shapes[s];
        if (popcount5(sh) != n || (pin_pos >= 0 && !(sh & (1u << pin_pos)))) continue;
        int kept = 0, dist = 0;
        for (int p = 0; p < MUX_POS; p++) {
            if (!cur[p] || cur[p] == pin) continue;
            if ((sh & (1u << p)) && p != pin_pos) kept++;
        }
        /* distance: movers to the free places, in reading order */
        int fx[MUX_POS], fy[MUX_POS], nf = 0;
        for (int p = 0; p < MUX_POS; p++) {
            bool taken = p == pin_pos;
            for (int q = 0; q < MUX_POS && !taken; q++) taken = q == p && cur[q] && cur[q] != pin && (sh & (1u << q));
            if ((sh & (1u << p)) && !taken) center2(p, &fx[nf], &fy[nf]), nf++;
        }
        int k = 0;
        for (int p = 0; p < MUX_POS; p++) {
            if (!cur[p] || cur[p] == pin || ((sh & (1u << p)) && p != pin_pos)) continue;
            int x, y;
            center2(p, &x, &y);
            if (k < nf) dist += abs(x - fx[k]) + abs(y - fy[k]);
            k++;
        }
        if (kept > best_kept || (kept == best_kept && dist < best_dist)) best = s, best_kept = kept, best_dist = dist;
    }
    if (best < 0) return false;
    unsigned sh = shapes[best];
    struct pane *res[MUX_POS] = {0};
    if (pin && pin_pos >= 0) res[pin_pos] = pin;
    struct pane *movers[MUX_POS + 1];
    int nm = 0;
    for (int p = 0; p < MUX_POS; p++) {
        if (!cur[p] || cur[p] == pin) continue;
        if ((sh & (1u << p)) && p != pin_pos) res[p] = cur[p];
        else movers[nm++] = cur[p];
    }
    if (extra && extra != pin) movers[nm++] = extra;
    for (int p = 0, k = 0; p < MUX_POS && k < nm; p++)
        if ((sh & (1u << p)) && !res[p]) res[p] = movers[k++];
    memcpy(out, res, sizeof(res));
    return true;
}

/* ---- the base ----------------------------------------------------------- */

int mux_base_slot(const struct mux *m, const struct pane *p) {
    for (int i = 0; p && i < MUX_SLOTS; i++)
        if (m->base[i] == p) return i;
    return -1;
}

int mux_shown_slot(const struct mux *m, const struct pane *p) {
    for (int i = 0; p && i < MUX_SLOTS; i++)
        if (m->shown[i] == p) return i;
    return -1;
}

/* "1A": a pane's place in the base, by window number and position. */
void mux_position_label(const struct mux *m, const struct pane *p, char out[4]) {
    int s = mux_base_slot(m, p);
    if (s < 0) snprintf(out, 4, "?");
    else snprintf(out, 4, "%d%c", s / MUX_POS + 1, "ABCDE"[s % MUX_POS]);
}

/* "1A".."9E" (either case) as a base slot; -1 if the word is not one. */
int mux_parse_position(const char *w) {
    if (!w || w[0] < '1' || w[0] > '9' || !w[1] || w[2]) return -1;
    int pos = toupper((unsigned char)w[1]) - 'A';
    if (pos < 0 || pos >= MUX_POS) return -1;
    return (w[0] - '1') * MUX_POS + pos;
}

/* Put window w's panes into a valid shape, extra joining it. */
static bool window_reshape(struct mux *m, int w, struct pane *extra, struct pane *pin, int pin_pos) {
    struct pane *out[MUX_POS];
    if (!mux_reshape(m->base + w * MUX_POS, extra, pin, pin_pos, out)) return false;
    memcpy(m->base + w * MUX_POS, out, sizeof(out));
    m->layout_dirty = true;
    return true;
}

/* Where an attach lands in the base. In spread, the window with the fewest
 * panes (the lowest on a tie): spreading while adding keeps the base as
 * even as the view. Otherwise the active pane's window if it has room, else
 * the first with room. Then the window grows into the next shape. */
bool mux_base_add(struct mux *m, struct pane *p) {
    int w = -1;
    if (m->arr == ARR_SPREAD) {
        for (int i = 0; i < MUX_MAX_WINDOWS; i++)
            if (mux_window_count(m->base, i) < 4 && (w < 0 || mux_window_count(m->base, i) < mux_window_count(m->base, w))) w = i;
    } else {
        int a = mux_base_slot(m, m->active);
        if (a >= 0 && mux_window_count(m->base, a / MUX_POS) < 4) w = a / MUX_POS;
        for (int i = 0; i < MUX_MAX_WINDOWS && w < 0; i++)
            if (mux_window_count(m->base, i) < 4) w = i;
    }
    return w >= 0 && window_reshape(m, w, p, NULL, -1);
}

/* A pane leaves: the rest of its window keep their places when that is a
 * shape, else close up into the nearest one. */
void mux_base_remove(struct mux *m, struct pane *p) {
    int s = mux_base_slot(m, p);
    if (s < 0) return;
    m->base[s] = NULL;
    int w = s / MUX_POS;
    if (!mux_shape_valid(mux_window_mask(m->base, w))) (void)window_reshape(m, w, NULL, NULL, -1);
    m->layout_dirty = true;
}

/* Swap, by places: two panes trade places, or a pane goes to a free place
 * - its window takes the shape that has that place (the sent pane there,
 * the others keeping theirs where they can, the rest in reading order).
 * Returns NULL, or why not. */
const char *mux_base_send(struct mux *m, struct pane *p, int to) {
    int from = mux_base_slot(m, p);
    if (from < 0 || to < 0 || to >= MUX_SLOTS) return "no such place";
    if (from == to) return NULL;
    struct pane *there = m->base[to];
    if (there) {
        m->base[to] = p;
        m->base[from] = there;
        m->layout_dirty = true;
        return NULL;
    }
    int wf = from / MUX_POS, wt = to / MUX_POS;
    struct pane *save[MUX_SLOTS];
    memcpy(save, m->base, sizeof(save));
    if (wf != wt) {
        if (mux_window_count(m->base, wt) >= 4) return "that window is full";
        m->base[from] = NULL;
        if (!window_reshape(m, wt, p, p, to % MUX_POS)) {
            memcpy(m->base, save, sizeof(save));
            return "no shape of that window has that place";
        }
        if (!mux_shape_valid(mux_window_mask(m->base, wf))) (void)window_reshape(m, wf, NULL, NULL, -1);
    } else if (!window_reshape(m, wt, NULL, p, to % MUX_POS)) {
        memcpy(m->base, save, sizeof(save));
        return "no shape with that many panes has that place";
    }
    m->layout_dirty = true;
    return NULL;
}

/* Move mode's number: the pane joins window w, which grows into its next
 * shape; its own window closes up. NULL, or why not. */
const char *mux_base_to_window(struct mux *m, struct pane *p, int w) {
    int from = mux_base_slot(m, p);
    if (from < 0 || w < 0 || w >= MUX_MAX_WINDOWS) return "no such window";
    if (from / MUX_POS == w) return NULL;
    if (mux_window_count(m->base, w) >= 4) return "that window is full";
    m->base[from] = NULL;
    if (!window_reshape(m, w, p, NULL, -1)) {
        m->base[from] = p;
        return "that window is full";
    }
    if (!mux_shape_valid(mux_window_mask(m->base, from / MUX_POS))) (void)window_reshape(m, from / MUX_POS, NULL, NULL, -1);
    return NULL;
}

/* A split of the active pane: right halves it across, down halves it top
 * to bottom. The table is every split a shape has room for. */
static const struct split {
    unsigned shape;
    char from, dir; /* the pane split, 'r' or 'd' */
    char to;        /* where the new pane goes */
    bool e_to_c;    /* E becomes C (the bottom divides in two) */
} splits[] = {
    {1u << POS_A, 'A', 'r', 'B', false},
    {(1u << POS_A) | (1u << POS_E), 'A', 'r', 'B', false},
    {(1u << POS_A) | (1u << POS_E), 'E', 'r', 'D', true},
    {(1u << POS_A) | (1u << POS_B) | (1u << POS_E), 'E', 'r', 'D', true},
    {(1u << POS_A) | (1u << POS_C) | (1u << POS_D), 'A', 'r', 'B', false},
    {1u << POS_A, 'A', 'd', 'E', false},
    {(1u << POS_A) | (1u << POS_B), 'A', 'd', 'C', false},
    {(1u << POS_A) | (1u << POS_B), 'B', 'd', 'D', false},
    {(1u << POS_A) | (1u << POS_B) | (1u << POS_C), 'B', 'd', 'D', false},
    {(1u << POS_A) | (1u << POS_B) | (1u << POS_D), 'A', 'd', 'C', false},
};

/* Can the active pane be split that way? */
bool mux_can_split(const struct mux *m, char dir) {
    int s = mux_base_slot(m, m->active);
    if (s < 0) return false;
    unsigned mask = mux_window_mask(m->base, s / MUX_POS);
    for (size_t i = 0; i < sizeof(splits) / sizeof(splits[0]); i++)
        if (splits[i].shape == mask && splits[i].from == "ABCDE"[s % MUX_POS] && splits[i].dir == dir) return true;
    return false;
}

bool mux_base_split(struct mux *m, struct pane *np, char dir) {
    int s = mux_base_slot(m, m->active);
    if (s < 0) return false;
    int w = s / MUX_POS;
    unsigned mask = mux_window_mask(m->base, w);
    for (size_t i = 0; i < sizeof(splits) / sizeof(splits[0]); i++) {
        const struct split *sp = &splits[i];
        if (sp->shape != mask || sp->from != "ABCDE"[s % MUX_POS] || sp->dir != dir) continue;
        struct pane **win = m->base + w * MUX_POS;
        if (sp->e_to_c) {
            win[POS_C] = win[POS_E];
            win[POS_E] = NULL;
        }
        win[sp->to - 'A'] = np;
        m->layout_dirty = true;
        return true;
    }
    return false;
}

/* ---- the views ---------------------------------------------------------- */

static int by_name(const void *a, const void *b) {
    const struct pane *x = *(struct pane *const *)a, *y = *(struct pane *const *)b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : (x->order > y->order) - (x->order < y->order); /* stable: base order */
}

/* The default shape for n panes, as panes arrive: A, AB, ABC, ABCD. */
static unsigned auto_shape(int n) {
    return n <= 0 ? 0 : n == 1 ? shapes[0] : n == 2 ? shapes[1] : n == 3 ? shapes[3] : shapes[7];
}

static void fill_window(struct pane **shown, int w, struct pane **panes, int n) {
    unsigned sh = auto_shape(n);
    for (int p = 0, k = 0; p < MUX_POS && k < n; p++)
        if (sh & (1u << p)) shown[w * MUX_POS + p] = panes[k++];
}

/* The base through the view: in the base's reading order (window by
 * window, A to E) or by name, then laid out as the base has them, four to
 * a window from the first (pack), or over all nine (spread). */
void mux_view_compute(struct mux *m) {
    struct pane *list[MUX_SLOTS];
    int n = 0;
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->base[i]) {
            m->base[i]->order = n;
            list[n++] = m->base[i];
        }
    if (m->sort) qsort(list, (size_t)n, sizeof(list[0]), by_name);
    memset(m->shown, 0, sizeof(m->shown));
    if (m->arr == ARR_BASE) {
        for (int i = 0, k = 0; i < MUX_SLOTS; i++)
            if (m->base[i]) m->shown[i] = list[k++];
    } else if (m->arr == ARR_PACK) {
        for (int w = 0, k = 0; w < MUX_MAX_WINDOWS && k < n; w++) {
            int here = n - k < 4 ? n - k : 4;
            fill_window(m->shown, w, list + k, here);
            k += here;
        }
    } else {
        for (int w = 0, k = 0; w < MUX_MAX_WINDOWS; w++) {
            int here = n / MUX_MAX_WINDOWS + (w < n % MUX_MAX_WINDOWS);
            if (here > 4) here = 4;
            fill_window(m->shown, w, list + k, here);
            k += here;
        }
    }
}

int mux_pane_total(const struct mux *m) {
    int n = 0;
    for (int i = 0; i < MUX_SLOTS; i++) n += m->base[i] != NULL;
    return n;
}

bool mux_has_pane(const struct mux *m, const char *id) {
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->base[i] && !strcmp(m->base[i]->id, id)) return true;
    return false;
}
