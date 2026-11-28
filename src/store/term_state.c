/* store/term_state.c - what a managed terminal is doing now.
 *
 * A record's launch (program, arguments, working directory) says how a
 * terminal started. The person then changes directory and runs programs in
 * it, so restoring the launch alone does not restore what they saved: a bash
 * launched in ~ that is running claude in ~/callbox is "bash in ~" by its
 * launch. This observes the live terminal - whether its program is a hosting
 * shell, its current directory, and the shell's foreground workload - and
 * keeps the last observation in the record, beside the launch, which stays
 * as history. It is configuration derived from the live process tree, never
 * a checkpoint of process memory, and it runs when a terminal is listed,
 * inspected or saved: there is no daemon to poll. */
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

bool hold_record_is_hosting_shell(const struct hold_run_record *r) {
    if (r->recipe.mode_shell) return true;
    if (r->recipe.argc < 1 || !r->recipe.argv || !r->recipe.argv[0]) return false;
    const char *b = strrchr(r->recipe.argv[0], '/');
    b = b ? b + 1 : r->recipe.argv[0];
    if (*b == '-') b++; /* a login shell's argv[0], -bash */
    bool known = false;
    for (int i = 0; hosting_shells[i]; i++) known = known || !strcmp(b, hosting_shells[i]);
    if (!known) return false;
    /* A shell given a command (-c, fish's --command) or a script runs that
     * and exits: it is the script's interpreter, not a host. */
    for (int i = 1; i < r->recipe.argc; i++) {
        const char *a = r->recipe.argv[i];
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

static int split_cmdline(const char *buf, size_t len, char ***argv_out, int *argc_out);

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

/* A process's argv and directory into t's workload fields. */
static int read_workload(pid_t pid, struct hold_term_state *t) {
    char buf[8192];
    size_t len = 0;
    if (hold_proc_cmdline(pid, buf, sizeof(buf), &len) != 0 || len == 0 ||
        split_cmdline(buf, len, &t->workload_argv, &t->workload_argc) != 0)
        return -1;
    if (hold_proc_cwd(pid, t->workload_cwd, sizeof(t->workload_cwd)) != 0) {
        hold_free_term_state(t);
        return -1;
    }
    t->workload_pid = pid;
    return 0;
}

/* The kernel's NUL-separated argument vector as an allocated argv. */
static int split_cmdline(const char *buf, size_t len, char ***argv_out, int *argc_out) {
    int n = 0;
    for (size_t i = 0; i < len; i++) n += buf[i] == '\0';
    if (len > 0 && buf[len - 1] != '\0') n++;
    if (n == 0) return -1;
    char **argv = calloc((size_t)n + 1, sizeof(char *));
    if (!argv) return -1;
    int k = 0;
    for (size_t i = 0; i < len && k < n;) {
        size_t l = strnlen(buf + i, len - i);
        argv[k] = strndup(buf + i, l);
        if (!argv[k]) {
            hold_free_argv_alloc(argv, k);
            return -1;
        }
        k++;
        i += l + 1;
    }
    *argv_out = argv;
    *argc_out = k;
    return 0;
}

void hold_free_term_state(struct hold_term_state *t) {
    if (!t) return;
    hold_free_argv_alloc(t->workload_argv, t->workload_argc);
    t->workload_argv = NULL;
    t->workload_argc = 0;
}

int hold_observe_terminal(const struct hold_run_record *r, struct hold_term_state *t) {
    memset(t, 0, sizeof(*t));
    if (!r->recipe.mode_tty || !same_live_process(r)) return -1;
    if (hold_proc_cwd(r->pid, t->cwd, sizeof(t->cwd)) != 0) return -1;
    t->observed_unix_ns = now_unix_ns();
    t->hosting_shell = hold_record_is_hosting_shell(r);
    if (!t->hosting_shell) return 0; /* a program launched directly: its directory is the state */
    pid_t fg = 0;
    if (hold_proc_foreground_group(r->pid, &fg) != 0 || fg <= 0) {
        snprintf(t->foreground, sizeof(t->foreground), "unavailable");
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
    return a->hosting_shell == b->hosting_shell && !strcmp(a->cwd, b->cwd) &&
           !strcmp(a->foreground, b->foreground) && a->workload_pid == b->workload_pid &&
           !strcmp(a->workload_cwd, b->workload_cwd) &&
           same_argv(a->workload_argc, a->workload_argv, b->workload_argc, b->workload_argv);
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
            char out[HOLD_PATH_MAX];
            rc = hold_write_record_atomic(record_dir, &r, r.recipe.argc, r.recipe.argv, out, sizeof(out)) == 0 ? 0 : -1;
            hold_free_run_record(&r); /* frees now's argv with it */
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
