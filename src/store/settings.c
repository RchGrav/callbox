#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/store.h"
#include "backmux/core.h"

/* Per-user settings: "<home>/.config/backmux/config", one "name value" line per
 * setting. Plain text a person can read and edit; lines this build does not
 * know (comments, a newer backmux's settings) are kept as they are. Rewrites are
 * atomic (temp file + rename), so a reader never sees half a file and a
 * refused value never touches what was saved. */

/* The file's real location. A person's ~/.config, the backmux directory in it,
 * or the file itself may be a symlink (dotfile managers make all three), and
 * $HOME may be reached through one: the deepest part of the path that exists
 * is resolved, and what does not exist yet is created beneath it. */
int hold_settings_path(const char *home, char *out, size_t n) {
    static const char *const tails[] = {"/.config/backmux/config", "/.config/backmux", "/.config", ""};
    char logical[HOLD_PATH_MAX], real[HOLD_PATH_MAX];
    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        if (hold_checked_snprintf(logical, sizeof(logical), "%s%s", home, tails[i]) != 0) return -1;
        if (realpath(logical, real)) return hold_checked_snprintf(out, n, "%s%s", real, tails[0] + strlen(tails[i]));
    }
    return hold_checked_snprintf(out, n, "%s%s", home, tails[0]);
}

/* The line's name, when it is a setting line: [name, name+len). */
static bool line_name(const char *line, const char *end, const char **name, size_t *len) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (line == end || *line == '#') return false;
    const char *p = line;
    while (p < end && *p != ' ' && *p != '\t') p++;
    *name = line;
    *len = (size_t)(p - line);
    return true;
}

/* 0 with the file's text (free it), 1 when there is no file, -1 on error. */
static int read_settings(const char *path, char **text) {
    *text = NULL;
    if (hold_read_owned_file_no_symlink(path, text) == 0) return 0;
    return errno == ENOENT || errno == ENOTDIR ? 1 : -1;
}

int hold_settings_get(const char *home, const char *name, char *value, size_t n) {
    char path[HOLD_PATH_MAX], *text = NULL;
    if (hold_settings_path(home, path, sizeof(path)) != 0) return -1;
    int rc = read_settings(path, &text);
    if (rc != 0) return rc;
    size_t want = strlen(name);
    rc = 1;
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *nm;
        size_t len;
        if (line_name(line, end, &nm, &len) && len == want && !memcmp(nm, name, want)) {
            const char *v = nm + len, *ve = end;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
            if ((size_t)(ve - v) >= n) {
                free(text);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(value, v, (size_t)(ve - v));
            value[ve - v] = '\0';
            rc = 0; /* the last line for a name wins, as a later edit would */
        }
        line = *end ? end + 1 : end;
    }
    free(text);
    return rc;
}

int hold_settings_put(const char *home, const char *name, const char *value) {
    char path[HOLD_PATH_MAX], dir[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX];
    if (hold_settings_path(home, path, sizeof(path)) != 0) return -1;
    char *slash = strrchr(path, '/');
    if (!slash || slash == path || hold_checked_snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path) != 0) {
        errno = EINVAL;
        return -1;
    }
    char *old = NULL;
    int had = read_settings(path, &old);
    if (had < 0) return -1;
    if (had == 1 && !value) return 0; /* nothing saved, nothing to clear */

    /* Every line but this setting's, then the new value. */
    size_t want = strlen(name), cap = (old ? strlen(old) : 0) + want + (value ? strlen(value) : 0) + 4, len = 0;
    char *next = malloc(cap);
    if (!next) {
        free(old);
        return -1;
    }
    for (const char *line = old ? old : ""; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *nm;
        size_t nl;
        if (!(line_name(line, end, &nm, &nl) && nl == want && !memcmp(nm, name, want))) {
            memcpy(next + len, line, (size_t)(end - line));
            len += (size_t)(end - line);
            next[len++] = '\n';
        }
        line = *end ? end + 1 : end;
    }
    if (value) len += (size_t)snprintf(next + len, cap - len, "%s %s\n", name, value);
    free(old);

    int rc = -1;
    if (len == 0) {
        rc = unlink(path) == 0 || errno == ENOENT ? 0 : -1;
        goto out;
    }
    if (hold_mkdir_p_mode(dir, 0700) != 0) goto out;
    int fd = hold_open_unique_temp(dir, "config", 0600, tmp, sizeof(tmp));
    if (fd < 0) goto out;
    /* fsync before the rename, as the store's writes do: a crash must leave
     * the old settings or the new ones, never an empty file. */
    bool ok = hold_write_all(fd, next, len) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) rc = 0;
    else {
        int saved = errno;
        unlink(tmp);
        errno = saved;
    }
out:
    free(next);
    return rc;
}
