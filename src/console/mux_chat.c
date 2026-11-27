/* console/mux_chat.c - the person's chat as the compositor hosts it: its
 * files (owner, conversation log), the inbox, opening it on a pane, and the
 * pane list backmux swap reads. */
#include "mux.h"

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

void mux_write_small(const char *path, const char *text, size_t len) {
    char tmp[HOLD_PATH_MAX + 16];
    if (hold_checked_snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid()) != 0) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    bool ok = hold_write_all(fd, text, len) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

size_t mux_read_small(const char *path, char *out, size_t cap) {
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
    if (!mux_read_small(path, text, sizeof(text))) return false;
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

void mux_on_sent(void *app, const char *label, const char *text, size_t len) {
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

void mux_chat_look(struct mux *m) {
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

void mux_chat_to_app(void *app, const unsigned char *bytes, size_t n) {
    mux_to_pane(app, bytes, n);
}

void mux_chat_pointer(void *app, int b, int x, int y, bool release) {
    mux_handle_mouse(app, b, x, y, release);
}

/* Open chat for the active pane, focus a visible chat window, or hide it
 * when it already has keyboard focus. Claim this identity's chat display. */
void mux_chat_key_pressed(struct mux *m) {
    if (!m->chat) {
        mux_notice(m, "chat unavailable: could not initialize this terminal's chat identity");
        return;
    }
    char path[HOLD_PATH_MAX], text[32];
    if (chat_path(m, "active", path, sizeof(path)) == 0) {
        int k = snprintf(text, sizeof(text), "%d\n", (int)getpid());
        mux_write_small(path, text, (size_t)k);
    }
    replay_log(m);
    m->fl = FL_NONE; /* one floating window at a time */
    struct pane *f = m->active;
    /* on the agent in focus, if it is one; chat talks only to agents */
    if (!chat_visible(m->chat) && f && f->id[0] && f->agent[0]) chat_open_on(m->chat, f->id, f->name);
    else if (chat_has_keyboard(m->chat)) chat_put_away(m->chat);
    else chat_toggle(m->chat);
    m->dirty = true;
}

/* chat/mux.<pid>: which panes this terminal shows, for backmux swap from
 * any shell or agent: "view W" (the window the view starts on, 0-based),
 * "viewport X Y" (in quadrants), then one "PLACE ID NAME" per pane, its
 * place in the base ("1A"), then one "shown PLACE NAME" per pane in view order. */
void mux_advertise(struct mux *m) {
    if (!m->advertise) return;
    m->advertise = false;
    char dir[HOLD_PATH_MAX], path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/chat", m->store->base) != 0 || hold_mkdir_p_mode(dir, 0700) != 0 ||
        hold_checked_snprintf(path, sizeof(path), "%s/mux.%d", dir, (int)getpid()) != 0)
        return;
    char buf[MUX_MAX_PANES * (ID_STR_LEN + 2 * ALIAS_MAX_LEN + 24) + 64];
    size_t k = (size_t)snprintf(buf, sizeof(buf), "view %d\nviewport %d %d\n", mux_view_window(m), m->vx, m->vy);
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->base[i] && k < sizeof(buf)) {
            char at[4];
            mux_position_label(m, m->base[i], at);
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s %s %s\n", at, m->base[i]->id[0] ? m->base[i]->id : "-",
                                  m->base[i]->name);
        }
    /* and where the view shows each: "shown PLACE NAME" (sort, spread and
     * pack move panes on screen, never in the base) */
    for (int i = 0; i < MUX_SLOTS; i++)
        if (m->shown[i] && k < sizeof(buf))
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "shown %d%c %s\n", i / MUX_POS + 1, "ABCDE"[i % MUX_POS],
                                  m->shown[i]->name);
    mux_write_small(path, buf, k < sizeof(buf) ? k : sizeof(buf) - 1);
}
