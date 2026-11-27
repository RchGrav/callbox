#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/store.h"
#include "backmux/core.h"
#include "store_internal.h"

/* Record lifecycle: the two-phase creation reserve (hashed run ids),
 * teardown of loaded records, and the exit stamp (mark_run_finished),
 * including its purged-is-final semantics. */

static bool run_id_material_exists(const struct hold_store *store,
                                   const struct hold_store *avoid_public_store,
                                   const char *id) {
    char path[HOLD_PATH_MAX];
    return (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (store->console_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && hold_path_exists(path)) ||
           (store->public_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && hold_path_exists(path)) ||
           (avoid_public_store && avoid_public_store->public_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.json", avoid_public_store->public_dir, id) == 0 && hold_path_exists(path));
}

static void hash_field(struct sha256_ctx *ctx, const char *key, const char *value) {
    hold_sha256_update_nul_field(ctx, key);
    hold_sha256_update_nul_field(ctx, value ? value : "-");
}

/* The launch id material: version tag, executable, cwd, timestamp, launcher
 * pid, argv, and a collision counter. */
struct run_hash_material {
    const char *exe;
    const char *cwd;
    int argc;
    char **argv;
    int64_t start_unix_ns;
};

static void compute_run_hash(const struct run_hash_material *m, unsigned long counter,
                             char out[ID_STR_LEN]) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    char buf[64];
    hold_sha256_init(&ctx);
    hash_field(&ctx, "version", "backmux-run-v1");
    hash_field(&ctx, "exe", m->exe);
    hash_field(&ctx, "cwd", m->cwd && *m->cwd ? m->cwd : "-");
    snprintf(buf, sizeof(buf), "%" PRId64, m->start_unix_ns);
    hash_field(&ctx, "timestamp_ns", buf);
    snprintf(buf, sizeof(buf), "%ld", (long)getpid());
    hash_field(&ctx, "launcher_pid", buf);
    snprintf(buf, sizeof(buf), "%d", m->argc);
    hash_field(&ctx, "argc", buf);
    for (int i = 0; i < m->argc; i++) {
        snprintf(buf, sizeof(buf), "argv[%d]", i);
        hash_field(&ctx, buf, m->argv && m->argv[i] ? m->argv[i] : "");
    }
    snprintf(buf, sizeof(buf), "%lu", counter);
    hash_field(&ctx, "counter", buf);
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), out, ID_STR_LEN);
}

static int reserve_hashed_id(const struct hold_store *store,
                             const struct hold_store *avoid_public_store,
                             const struct run_hash_material *m,
                             char out_id[ID_STR_LEN]) {
    if (!store || !out_id) {
        errno = EINVAL;
        return -1;
    }
    char reserve[HOLD_PATH_MAX];
    for (unsigned long counter = 0; counter < 1024; counter++) {
        compute_run_hash(m, counter, out_id);
        /* A letter first (a-f): then a word that starts with a digit is never
         * an id, so it cannot be mistaken for a window number, a size or a pid,
         * and a name, an id and a pane can all be written as the shortest
         * unique prefix. Ids made before this still resolve as ids. */
        if (isdigit((unsigned char)out_id[0])) continue;
        if (!hold_valid_id(out_id) || run_id_material_exists(store, avoid_public_store, out_id)) continue;
        if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", store->record_dir, out_id) != 0) return -1;
        int fd = open(reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int hold_reserve_run_id(const struct hold_store *store,
                        const char *resolved_exec_path,
                        int argc, char **argv,
                        const char *cwd,
                        int64_t start_unix_ns,
                        char out_id[ID_STR_LEN]) {
    struct run_hash_material m = {
        .exe = resolved_exec_path, .cwd = cwd, .argc = argc, .argv = argv,
        .start_unix_ns = start_unix_ns,
    };
    return reserve_hashed_id(store, NULL, &m, out_id);
}

void hold_abort_run_reservation(const struct hold_store *store, const char *id) {
    char path[HOLD_PATH_MAX];
    if (!store || !id || !*id) return;
    if (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0) {
        unlink(path);
    }
}

int hold_claim_existing_run(const struct hold_store *store, const char *id) {
    char path[HOLD_PATH_MAX];
    if (!store || !id || !hold_valid_id(id)) {
        errno = EINVAL;
        return -1;
    }
    if (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) != 0) return -1;
    for (;;) {
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return -1;
        while (flock(fd, LOCK_EX) != 0) {
            if (errno != EINTR) {
                int e = errno;
                close(fd);
                errno = e;
                return -1;
            }
        }
        /* The holder before us committed (its record write unlinked this
         * file) or aborted: our lock is on a file no longer at the path.
         * Claim the one that is there now. */
        struct stat held, now;
        if (fstat(fd, &held) == 0 && lstat(path, &now) == 0 &&
            held.st_dev == now.st_dev && held.st_ino == now.st_ino) return fd;
        close(fd);
    }
}

void hold_release_existing_run(const struct hold_store *store, const char *id, int fd) {
    if (fd < 0) return;
    int saved = errno;
    char path[HOLD_PATH_MAX];
    struct stat held, now;
    /* Unlink only our own file: a committed launch's reserve is already gone,
     * and the path may by now be the next redial's claim. */
    if (store && id && hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 &&
        fstat(fd, &held) == 0 && lstat(path, &now) == 0 &&
        held.st_dev == now.st_dev && held.st_ino == now.st_ino) {
        unlink(path);
    }
    close(fd); /* releases the flock: a waiting redial goes on */
    errno = saved;
}

bool hold_existing_run_claimed(const struct hold_store *store, const char *id) {
    char path[HOLD_PATH_MAX];
    if (!store || !id || hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) != 0)
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    /* never waits: the redial takes the store lock we hold before it lets go */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        bool held = errno == EWOULDBLOCK;
        close(fd);
        return held;
    }
    hold_release_existing_run(store, id, fd);
    return false;
}

void hold_free_run_record(struct hold_run_record *r) {
    if (!r) return;
    hold_free_argv_alloc(r->recipe.argv, r->recipe.argc);
    hold_free_argv_alloc(r->recipe.env, r->recipe.envc);
    hold_free_argv_alloc(r->observed_argv, r->observed_argc);
    r->recipe.argv = NULL;
    r->recipe.env = NULL;
    r->observed_argv = NULL;
    r->recipe.argc = 0;
    r->recipe.envc = 0;
    r->observed_argc = 0;
}

static int wait_status_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

/* The fields an exit stamp owns: state, exit code or signal, end time. */
static void apply_exit(struct hold_run_record *r, int status) {
    snprintf(r->state, sizeof(r->state), "%s", "exited");
    r->has_state = true;
    r->exit_code = wait_status_exit_code(status);
    r->has_exit_code = true;
    r->has_term_signal = WIFSIGNALED(status);
    r->term_signal = r->has_term_signal ? WTERMSIG(status) : 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        int64_t ended_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        hold_format_rfc3339_utc_from_ns(ended_ns, r->ended_at, sizeof(r->ended_at));
        r->has_ended_at = true;
    }
}

int hold_mark_run_finished(const struct hold_store *store, const char *id, int status, pid_t pid) {
    if (!store || !hold_valid_id(id)) {
        errno = EINVAL;
        return -1;
    }
    char path[HOLD_PATH_MAX];
    struct hold_run_record r;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return -1;
    /* A purge may have removed the record between our load and this write
     * (force-purging a live process races the reaper's exit stamp); recreating
     * it here would resurrect a process the user just removed. Purge unlinks
     * under the store lock, so holding it from this re-check through the
     * rename makes "purged, stay gone" hold. Return 1 — distinct from -1 —
     * so callers can tell "purged, stay gone" (terminal) from a load failure
     * such as "record not written yet" (retryable). */
    int lock_fd = hold_store_lock(store->record_dir);
    struct stat still_there;
    if (stat(path, &still_there) != 0 && errno == ENOENT) {
        hold_store_unlock(lock_fd);
        hold_free_run_record(&r);
        return 1;
    }
    /* The exit stamp owns only state, exit code, and end time. Everything
     * else comes from the record as it is now, under the lock: a concurrent
     * save/rename may have changed it, and a program that exits at once can
     * be stamped before its launcher has written this run's record over the
     * previous run's (a redial) - stamping our earlier copy would put the old
     * run's fields back (its log start, its times). Best-effort: on a
     * transient read failure keep our loaded copy. */
    struct hold_run_record cur;
    char cur_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &cur, cur_path, sizeof(cur_path)) == 0) {
        hold_free_run_record(&r);
        r = cur;
    }
    /* Not this run's record (the previous run's, before this launcher writes
     * its own): stamping it would put that run back over this one. */
    if (pid > 0 && r.pid != pid) {
        hold_store_unlock(lock_fd);
        hold_free_run_record(&r);
        errno = EAGAIN;
        return -1;
    }
    apply_exit(&r, status);
    int argc = r.recipe.argc;
    char **argv = r.recipe.argv;
    if (argc <= 0 || !argv) {
        argc = r.observed_argc;
        argv = r.observed_argv;
    }
    char rewritten_path[HOLD_PATH_MAX] = {0};
    int rc = hold_write_record_atomic(store->record_dir, &r, argc, argv, rewritten_path, sizeof(rewritten_path));
    /* (Root rewriting a sudo user's record: hold_atomic_write_json gives the
     * new file the old one's owner before the rename.) */
    if (rc == 0 && store->kind == STORE_SYSTEM_MANAGED && store->public_dir[0]) {
        /* A finishing process has no live ports; the projection clears them. */
        rc = hold_write_public_index_atomic(store, &r, NULL);
    }
    hold_store_unlock(lock_fd);
    hold_free_run_record(&r);
    return rc;
}
