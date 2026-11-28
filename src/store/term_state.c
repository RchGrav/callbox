/* store/term_state.c - what a managed terminal is doing now.
 *
 * A record's launch (program, arguments, working directory) says how a
 * terminal started. The person then changes directory and runs programs in
 * it, so restoring the launch alone does not restore what they saved: a bash
 * launched in ~ that is running claude in ~/callbox is "bash in ~" by its
 * launch. This observes the live terminal - the program running in it now,
 * whether that is a hosting shell, its current directory, the shell's
 * foreground workload and how many children it has - and keeps the last
 * observation in the record, beside the launch, which stays as history. It is
 * configuration derived from the live process tree, never a checkpoint of
 * process memory, and it runs when a terminal is listed, inspected or saved:
 * there is no daemon to poll. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"

static int64_t now_unix_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Shells that host a person's commands when started with nothing to run. */
static const char *const hosting_shells[] = {
    "bash", "zsh", "fish", "sh", "dash", "ksh", "mksh", "yash", "ash", "tcsh", "csh", NULL,
};

/* Options of those shells that take a value: the value is not a script. */
static bool option_takes_value(const char *a) {
    return !strcmp(a, "-o") || !strcmp(a, "+o") || !strcmp(a, "-O") || !strcmp(a, "+O") ||
           !strcmp(a, "--rcfile") || !strcmp(a, "--init-file");
}

static const char *program_name(const char *argv0) {
    const char *b = strrchr(argv0, '/');
    b = b ? b + 1 : argv0;
    return *b == '-' ? b + 1 : b; /* a login shell's argv[0], -bash */
}

/* Whether this argv is a shell hosting a person's commands: a known shell
 * given no command (-c, fish's --command) and no script, which it would run
 * and exit, being that script's interpreter rather than a host. */
bool hold_argv_is_hosting_shell(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) return false;
    const char *b = program_name(argv[0]);
    bool known = false;
    for (int i = 0; hosting_shells[i]; i++) known = known || !strcmp(b, hosting_shells[i]);
    if (!known) return false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a || !strcmp(a, "--")) return false;
        if (option_takes_value(a)) {
            i++;
            continue;
        }
        if (a[0] != '-' && a[0] != '+') return false;
        if (!strncmp(a, "--command", 9)) return false;
        if (a[0] == '-' && a[1] != '-' && strchr(a + 1, 'c')) return false;
    }
    return true;
}

/* Whether r's pid is still the process the record describes: the same boot,
 * and the same start time. Observing a reused PID would record another
 * program's directory as this terminal's. */
static bool same_live_process(const struct hold_run_record *r) {
    char boot[128];
    const char *now = hold_boot_id_or_null(boot);
    if (r->has_boot && now && strcmp(r->boot_id, now) != 0) return false;
    char state = 0;
    uint64_t start = 0;
    if (r->pid <= 1 || hold_read_proc_stat_tokens(r->pid, &state, &start) != 0) return false;
    if (state == 'Z') return false;
    return !r->proc_starttime_ticks || start == r->proc_starttime_ticks;
}

/* The live members of the foreground group that the shell started itself.
 * Why the group walk and the parent, not the shell's children list and their
 * ids: both work on macOS too, where a process's group cannot be read on its
 * own (hold_proc_read_ids is Linux-only). */
struct fg_members {
    pid_t shell, pgid;
    int n;          /* the shell's live children in the foreground group */
    pid_t first;    /* one of them, other than the group's leader */
};

static int count_fg_member(pid_t pid, void *ctx) {
    struct fg_members *c = ctx;
    pid_t ppid = 0;
    char name[16];
    if (hold_proc_parent_and_name(pid, &ppid, name, sizeof(name)) == 0 && ppid == c->shell) {
        c->n++;
        if (pid != c->pgid && !c->first) c->first = pid;
    }
    return 0;
}

static int count_live_child(pid_t child, void *ctx) {
    if (hold_leader_present(child)) (*(int *)ctx)++;
    return 0;
}

/* A process's argv (whole) and directory into t's workload fields. */
static int read_workload(pid_t pid, struct hold_term_state *t) {
#ifdef HOLD_TESTING
    /* The suite cannot make a workload unreadable without another user
     * (/proc/<pid>/cwd needs ptrace access), so a test build fails it here. */
    if (getenv("HOLD_TEST_FOREGROUND_UNREADABLE")) return -1;
#endif
    if (hold_proc_argv(pid, &t->workload_argv, &t->workload_argc) != 0) return -1;
    if (hold_proc_cwd(pid, t->workload_cwd, sizeof(t->workload_cwd)) != 0) {
        hold_free_argv_alloc(t->workload_argv, t->workload_argc);
        t->workload_argv = NULL;
        t->workload_argc = 0;
        return -1;
    }
    t->workload_pid = pid;
    return 0;
}

void hold_free_term_state(struct hold_term_state *t) {
    if (!t) return;
    hold_free_argv_alloc(t->argv, t->argc);
    hold_free_argv_alloc(t->workload_argv, t->workload_argc);
    t->argv = t->workload_argv = NULL;
    t->argc = t->workload_argc = 0;
}

int hold_observe_terminal(const struct hold_run_record *r, struct hold_term_state *t) {
    memset(t, 0, sizeof(*t));
    if (!r->recipe.mode_tty || !same_live_process(r)) return -1;
    /* The program running now, read from the live process: an exec in place
     * (exec zsh, exec claude) keeps the pid and start time but not the
     * program, and the launch is only history. */
    if (hold_proc_argv(r->pid, &t->argv, &t->argc) != 0 || hold_proc_cwd(r->pid, t->cwd, sizeof(t->cwd)) != 0) {
        hold_free_term_state(t);
        return -1;
    }
    t->observed_unix_ns = now_unix_ns();
    /* backmux shell hosts whatever shell it started (one this list does not
     * know too), while that shell is still the program running. */
    t->hosting_shell = hold_argv_is_hosting_shell(t->argc, t->argv) ||
                       (r->recipe.mode_shell && r->recipe.argc > 0 && r->recipe.argv &&
                        !strcmp(program_name(t->argv[0]), program_name(r->recipe.argv[0])));
    (void)hold_proc_for_each_child(r->pid, count_live_child, &t->children);
    if (!t->hosting_shell) return 0; /* a program launched directly: its argv and directory are the state */
    pid_t fg = 0;
    if (hold_proc_foreground_group(r->pid, &fg) != 0 || fg <= 0) {
        snprintf(t->foreground, sizeof(t->foreground), "unavailable");
        t->foreground_unread = true;
        return 0;
    }
    if (fg == r->pgid) {
        snprintf(t->foreground, sizeof(t->foreground), "idle");
        return 0;
    }
    /* The foreground job is its group leader. More than one of the shell's
     * children in the group is a pipeline, which one argv cannot rebuild;
     * so is a group whose leader has gone while others run on (ls | less
     * once ls is done): the facts are kept from a member still running. */
    struct fg_members c = {r->pid, fg, 0, 0};
    bool denied = false;
    (void)hold_for_each_group_process(fg, r->sid, count_fg_member, &c, &denied);
    const char *kind = c.n > 1 ? "pipeline" : "workload";
    if (read_workload(fg, t) != 0) {
        if (!c.first || read_workload(c.first, t) != 0) {
            snprintf(t->foreground, sizeof(t->foreground), "unavailable");
            t->foreground_unread = true;
            return 0;
        }
        kind = "pipeline";
    }
    snprintf(t->foreground, sizeof(t->foreground), "%s", kind);
    return 0;
}

static bool same_argv(int ac, char **av, int bc, char **bv) {
    if (ac != bc) return false;
    for (int i = 0; i < ac; i++)
        if (strcmp(av[i] ? av[i] : "", bv[i] ? bv[i] : "") != 0) return false;
    return true;
}

/* Whether two observations say the same thing (the time aside). */
static bool same_state(const struct hold_term_state *a, const struct hold_term_state *b) {
    return a->hosting_shell == b->hosting_shell && !strcmp(a->cwd, b->cwd) && a->children == b->children &&
           same_argv(a->argc, a->argv, b->argc, b->argv) && !strcmp(a->foreground, b->foreground) &&
           a->foreground_unread == b->foreground_unread && a->workload_pid == b->workload_pid &&
           !strcmp(a->workload_cwd, b->workload_cwd) &&
           same_argv(a->workload_argc, a->workload_argv, b->workload_argc, b->workload_argv);
}

/* A look that could not read the foreground is not a look at an idle shell:
 * the last foreground that was read (and its workload) stays, marked unread,
 * so what restores the terminal is never thrown away by a failed read. An
 * idle shell that was read does clear it. */
static void keep_last_foreground(struct hold_term_state *now, struct hold_term_state *prev) {
    if (!now->foreground_unread || !now->hosting_shell || !prev->hosting_shell || !prev->foreground[0] ||
        !strcmp(prev->foreground, "unavailable"))
        return;
    snprintf(now->foreground, sizeof(now->foreground), "%s", prev->foreground);
    now->workload_pid = prev->workload_pid;
    snprintf(now->workload_cwd, sizeof(now->workload_cwd), "%s", prev->workload_cwd);
    now->workload_argc = prev->workload_argc;
    now->workload_argv = prev->workload_argv; /* moved: prev no longer owns it */
    prev->workload_argc = 0;
    prev->workload_argv = NULL;
}

static int write_record(const char *record_dir, const struct hold_run_record *r) {
#ifdef HOLD_TESTING
    /* The suite cannot make the store refuse a write (backmux resets its
     * directory's mode when it starts), so a test build fails it here. */
    if (getenv("HOLD_TEST_TERM_WRITE_FAILS")) {
        errno = EIO;
        return -1;
    }
#endif
    char out[HOLD_PATH_MAX];
    return hold_write_record_atomic(record_dir, r, r->recipe.argc, r->recipe.argv, out, sizeof(out));
}

int hold_refresh_terminal(const char *record_dir, const char *id) {
    /* Only the store's owner rewrites its records: root under sudo looking at
     * a user's store must not leave root-owned files there, and a user cannot
     * write the root-managed store. */
    struct stat sd;
    if (stat(record_dir, &sd) != 0 || sd.st_uid != geteuid()) return 0;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(record_dir, id, &r, path, sizeof(path)) != 0) return 0;
    struct hold_term_state now;
    bool observed = hold_observe_terminal(&r, &now) == 0;
    pid_t pid = r.pid;
    uint64_t start = r.proc_starttime_ticks;
    if (observed && r.has_term) keep_last_foreground(&now, &r.term);
    /* Unchanged since the last observation: no rewrite. Why not refresh the
     * time anyway: every rewrite is an fsync, and list looks at every
     * running terminal each time it runs. */
    bool unchanged = !observed || (r.has_term && same_state(&r.term, &now));
    hold_free_run_record(&r);
    if (unchanged) {
        if (observed) hold_free_term_state(&now);
        return 0;
    }
    /* Re-read under the store lock: an exit stamp, a save or a rename written
     * since the look above must survive this rewrite. */
    int lock_fd = hold_store_lock(record_dir);
    int rc = 0;
    if (hold_load_record_by_id(record_dir, id, &r, path, sizeof(path)) == 0) {
        /* The run observed is still the record's run, and its exit has not
         * been stamped meanwhile (an ended terminal keeps its last
         * observation). */
        bool same_run = r.pid == pid && r.proc_starttime_ticks == start &&
                        !(r.has_state && (!strcmp(r.state, "exited") || !strcmp(r.state, "failed")));
        if (same_run) {
            hold_free_term_state(&r.term);
            r.term = now;
            r.has_term = true;
            rc = write_record(record_dir, &r) == 0 ? 0 : -1;
            int err = errno;
            hold_free_run_record(&r); /* frees now's argv with it */
            errno = err;
        } else {
            hold_free_run_record(&r);
            hold_free_term_state(&now);
        }
    } else {
        hold_free_term_state(&now);
    }
    hold_store_unlock(lock_fd);
    return rc;
}
