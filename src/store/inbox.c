#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/store.h"

/* Operator inboxes: where replies go for an agent that runs outside any process
 * (a Claude Code session in a plain terminal, a script). Such a sender has no
 * process to type a reply into, so it gets a name like any process's and an inbox
 * under that name in the store's inbox/ directory:
 *
 *   <name>.who    who the name belongs to (the harness process: name, pid,
 *                 start time), one line; its mtime is when it was last used
 *   <name>.msgs   one line per message: UNIXTIME FROMHEX TEXTHEX
 *   <name>.seen   how far into .msgs has been shown (a byte offset), so
 *                 "the next message" means the next unread one
 *
 * Why files and no daemon: the sender may not be running when a reply comes,
 * and backmux keeps no process for it. A name and its messages expire after
 * HOLD_INBOX_TTL_S unused; nothing is rewritten in place (messages are only
 * appended, and expired ones are skipped on read and removed with their name),
 * so a reader's offset into .msgs stays valid while it waits. */

static int inbox_path(const struct hold_store *s, const char *name, const char *ext, char *out, size_t n) {
    if (!s || !name || !hold_valid_alias(name)) {
        errno = EINVAL;
        return -1;
    }
    return hold_checked_snprintf(out, n, "%s/inbox/%s%s", s->base, name, ext);
}

int hold_inbox_dir(const struct hold_store *s, char *out, size_t n) {
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    if (hold_checked_snprintf(out, n, "%s/inbox", s->base) != 0) return -1;
    if (hold_mkdir_p_mode(out, 0700) != 0 || hold_chmod_dir_no_symlink(out, 0700) != 0) return -1;
    return 0;
}

static bool fresh(const struct stat *st) {
    return time(NULL) - st->st_mtime <= HOLD_INBOX_TTL_S;
}

bool hold_inbox_live(const struct hold_store *s, const char *name) {
    char path[HOLD_PATH_MAX];
    struct stat st;
    if (inbox_path(s, name, ".who", path, sizeof(path)) != 0) return false;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return fresh(&st);
}

void hold_inbox_touch(const struct hold_store *s, const char *name) {
    char path[HOLD_PATH_MAX];
    if (inbox_path(s, name, ".who", path, sizeof(path)) == 0) (void)utimensat(AT_FDCWD, path, NULL, AT_SYMLINK_NOFOLLOW);
}

/* Remove a name and its messages (expired, or never fully made). */
static void drop_inbox(const struct hold_store *s, const char *name) {
    char path[HOLD_PATH_MAX];
    if (inbox_path(s, name, ".seen", path, sizeof(path)) == 0) unlink(path);
    if (inbox_path(s, name, ".msgs", path, sizeof(path)) == 0) unlink(path);
    if (inbox_path(s, name, ".who", path, sizeof(path)) == 0) unlink(path);
}

int hold_inbox_find(const struct hold_store *s, const char *key, char *name, size_t n) {
    char dir[HOLD_PATH_MAX];
    if (hold_inbox_dir(s, dir, sizeof(dir)) != 0) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int found = 1;
    const struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || strcmp(e->d_name + len - 4, ".who") != 0 || len - 4 > ALIAS_MAX_LEN) continue;
        char who[ALIAS_MAX_LEN + 1];
        memcpy(who, e->d_name, len - 4);
        who[len - 4] = '\0';
        if (!hold_valid_alias(who)) continue;
        char path[HOLD_PATH_MAX];
        struct stat st;
        if (inbox_path(s, who, ".who", path, sizeof(path)) != 0 || lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!fresh(&st)) {
            drop_inbox(s, who); /* expired: the name and its messages go */
            continue;
        }
        char *text = NULL;
        size_t tlen = 0;
        if (found == 1 && hold_read_owned_file_capped(path, 512, &text, &tlen) == 0) {
            while (tlen > 0 && (text[tlen - 1] == '\n' || text[tlen - 1] == '\r')) text[--tlen] = '\0';
            if (!strcmp(text, key) && hold_checked_snprintf(name, n, "%s", who) == 0) found = 0;
        }
        free(text);
    }
    closedir(d);
    if (found == 0) hold_inbox_touch(s, name);
    return found;
}

int hold_inbox_claim(const struct hold_store *s, const char *name, const char *key) {
    char dir[HOLD_PATH_MAX], path[HOLD_PATH_MAX];
    if (hold_inbox_dir(s, dir, sizeof(dir)) != 0 || inbox_path(s, name, ".who", path, sizeof(path)) != 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1; /* EEXIST: someone has the name */
    size_t kl = strlen(key);
    int rc = hold_write_all(fd, key, kl) == 0 && hold_write_all(fd, "\n", 1) == 0 ? 0 : -1;
    int saved = errno;
    close(fd);
    if (rc != 0) {
        unlink(path);
        errno = saved;
    }
    return rc;
}

int hold_inbox_deliver(const struct hold_store *s, const char *name, const char *from, const char *text, size_t len) {
    char path[HOLD_PATH_MAX];
    if (!hold_inbox_live(s, name)) {
        errno = ENOENT;
        return -1;
    }
    if (inbox_path(s, name, ".msgs", path, sizeof(path)) != 0) return -1;
    if (len > HOLD_INBOX_MAX_TEXT) len = HOLD_INBOX_MAX_TEXT;
    size_t fl = strlen(from);
    size_t cap = 32 + fl * 2 + len * 2 + 3;
    char *line = malloc(cap);
    if (!line) return -1;
    int n = snprintf(line, cap, "%lld ", (long long)time(NULL));
    hold_hex_encode((const unsigned char *)from, fl, line + n, cap - (size_t)n);
    n += (int)(fl * 2);
    line[n++] = ' ';
    hold_hex_encode((const unsigned char *)text, len, line + n, cap - (size_t)n);
    n += (int)(len * 2);
    line[n++] = '\n';
    int fd = hold_open_append_no_symlink(path);
    int rc = -1;
    if (fd >= 0) {
        /* One write per message, under a lock, so concurrent senders never
         * interleave and a reader never sees half a line it keeps. */
        (void)flock(fd, LOCK_EX);
        rc = hold_write_all(fd, line, (size_t)n);
        (void)flock(fd, LOCK_UN);
        close(fd);
    }
    free(line);
    if (rc == 0) hold_inbox_touch(s, name);
    return rc;
}

static int unhex(const char *h, size_t hl, char *out) {
    for (size_t i = 0; i < hl; i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[i + (size_t)k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i / 2] = (char)v;
    }
    return 0;
}

off_t hold_inbox_size(const struct hold_store *s, const char *name) {
    char path[HOLD_PATH_MAX];
    struct stat st;
    if (inbox_path(s, name, ".msgs", path, sizeof(path)) != 0 || lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return st.st_size;
}

int hold_inbox_read(const struct hold_store *s, const char *name, off_t from_off, off_t *end_off, hold_inbox_fn fn, void *ctx) {
    char path[HOLD_PATH_MAX];
    if (end_off) *end_off = from_off;
    if (inbox_path(s, name, ".msgs", path, sizeof(path)) != 0) return -1;
    struct stat st;
    int fd = hold_open_owned_file_no_symlink(path, &st);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    if (st.st_size <= from_off) {
        close(fd);
        return 0;
    }
    size_t want = (size_t)(st.st_size - from_off);
    char *buf = malloc(want + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    ssize_t got = pread(fd, buf, want, from_off);
    close(fd);
    if (got < 0) {
        free(buf);
        return -1;
    }
    /* Only whole lines: a line still being written is read on a later pass. */
    size_t used = 0, count = 0;
    time_t now = time(NULL);
    int rc = 0;
    for (size_t i = 0; i < (size_t)got;) {
        char *nl = memchr(buf + i, '\n', (size_t)got - i);
        if (!nl) break;
        *nl = '\0';
        char *line = buf + i, *sp1 = strchr(line, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        size_t next = (size_t)(nl - buf) + 1;
        if (sp1 && sp2) {
            long long at = strtoll(line, NULL, 10);
            size_t fl = (size_t)(sp2 - sp1 - 1), tl = strlen(sp2 + 1);
            char *from = malloc(fl / 2 + 1), *text = malloc(tl / 2 + 1);
            if (from && text && fl % 2 == 0 && tl % 2 == 0 && unhex(sp1 + 1, fl, from) == 0 &&
                unhex(sp2 + 1, tl, text) == 0 && now - (time_t)at <= HOLD_INBOX_TTL_S) {
                from[fl / 2] = '\0';
                text[tl / 2] = '\0';
                size_t clean = hold_strip_controls(text, tl / 2);
                text[clean] = '\0';
                size_t fclean = hold_strip_controls(from, fl / 2);
                from[fclean] = '\0';
                count++;
                if (fn) rc = fn((time_t)at, from, text, clean, ctx);
            }
            free(from);
            free(text);
        }
        used = next;
        i = next;
        if (rc != 0) break; /* the callback has what it wanted: the rest waits */
    }
    free(buf);
    if (end_off) *end_off = from_off + (off_t)used;
    hold_inbox_touch(s, name);
    return count > 0 ? 1 : 0;
}

off_t hold_inbox_seen(const struct hold_store *s, const char *name) {
    char path[HOLD_PATH_MAX], *text = NULL;
    size_t len = 0;
    if (inbox_path(s, name, ".seen", path, sizeof(path)) != 0 || hold_read_owned_file_capped(path, 64, &text, &len) != 0)
        return 0;
    long long v = strtoll(text, NULL, 10);
    free(text);
    off_t size = hold_inbox_size(s, name);
    return v < 0 ? 0 : (off_t)v > size ? size : (off_t)v;
}

int hold_inbox_mark_seen(const struct hold_store *s, const char *name, off_t off) {
    char path[HOLD_PATH_MAX], dir[HOLD_PATH_MAX], line[32];
    if (inbox_path(s, name, ".seen", path, sizeof(path)) != 0 || hold_inbox_dir(s, dir, sizeof(dir)) != 0) return -1;
    if (off <= hold_inbox_seen(s, name)) return 0; /* never moves back */
    int n = snprintf(line, sizeof(line), "%lld\n", (long long)off);
    /* temp file, then rename: a reader never sees a half-written offset */
    char tmp[HOLD_PATH_MAX];
    int fd = hold_open_unique_temp(dir, "seen", 0600, tmp, sizeof(tmp));
    if (fd < 0) return -1;
    int rc = hold_write_all(fd, line, (size_t)n);
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(tmp, path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}
