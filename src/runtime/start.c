#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/runtime.h"
#include "callbox/runtime_internal.h"
#include "callbox/core.h"
#include "callbox/platform.h"
#include "callbox/store.h"
#include "callbox/term.h"
#include "callbox/console.h"
#include "callbox/access.h"

/* runtime/start: launch orchestration — hashed-id reservation (via the store
 * reserve API), name generation, recipe capture, transactional record+index
 * write, redial reuse, restart supervision, docker-run exit parity. */

static volatile sig_atomic_t g_restart_stop = 0;
/* Decided once by the launcher, before it forks, and inherited: whether this
 * -d call's supervisor (waiter or restart supervisor) holds the lifetime log
 * lock - only under the fallback. The record says the same (liveness_lock). */
static bool g_liveness_lock = false;
/* Plain -d under the fallback: no supervisor, so the program's own output
 * descriptors carry the lock (lock held = it runs; gone proves nothing). */
static bool g_output_lock = false;


static void free_launch_and_observed_argv(char **launch_argv, char **observed_argv, int argc) {
    hold_free_argv_alloc(launch_argv, argc);
    hold_free_argv_alloc(observed_argv, argc);
}

static int enter_privileged_exec_cwd(void) {
    struct stat st;
    if (lstat("/", &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        errno = EPERM;
        return -1;
    }
    return chdir("/");
}

static void kill_supervisor_if_distinct(pid_t supervisor_pid, pid_t target_pid) {
    if (supervisor_pid > 1 && supervisor_pid != target_pid) {
        kill(supervisor_pid, SIGKILL);
        int st = 0;
        while (waitpid(supervisor_pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

/* ---- child environment ---- */

extern char **environ;

static int env_entry_key(const char *entry, char key[256]) {
    const char *eq = entry ? strchr(entry, '=') : NULL;
    if (!entry || !*entry || !eq || eq == entry || (size_t)(eq - entry) >= 256) {
        errno = EINVAL;
        return -1;
    }
    memcpy(key, entry, (size_t)(eq - entry));
    key[eq - entry] = '\0';
    return 0;
}

static int clear_process_environment(void) {
#if defined(__GLIBC__)
    return clearenv() == 0 ? 0 : -1;
#else
    char key[256];
    while (environ && environ[0]) {
        if (env_entry_key(environ[0], key) != 0) return -1;
        if (unsetenv(key) != 0) return -1;
    }
    return 0;
#endif
}

/* The call's own identity, exported to the held program: CALLBOX_ID and
 * CALLBOX_NAME are its address (callbox send/peek/wait accept either), and
 * CALLBOX_PARENT names the call that launched it, when it was launched
 * from inside one (an agent starting a worker), so the worker can reply.
 * Captured in the launcher before fork; always overrides inherited values. */
static struct {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char parent[ALIAS_MAX_LEN + 1];
    bool set;
    bool tty;
} g_child_identity;

static void child_identity_prepare(const char *id, const char *name, bool tty) {
    memset(&g_child_identity, 0, sizeof(g_child_identity));
    snprintf(g_child_identity.id, sizeof(g_child_identity.id), "%s", id ? id : "");
    snprintf(g_child_identity.name, sizeof(g_child_identity.name), "%s", name ? name : "");
    const char *parent = getenv("CALLBOX_NAME");
    if (!parent || !*parent) parent = getenv("CALLBOX_ID");
    if (parent && strlen(parent) < sizeof(g_child_identity.parent) && strcmp(parent, g_child_identity.name) != 0)
        snprintf(g_child_identity.parent, sizeof(g_child_identity.parent), "%s", parent);
    g_child_identity.set = g_child_identity.id[0] != '\0';
    g_child_identity.tty = tty;
}

/* A TTY call runs on Callbox's own xterm-compatible terminal, never on the
 * launcher's, so its environment describes that terminal: TERM is
 * xterm-256color (as docker run -t sets TERM for its terminal), and the
 * variables naming the launcher's terminal or multiplexer go. A program that
 * sees TMUX believes it runs inside that tmux and acts on it - hollywood
 * opened its panes in the caller's byobu, leaving callbox's screen blank.
 * Applied before the recipe env, so an explicit -e wins. Plain calls are
 * left alone. */
static int apply_default_term(void) {
    if (!g_child_identity.tty) return 0;
    static const char *const launcher_terminal[] = {
        "TMUX", "TMUX_PANE",           /* tmux (and byobu on tmux) */
        "STY", "WINDOW",               /* GNU screen */
        "TERM_PROGRAM", "TERM_PROGRAM_VERSION",
    };
    for (size_t i = 0; i < sizeof(launcher_terminal) / sizeof(launcher_terminal[0]); i++)
        if (unsetenv(launcher_terminal[i]) != 0) return -1;
    return setenv("TERM", "xterm-256color", 1);
}

static int apply_child_identity(void) {
    if (!g_child_identity.set) return 0;
    if (setenv("CALLBOX_ID", g_child_identity.id, 1) != 0) return -1;
    if (g_child_identity.name[0] ? setenv("CALLBOX_NAME", g_child_identity.name, 1) != 0
                                 : unsetenv("CALLBOX_NAME") != 0)
        return -1;
    if (g_child_identity.parent[0] ? setenv("CALLBOX_PARENT", g_child_identity.parent, 1) != 0
                                   : unsetenv("CALLBOX_PARENT") != 0)
        return -1;
    return 0;
}

static int apply_child_env(int envc, char **env, bool clean_base) {
    if (clean_base) {
        if (clear_process_environment() != 0) return -1;
        if (setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0) return -1;
    }
    if (apply_default_term() != 0) return -1;
    for (int i = 0; i < envc; i++) {
        const char *entry = env ? env[i] : NULL;
        char key[256];
        if (env_entry_key(entry, key) != 0) return -1;
        if (setenv(key, strchr(entry, '=') + 1, 1) != 0) return -1;
    }
    return apply_child_identity();
}

/* ---- the child-side handshake and session preamble ---- */

/* THE errno-write handshake tail: any pre-exec failure rides the handshake
 * pipe as an errno, then _exit(127) — never a return into the caller. */
static _Noreturn void child_fail_errno(int handshake_fd) {
    int e = errno;
    (void)hold_write_all(handshake_fd, &e, sizeof(e));
    _exit(127);
}

/* The program's stdout and stderr go straight into the log (the spec's direct
 * redirection; no copying process in between). */
static void redirect_output_to_log(const char *log_path, int handshake_fd, bool lock) {
    int fd = hold_open_append_no_symlink(log_path);
    if (fd < 0) child_fail_errno(handshake_fd);
    if (lock) (void)flock(fd, LOCK_SH | LOCK_NB);
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) child_fail_errno(handshake_fd);
    if (fd > STDERR_FILENO) close(fd);
}

/* THE spawn preamble, shared by the direct target and the restart
 * supervisor: new session, optionally pinned privileged cwd, child env,
 * optionally /dev/null stdin. */
static void child_session_setup(int handshake_fd,
                                bool pin_privileged_cwd,
                                int envc, char **env, bool clean_base,
                                bool null_stdin) {
    if (setsid() < 0) child_fail_errno(handshake_fd);
    /* A program starts with default Ctrl-C and Ctrl-\ handling, as a
     * container does: a shell starts background jobs with SIGINT and SIGQUIT
     * ignored, and an ignored-at-start signal cannot even be trapped - Ctrl-C
     * passed on to the program would do nothing. */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    if (pin_privileged_cwd && enter_privileged_exec_cwd() != 0) child_fail_errno(handshake_fd);
    if (apply_child_env(envc, env, clean_base) != 0) child_fail_errno(handshake_fd);
    if (null_stdin) {
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) child_fail_errno(handshake_fd);
        if (nullfd > STDERR_FILENO) close(nullfd);
    }
}

/* ---- restart policy ---- */

static void handle_restart_signal(int signo) {
    (void)signo;
    g_restart_stop = 1;
}

static bool restart_policy_is_enabled(const char *policy) {
    return policy && *policy && strcmp(policy, "no") != 0;
}

static bool restart_status_failed(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status) != 0;
    return WIFSIGNALED(status);
}

static bool restart_should_run_again(const char *policy, int status, int failures) {
    if (!policy || !*policy || strcmp(policy, "no") == 0) return false;
    if (strcmp(policy, "always") == 0 || strcmp(policy, "unless-stopped") == 0) return true;
    if (!strncmp(policy, "on-failure", 10) && (policy[10] == '\0' || policy[10] == ':')) {
        if (!restart_status_failed(status)) return false;
        if (policy[10] == ':' && policy[11]) {
            /* on-failure:N caps retries; an unparsable N means no cap. */
            char *end = NULL;
            long max_retries = strtol(policy + 11, &end, 10);
            if (end && !*end && max_retries >= 0 && max_retries <= INT_MAX && failures > max_retries) return false;
        }
        return true;
    }
    return false;
}

static void sleep_restart_delay(int seconds) {
    if (seconds <= 0) return;
    struct timespec sl = {.tv_sec = seconds, .tv_nsec = 0};
    while (!g_restart_stop && nanosleep(&sl, &sl) != 0 && errno == EINTR) {
        continue;
    }
}

/* A program that could not be started exits as a shell's would: 127 when
 * there is no such command, 126 when it is there but cannot be run (not
 * executable, not a program). Why: scripts, CI runners and docker run
 * already read those codes that way; 1 told them nothing. */
static int cannot_start(const char *name, int err) {
    if (err == ENOENT) {
        fprintf(stderr, "callbox: cannot start '%s': command not found\n", name);
        return 127;
    }
    fprintf(stderr, "callbox: cannot start '%s': %s\n", name, strerror(err));
    return (err == EACCES || err == ENOEXEC || err == EISDIR || err == EPERM) ? 126 : 1;
}

/* Exit persistence: rc 1 (record purged after load) is FINAL — never
 * resurrect a call the user just removed. Only rc -1 retries (covers a
 * fast exit before the parent has written the record). */
static void mark_finished_with_retry(const struct hold_store *store, const char *id, int status, pid_t pid) {
    for (int i = 0; i < 50; i++) {
        int rc = hold_mark_run_finished(store, id, status, pid);
        if (rc == 0 || rc == 1) break;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
}

static void run_restart_supervisor(const struct hold_store *store,
                                   const char *id,
                                   int handshake_fd,
                                   bool interactive_stdin,
                                   int envc,
                                   char **env,
                                   bool clean_child_env,
                                   const char *resolved_exec_path,
                                   char **launch_argv,
                                   const char *restart_policy,
                                   int restart_delay_seconds,
                                   bool pin_privileged_cwd) {
    child_session_setup(handshake_fd, pin_privileged_cwd, envc, env, clean_child_env, !interactive_stdin);
    char log_path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
        errno = ENAMETOOLONG;
        child_fail_errno(handshake_fd);
    }
    redirect_output_to_log(log_path, handshake_fd, false);
    struct sigaction sa = {0};
    sa.sa_handler = handle_restart_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    int failures = 0;
    int status = 0;
    bool have_status = false;
    int backoff = HOLD_RESTART_BACKOFF_FIRST_S;
    while (!g_restart_stop) {
        struct timespec run_start;
        clock_gettime(CLOCK_MONOTONIC, &run_start);
        /* The first worker's exec is the launch: its errno rides the launch
         * handshake, so a program that cannot start is reported (126/127)
         * and rolled back exactly as without --restart, instead of a call
         * id for a program that never ran. Later workers' failures are runs
         * of the call and go to its log. */
        bool first = handshake_fd >= 0;
        int exec_pipe[2] = {-1, -1};
        if (first && hold_cloexec_pipe(exec_pipe) != 0) child_fail_errno(handshake_fd);
        pid_t worker = fork();
        if (worker < 0) {
            if (first) child_fail_errno(handshake_fd);
            fprintf(stderr, "callbox: restart supervisor failed to fork: %s\n", strerror(errno));
            mark_finished_with_retry(store, id, 127 << 8, getpid()); /* the record names the supervisor */
            _exit(127);
        }
        if (worker == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            execv(resolved_exec_path, launch_argv);
            if (first) child_fail_errno(exec_pipe[1]);
            fprintf(stderr, "callbox: cannot restart '%s': %s\n", resolved_exec_path, strerror(errno));
            _exit(127);
        }
        if (first) {
            close(exec_pipe[1]);
            int worker_errno = 0;
            int hs = hold_read_exec_handshake(exec_pipe[0], &worker_errno);
            close(exec_pipe[0]);
            if (hs != 0) {
                /* hs < 0: the worker's fate is unknown - do not leave it */
                if (hs < 0) kill(worker, SIGKILL);
                while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) continue;
                errno = hs > 0 && worker_errno ? worker_errno : EIO;
                child_fail_errno(handshake_fd); /* the launcher rolls the call back */
            }
            close(handshake_fd);
            handshake_fd = -1;
            /* Under the fallback, a passive lifetime lock on the log tells
             * shells in other PID namespaces the call is alive; nothing else
             * changes. */
            if (g_liveness_lock) hold_supervisor_lock_log(log_path);
        }
        while (waitpid(worker, &status, 0) < 0) {
            if (errno == EINTR) {
                if (g_restart_stop) kill(worker, SIGTERM);
                continue;
            }
            status = 127 << 8;
            break;
        }
        have_status = true;
        if (g_restart_stop) break;
        if (restart_status_failed(status)) failures++;
        else failures = 0;
        if (!restart_should_run_again(restart_policy, status, failures)) break;
        /* status is a wait status: say what the program did */
        if (WIFSIGNALED(status))
            fprintf(stderr, "callbox: restarting %s after signal %d\n", resolved_exec_path, WTERMSIG(status));
        else
            fprintf(stderr, "callbox: restarting %s after exit status %d\n", resolved_exec_path,
                    WIFEXITED(status) ? WEXITSTATUS(status) : status);
        if (restart_delay_seconds != HOLD_RESTART_BACKOFF) {
            sleep_restart_delay(restart_delay_seconds);
        } else {
            /* (see HOLD_RESTART_BACKOFF) a run long enough to count as
             * working starts the backoff over */
            struct timespec run_end;
            clock_gettime(CLOCK_MONOTONIC, &run_end);
            if (run_end.tv_sec - run_start.tv_sec >= HOLD_RESTART_BACKOFF_RESET_S) backoff = HOLD_RESTART_BACKOFF_FIRST_S;
            sleep_restart_delay(backoff);
            backoff = backoff * 2 > HOLD_RESTART_BACKOFF_MAX_S ? HOLD_RESTART_BACKOFF_MAX_S : backoff * 2;
        }
    }
    if (have_status) mark_finished_with_retry(store, id, status, getpid());
    _exit(0);
}

/* fork() for a helper that must outlive the caller's process group: the
 * child moves to its own session before the parent returns, so an agent
 * harness that kills the caller's group the moment the command finishes
 * cannot take it along (a plain fork leaves a gap before the child's setsid). */
static pid_t fork_detached(void) {
    int sync[2];
    if (pipe(sync) != 0) sync[0] = sync[1] = -1;
    pid_t pid = fork();
    if (pid != 0) {
        if (sync[1] >= 0) close(sync[1]);
        if (pid > 0 && sync[0] >= 0) {
            char c;
            while (read(sync[0], &c, 1) < 0 && errno == EINTR) continue;
        }
        if (sync[0] >= 0) close(sync[0]);
        return pid;
    }
    (void)setsid();
    if (sync[0] >= 0) close(sync[0]);
    if (sync[1] >= 0) close(sync[1]); /* EOF: detached */
    return 0;
}

/* docker run parity: a foreground launch exits with the held process's exit
 * status once it ends. An interrupted tail leaves the call held: exit 0. */
static int foreground_exit_status(const struct hold_store *store, const char *id) {
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    for (int i = 0; i < 50; i++) {
        struct hold_run_record r = {0};
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) break;
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        bool marked = r.has_exit_code;
        int code = r.exit_code;
        hold_free_run_record(&r);
        /* The stamp first: an exited program whose background child still
         * holds its process group has ended, with its own exit code. */
        if (marked) return code;
        if (st == STATE_RUNNING) return 0;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
    fprintf(stderr, "callbox: exit status unknown for call %.12s\n", id);
    return 1;
}

static void spawn_auto_remove_watcher(const struct hold_store *store, const struct hold_run_record *record) {
    if (!store || !record || !hold_valid_id(record->id)) {
        return;
    }
    pid_t watcher = fork_detached();
    if (watcher != 0) {
        return;
    }
    hold_close_stdio_to_devnull();
    struct hold_store watch_store = *store;
    struct hold_run_record watch_record = *record;
    for (;;) {
        char boot[128];
        const char *boot_id = hold_boot_id_or_null(boot);
        enum run_state st = hold_eval_state(&watch_record, boot_id);
        if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) {
            /* The process can be gone a moment before its reaper stamps the
             * exit code; settle waits for the stamp so the exit note left
             * for `callbox wait` carries it. */
            hold_auto_remove_settle(&watch_store, watch_record.id, 2000);
            _exit(0);
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 200 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
}

/* ---- sudo-home store routing ----
 * A root or sudo start of something under the invoking user's home goes into
 * that user's store: the user started it, and people keep track of what they
 * start in their own store. */

bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv) {
    /* Only an explicit start target (bare `callbox <cmd>` or `callbox start <cmd>`)
     * is inspected; other owned verbs never route stores by argv. */
    if (argc <= 0 || !argv || !argv[0] || !*argv[0] ||
        (owned && !(command && strcmp(command, "start") == 0))) {
        return false;
    }
    const char *target = argv[0];

    const char *home = NULL;
    if (inv && inv->euid_root && inv->have_sudo_user && inv->invoking_home[0]) {
        home = inv->invoking_home;
    } else {
        home = getenv("HOME");
    }
    if (!home || !*home) {
        return false;
    }

    char resolved[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(target, resolved, sizeof(resolved)) != 0) {
        return false;
    }
    if (hold_path_is_within_dir(resolved, home)) {
        return true;
    }

    char cwd[HOLD_PATH_MAX] = {0};
    if (!getcwd(cwd, sizeof(cwd))) {
        cwd[0] = '\0';
    }
    for (int i = 1; i < argc; i++) {
        const char *arg = argv ? argv[i] : NULL;
        if (!arg || !*arg) {
            continue;
        }
        if (arg[0] == '-') {
            const char *eq = strchr(arg, '=');
            if (!eq || !eq[1]) {
                continue;
            }
            arg = eq + 1;
        }
        char path[HOLD_PATH_MAX];
        if (hold_resolve_existing_path_from_cwd(arg, cwd[0] ? cwd : NULL, path, sizeof(path)) == 0 &&
            hold_path_is_within_dir(path, home)) {
            return true;
        }
    }
    return false;
}

/* ---- the one launch unwind ---- */

/* Every launch failure rolls back through here: close what is open, kill and
 * reap the spawned group, then remove creation material (record, reserve,
 * owned log, console socket) and free the argv copies. Preserves errno. */
struct launch_undo {
    const struct hold_store *store;
    const char *id;           /* reserve to abort once reserved */
    bool reserved;
    int claim_fd;             /* a redial's claim on the call (-1: none) */
    const char *log_path;
    bool unlink_log;          /* owns_new_log, once launch material exists */
    const char *console_sock; /* unlinked when nonempty */
    const char *record_path;  /* unlinked when nonempty */
    int *fds;                 /* fd slots to close; -1 = already closed */
    int nfds;
    pid_t reap_pid;           /* rollback: pid to waitpid */
    pid_t group;              /* rollback: SIGKILL -group when > 0 */
    pid_t supervisor;
    char **launch_argv;
    char **observed_argv;
    int argc;
};

static void launch_undo(struct launch_undo *u) {
    int saved = errno;
    for (int i = 0; i < u->nfds; i++) {
        if (u->fds[i] >= 0) {
            close(u->fds[i]);
            u->fds[i] = -1;
        }
    }
    if (u->group > 0) {
        hold_rollback_spawned_group(u->reap_pid, u->group);
        kill_supervisor_if_distinct(u->supervisor, u->group);
    }
    if (u->record_path && u->record_path[0]) unlink(u->record_path);
    if (u->reserved) hold_abort_run_reservation(u->store, u->id);
    /* after the rollback above, so a waiting redial finds this launch gone */
    hold_release_existing_run(u->store, u->id, u->claim_fd);
    u->claim_fd = -1;
    if (u->unlink_log && u->log_path[0]) unlink(u->log_path);
    if (u->console_sock[0]) unlink(u->console_sock);
    free_launch_and_observed_argv(u->launch_argv, u->observed_argv, u->argc);
    u->launch_argv = NULL;
    u->observed_argv = NULL;
    errno = saved;
}

static _Noreturn void launch_die(struct launch_undo *u, const char *msg) {
    launch_undo(u);
    hold_die_errno(msg);
}

static void copy_field(struct launch_undo *u, char *dst, size_t n, const char *src, const char *msg) {
    if (hold_checked_snprintf(dst, n, "%s", src) != 0) launch_die(u, msg);
}

/* ---- launch orchestration ---- */

int hold_perform_start_options(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_start_options *opts) {
    if (!opts) {
        hold_usage();
        return HOLD_EXIT_USAGE;
    }
    bool tail = opts->tail;
    bool console_mode = opts->console_mode;
    bool interactive_stdin = opts->interactive_stdin;
    int argc = opts->argc;
    char **argv = opts->argv;
    int envc = opts->envc;
    char **env = opts->env;
    const char *restart_policy = opts->restart_policy;
    const char *existing_id = opts->existing_id;
    if (argc <= 0 || !argv || !argv[0] ||
        envc < 0 || (envc > 0 && !env) ||
        opts->restart_delay_seconds < HOLD_RESTART_BACKOFF) {
        hold_usage();
        return HOLD_EXIT_USAGE;
    }
    bool restart_enabled = restart_policy_is_enabled(restart_policy);
    /* -i without -t: the program runs on pipes under a broker that relays its
     * input, so the program never holds the person's terminal, Ctrl-P Ctrl-Q
     * can detach, and callbox attach can reconnect. (--restart's supervisor
     * still hands its workers stdin directly.) */
    bool pipe_mode = interactive_stdin && !console_mode && !restart_enabled;
    bool brokered = console_mode || pipe_mode;
    if (restart_enabled && console_mode) {
        fprintf(stderr, "callbox: error: --restart with --tty/-t is not supported yet\n");
        return HOLD_EXIT_USAGE;
    }
    if (opts->chat && (!console_mode || !tail)) {
        fprintf(stderr, "callbox: error: a callbox shell runs attached, on its PTY; -d does not apply\n");
        return HOLD_EXIT_USAGE;
    }
    if (opts->chat && !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))) {
        fprintf(stderr, "callbox: error: callbox shell needs a terminal to draw on; run it from an interactive terminal "
                        "(to drive a program from a script: callbox -dt <cmd>)\n");
        return HOLD_EXIT_USAGE;
    }
    /* --size is for a PTY call; one is never allocated just because a size
     * was given. */
    if (opts->size_cols && !console_mode) {
        fprintf(stderr, "callbox: error: --size sets a PTY call's terminal size; add -t "
                        "(for example: callbox -dt --size 120 40 -- <cmd>)\n");
        return HOLD_EXIT_USAGE;
    }
    /* The terminal the program starts on, settled before anything exists (an
     * unusable size starts nothing): --size, or the size a redialed recipe
     * was placed with; else, for a detached launch, the saved detach-size;
     * else the invoking terminal, when it reports a usable size; else 80x24.
     * The broker runs with /dev/null stdio and cannot ask a terminal, so the
     * launcher samples it here, while the real one is still on its stdio. */
    struct hold_console_options console_options = {.chat = opts->chat, .pipe = pipe_mode, .program = !opts->shell};
    if (opts->chat) {
        /* A callbox shell's broker reads the keys itself: the lead key, then the
         * detach key or the chat key. */
        unsigned char keys[8];
        if (hold_console_get_detach_keys(keys, sizeof(keys)) != 2) {
            fprintf(stderr, "callbox: error: a callbox shell's --detach-keys are two keys: the lead key, then the key that "
                            "detaches (the default is ctrl-p,ctrl-q)\n");
            return HOLD_EXIT_USAGE;
        }
        console_options.lead_key = keys[0];
        console_options.detach_key = keys[1];
        console_options.chat_key = opts->chat_key ? opts->chat_key : HOLD_CHAT_KEY_DEFAULT;
        if (console_options.chat_key == keys[0] || console_options.chat_key == keys[1]) {
            fprintf(stderr, "callbox: error: --chat-key is pressed after the lead key, so it cannot be the lead key or the "
                            "detach key\n");
            return HOLD_EXIT_USAGE;
        }
    }
    if (console_mode) {
        int cols = opts->size_cols, rows = opts->size_rows;
        if (cols && !hold_term_size_valid(cols, rows)) {
            fprintf(stderr, "callbox: error: --size: invalid size '%d %d': columns and rows are whole numbers from 1 to %d\n",
                    cols, rows, HOLD_TERM_MAX_DIM);
            return HOLD_EXIT_USAGE;
        }
        if (!cols && !tail) {
            int saved = hold_saved_detach_size(inv, &cols, &rows);
            if (saved < 0) return HOLD_EXIT_USAGE;
            if (saved != 0) cols = rows = 0;
        }
        if (!cols) {
            struct winsize ws;
            memset(&ws, 0, sizeof(ws));
            if ((ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) &&
                hold_term_size_valid(ws.ws_col, ws.ws_row)) {
                cols = ws.ws_col;
                rows = ws.ws_row;
            } else {
                cols = HOLD_TERM_DEFAULT_COLS;
                rows = HOLD_TERM_DEFAULT_ROWS;
            }
        }
        console_options.cols = (unsigned short)cols;
        console_options.rows = (unsigned short)rows;
    }

    /* Launched as named (symlinks intact: a venv's python finds its venv by
     * its own path); the realpath is recorded only as observed_exe. */
    char resolved_exec_path[HOLD_PATH_MAX];
    const char *path_to_resolve = (opts->exec_path && *opts->exec_path) ? opts->exec_path : argv[0];
    if (hold_find_binary_path(path_to_resolve, resolved_exec_path, sizeof(resolved_exec_path)) != 0) {
        return cannot_start(argv[0], errno);
    }

    char id[ID_STR_LEN] = {0}, log_path[HOLD_PATH_MAX] = {0}, console_sock[HOLD_PATH_MAX] = {0};
    char run_name[ALIAS_MAX_LEN + 1] = {0}, boot_id[128] = {0};
    /* Handshake, stdout, stderr, and target-pid pipe slots; adjacent R/W
     * pairs so hold_cloexec_pipe fills them in place and the unwind closes
     * whatever is still open. */
    enum { HS_R, HS_W, TP_R, TP_W, IO_N };
    int io[IO_N] = {-1, -1, -1, -1};
    struct launch_undo u = {
        .store = store,
        .id = id,
        .log_path = log_path,
        .console_sock = console_sock,
        .fds = io,
        .nfds = IO_N,
        .argc = argc,
        .claim_fd = -1,
    };

    char observed_cwd[HOLD_PATH_MAX] = {0};
    if (!getcwd(observed_cwd, sizeof(observed_cwd))) {
        observed_cwd[0] = '\0';
    }
    if (hold_copy_argv(&u.observed_argv, argc, argv) != 0) {
        hold_die_errno("callbox: failed to prepare observed argv");
    }
    if (hold_copy_argv(&u.launch_argv, argc, argv) != 0) {
        launch_die(&u, "callbox: failed to prepare argv");
    }
    char **observed_argv = u.observed_argv;
    char **launch_argv = u.launch_argv;
    free(launch_argv[0]);
    launch_argv[0] = strdup(resolved_exec_path);
    if (!launch_argv[0]) {
        launch_die(&u, "callbox: failed to prepare argv");
    }
    if (hold_normalize_existing_argv_paths_from_cwd(launch_argv, argc, 1, observed_cwd[0] ? observed_cwd : NULL) != 0) {
        launch_die(&u, "callbox: failed to normalize argv paths");
    }

    struct timespec start_ts;
    clock_gettime(CLOCK_REALTIME, &start_ts);
    int64_t start_unix_ns = (int64_t)start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec;
    bool has_boot = hold_current_boot_id(boot_id, sizeof(boot_id));
    bool restarting_existing = existing_id && *existing_id;
    bool owns_new_log = !restarting_existing;

    if (restarting_existing) {
        if (!hold_valid_id(existing_id) || !opts->existing_log_path || !*opts->existing_log_path) {
            errno = EINVAL;
            launch_die(&u, "callbox: invalid restart record");
        }
        copy_field(&u, id, sizeof(id), existing_id, "callbox: restart metadata too long");
        copy_field(&u, log_path, sizeof(log_path), opts->existing_log_path, "callbox: restart metadata too long");
        if (opts->existing_run_name && *opts->existing_run_name) {
            copy_field(&u, run_name, sizeof(run_name), opts->existing_run_name, "callbox: call name too long");
        }
        /* One launch per call at a time: claim it, then decide. The caller's
         * "not running" was read before this claim; a redial that launched
         * in between is seen only now (see hold_claim_existing_run). */
        u.claim_fd = hold_claim_existing_run(store, id);
        if (u.claim_fd < 0) launch_die(&u, "callbox: failed to claim the call for redial");
        /* sudo-routed user-local redial: the claim is the user's, like the
         * log, so a launcher that died holding it leaves the user a file
         * they can claim again. */
        if (store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user &&
            fchown(u.claim_fd, inv->invoking_uid, inv->invoking_gid) != 0) {
            launch_die(&u, "callbox: failed to set user-local ownership");
        }
        struct hold_run_record cur;
        char cur_path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(store->record_dir, id, &cur, cur_path, sizeof(cur_path)) != 0) {
            char display[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(id, display);
            fprintf(stderr, "callbox: error: call %s is gone (purged while redialing)\n", display);
            launch_undo(&u);
            return HOLD_EXIT_NO_TARGET;
        }
        char boot_now[128];
        bool running = hold_eval_state(&cur, hold_boot_id_or_null(boot_now)) == STATE_RUNNING;
        hold_free_run_record(&cur);
        if (running) {
            char display[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(id, display);
            fprintf(stderr, "callbox: error: call %s is already running\n", display);
            launch_undo(&u);
            return 6; /* as the caller's own check: already running */
        }
    } else {
        if (hold_reserve_run_id(store, resolved_exec_path, argc, launch_argv,
                                observed_cwd[0] ? observed_cwd : NULL,
                                start_unix_ns, id) != 0) {
            launch_die(&u, "callbox: failed to generate id");
        }
        u.reserved = true;
        int name_rc = hold_generate_run_name_for_id(store, id, opts->run_name, run_name);
        if (name_rc != 0) {
            launch_undo(&u);
            return name_rc;
        }
        if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
            launch_die(&u, "callbox: log path too long");
        }
    }
    if (brokered && hold_format_console_sock_path(store, id, console_sock, sizeof(console_sock)) != 0) {
        console_sock[0] = '\0';
        launch_die(&u, "callbox: console socket path too long");
    }
    int log_preflight_fd = hold_open_append_no_symlink(log_path);
    /* Where this run's output will start: a redial appends to the same log,
     * and what is shown of a run (when its console was over before callbox
     * got there) is this run's part, not every earlier run's too. */
    struct stat log_st;
    int64_t run_log_start = (log_preflight_fd >= 0 && fstat(log_preflight_fd, &log_st) == 0) ? (int64_t)log_st.st_size : 0;
    if (log_preflight_fd < 0) {
        launch_die(&u, "callbox: failed to open log");
    }
    /* sudo-routed user-local launch: the log is the user's from the start,
     * before the broker exists - it gives what it writes beside the log the
     * log's owner, and a fast program can end before the lchown below. */
    if (store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user &&
        fchown(log_preflight_fd, inv->invoking_uid, inv->invoking_gid) != 0) {
        close(log_preflight_fd);
        launch_die(&u, "callbox: failed to set user-local ownership");
    }
    close(log_preflight_fd);

    uid_t console_owner_uid = geteuid();
    bool console_have_allowed_peer_uid = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_uid != console_owner_uid;
    uid_t console_allowed_peer_uid = console_have_allowed_peer_uid ? inv->invoking_uid : (uid_t)0;
    bool pin_privileged_cwd = inv && inv->euid_root && inv->have_sudo_user;
    bool clean_child_env = inv && inv->euid_root && store && store->kind == STORE_SYSTEM_MANAGED;

    if (hold_cloexec_pipe(io + HS_R) != 0) {
        launch_die(&u, "callbox: pipe failed");
    }
    /* A waiter stays behind as the program's parent to record its exit code:
     * foreground (it reports that code), -w, --rm, and --restart (whose
     * supervisor is the waiter). A plain -d call leaves nothing behind - root's
     * too: other users see a global call's liveness from its process group,
     * not from the public entry's hint. A TTY call's broker is its parent. */
    bool waiter = !brokered && (opts->wait || tail || opts->auto_remove || restart_enabled);
    /* The waiter (or broker) reports the program's pid back over this pipe. */
    if (!restart_enabled && (brokered || waiter) && hold_cloexec_pipe(io + TP_R) != 0) {
        launch_die(&u, "callbox: pipe failed");
    }

    child_identity_prepare(id, run_name, console_mode);
    /* Only under the fallback (Unix sockets denied, or forced) does a call
     * carry the cross-shell lock; asked once, before the fork (a TTY call's
     * broker decides by its own listener). */
    bool fallback = !brokered && hold_console_fallback_selected();
    g_liveness_lock = fallback && waiter;
    g_output_lock = fallback && !waiter;
    pid_t pid = fork();
    if (pid < 0) {
        launch_die(&u, "callbox: fork failed");
    }
    if (pid == 0) {
        close(io[HS_R]);
        if (io[TP_R] >= 0) close(io[TP_R]);
        /* The claim is the launcher's alone: a broker, waiter or supervisor
         * that kept it would hold every later redial for the call's life. */
        if (u.claim_fd >= 0) close(u.claim_fd);
        if (restart_enabled) {
            run_restart_supervisor(store, id, io[HS_W], interactive_stdin,
                                   envc, env, clean_child_env,
                                   resolved_exec_path, launch_argv,
                                   restart_policy, opts->restart_delay_seconds,
                                   pin_privileged_cwd);
        }
        if (waiter) {
            /* Double-fork: this middle process becomes the waiter; the record
             * pid/pgid/sid are the TARGET's. Its own session first, so killing
             * the caller's process group cannot leave the exit unrecorded (the
             * launcher returns only after the target has exec'd, after this). */
            (void)setsid();
            pid_t target = fork();
            if (target < 0) {
                child_fail_errno(io[HS_W]);
            }
            if (target != 0) {
                /* Under the fallback (sockets denied, or forced), a passive
                 * lifetime lock on the log tells shells in other PID namespaces
                 * the call is alive - taken after the fork, so the target never
                 * holds it. The waiter itself is the original, below. */
                if (g_liveness_lock) hold_supervisor_lock_log(log_path);
                /* Write the target pid BEFORE closing the handshake pipe, so
                 * the parent never sees exec-success without a pid to read. */
                if (io[TP_W] >= 0) {
                    (void)hold_write_all(io[TP_W], &target, sizeof(target));
                    close(io[TP_W]);
                }
                close(io[HS_W]);
                hold_close_stdio_to_devnull();
                signal(SIGTERM, SIG_IGN);
                signal(SIGINT, SIG_IGN);
                signal(SIGHUP, SIG_IGN);
                int status = 0;
                while (waitpid(target, &status, 0) < 0) {
                    if (errno == EINTR) continue;
                    status = 127 << 8;
                    break;
                }
                mark_finished_with_retry(store, id, status, target);
                if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
                if (WIFSIGNALED(status)) _exit(128 + WTERMSIG(status));
                _exit(255);
            }
            if (io[TP_W] >= 0) close(io[TP_W]);
        }
        child_session_setup(io[HS_W], pin_privileged_cwd, envc, env, clean_child_env,
                            !console_mode && !interactive_stdin);
        if (brokered) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd < 0 ||
                dup2(nullfd, STDIN_FILENO) < 0 ||
                dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0) {
                child_fail_errno(io[HS_W]);
            }
            if (nullfd > STDERR_FILENO) {
                close(nullfd);
            }
            hold_run_console_broker(io[HS_W], io[TP_W], store, id, log_path,
                                      console_sock, console_owner_uid,
                                      console_have_allowed_peer_uid,
                                      console_allowed_peer_uid,
                                      argc, launch_argv, resolved_exec_path, &console_options);
            _exit(127);
        }
        redirect_output_to_log(log_path, io[HS_W], g_output_lock);
        execv(resolved_exec_path, launch_argv);
        child_fail_errno(io[HS_W]);
    }

    u.reap_pid = pid;
    u.group = pid;
    u.supervisor = pid;
    u.unlink_log = owns_new_log;
    pid_t broker_pid = brokered ? pid : 0; /* the forked child is the broker itself */
    close(io[HS_W]);
    io[HS_W] = -1;
    for (int w = TP_W; w < IO_N; w += 2) {
        if (io[w] >= 0) {
            close(io[w]);
            io[w] = -1;
        }
    }
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(io[HS_R], &child_errno);
    int handshake_errno = errno;
    close(io[HS_R]);
    io[HS_R] = -1;
    if (handshake < 0) {
        errno = handshake_errno;
        launch_die(&u, "callbox: exec handshake failed");
    }
    if (handshake > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        /* (reported before the undo, which frees launch_argv) */
        int start_rc = cannot_start(launch_argv[0], child_errno);
        u.group = 0; /* the child exited and is reaped — nothing to roll back */
        launch_undo(&u);
        return start_rc;
    }
    if (io[TP_R] >= 0) {
        pid_t target_pid = -1;
        /* All of it, through signals (a resize during launch is one). */
        ssize_t n = 0;
        while ((size_t)n < sizeof(target_pid)) {
            ssize_t r = read(io[TP_R], (char *)&target_pid + n, sizeof(target_pid) - (size_t)n);
            if (r > 0) n += r;
            else if (r < 0 && errno == EINTR) continue;
            else break;
        }
        close(io[TP_R]);
        io[TP_R] = -1;
        if (n != (ssize_t)sizeof(target_pid) || target_pid <= 1) {
            errno = EIO;
            launch_die(&u, "callbox: failed to read target pid");
        }
        pid = target_pid;
        u.reap_pid = pid;
        u.group = pid;
    }

    /* The target's identity, read first: a leader that exits at once (its
     * group living on, as in `sh -c 'worker & exit'`) leaves nothing to read
     * a moment later. */
    uint64_t id_starttime = 0, id_exe_dev = 0, id_exe_ino = 0;
    hold_read_proc_stat_tokens(pid, NULL, &id_starttime);
    hold_read_proc_exe(pid, &id_exe_dev, &id_exe_ino);

    struct hold_run_record r;
    hold_record_init_running(&r, id, log_path, pid, pid, pid, start_unix_ns,
                             opts->existing_created_unix_ns > 0 ? opts->existing_created_unix_ns : start_unix_ns);
    if (opts->existing_created_at && *opts->existing_created_at) {
        copy_field(&u, r.created_at, sizeof(r.created_at), opts->existing_created_at, "callbox: created timestamp too long");
    }
    if (store->kind == STORE_SYSTEM_MANAGED) {
        r.has_invocation = true;
        bool via_sudo = inv && inv->have_sudo_user;
        r.invoked_by_uid = via_sudo ? inv->invoking_uid : 0;
        r.invoked_by_gid = via_sudo ? inv->invoking_gid : 0;
        r.invoked_via_sudo = via_sudo;
        copy_field(&u, r.invoked_by_user, sizeof(r.invoked_by_user),
                   via_sudo ? inv->invoking_user : "root", "callbox: invoking user too long");
    }
    if (run_name[0]) {
        r.has_name = true;
        copy_field(&u, r.name, sizeof(r.name), run_name, "callbox: call name too long");
    }
    r.auto_remove = opts->auto_remove;
    r.pid_ns = hold_current_pid_ns();
    /* Whether a supervisor holds the lifetime log lock - a waiter under the
     * fallback, or a broker whose endpoint came up as a FIFO - so that the
     * lock going away means the call has ended. (A plain -d call's output
     * lock says only that it runs: a program may close its output.) */
    struct stat ep;
    r.liveness_lock = brokered ? (console_sock[0] && lstat(console_sock, &ep) == 0 && S_ISFIFO(ep.st_mode))
                                   : g_liveness_lock;
    if (console_sock[0]) {
        r.has_console = true;
        copy_field(&u, r.console_sock, sizeof(r.console_sock), console_sock, "callbox: console socket path too long");
    }
    r.recipe.mode_interactive = interactive_stdin;
    r.recipe.mode_tty = console_mode;
    r.recipe.mode_detach = !tail; /* how it ran: -dt redials detached too */
    r.recipe.mode_wait = waiter;
    r.recipe.allow_multi = false;
    r.log_start = run_log_start;
    r.recipe.mode_chat = opts->chat || opts->user_mode;
    r.recipe.mode_shell = opts->shell;
    r.recipe.has_chat_key = opts->chat;
    r.recipe.chat_key = console_options.chat_key;
    r.recipe.has_keys = opts->chat;
    r.recipe.lead_key = console_options.lead_key;
    r.recipe.detach_key = console_options.detach_key;
    r.recipe.has_size = console_mode && opts->size_cols > 0;
    r.recipe.size_cols = opts->size_cols;
    r.recipe.size_rows = opts->size_rows;
    /* The broker is its own session's leader; a message it sends for the
     * call (a callbox shell's chat) is verified against this. */
    r.has_broker = broker_pid > 1;
    r.broker_pid = broker_pid;
    if (envc > 0 && hold_copy_argv(&r.recipe.env, envc, env) != 0) {
        launch_die(&u, "callbox: failed to copy env metadata");
    }
    r.recipe.envc = envc;
    if (restart_enabled) {
        copy_field(&u, r.recipe.restart_policy, sizeof(r.recipe.restart_policy), restart_policy, "callbox: restart policy too long");
        r.recipe.has_restart_policy = true;
    }
    /* Recorded only when given, so a redial replays an explicit delay (0
     * too) and otherwise backs off again. */
    r.recipe.has_restart_delay = opts->restart_delay_seconds >= 0;
    r.recipe.restart_delay_seconds = r.recipe.has_restart_delay ? opts->restart_delay_seconds : 0;
    r.has_boot = has_boot;
    if (r.has_boot) {
        snprintf(r.boot_id, sizeof(r.boot_id), "%s", boot_id);
    }
    r.proc_starttime_ticks = id_starttime;
    r.exe_dev = id_exe_dev;
    r.exe_ino = id_exe_ino;
    if (hold_format_argv_human(r.cmdline, sizeof(r.cmdline), argc, launch_argv) != 0) {
        snprintf(r.cmdline, sizeof(r.cmdline), "?");
    }
    r.has_observed = true;
    char real_exe[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(resolved_exec_path, real_exe, sizeof(real_exe)) != 0)
        snprintf(real_exe, sizeof(real_exe), "%s", resolved_exec_path);
    copy_field(&u, r.observed_exe, sizeof(r.observed_exe), real_exe, "callbox: observed executable path too long");
    copy_field(&u, r.observed_cwd, sizeof(r.observed_cwd), observed_cwd, "callbox: observed cwd too long");
    r.observed_argc = argc;
    r.observed_argv = observed_argv;

    char record_path[HOLD_PATH_MAX] = {0};
#ifdef HOLD_TESTING
    if (getenv("HOLD_TEST_FAIL_RECORD_WRITE")) {
        errno = EIO;
        launch_die(&u, "callbox: failed to write record");
    }
#endif
    int record_lock = -1;
    if (restarting_existing) {
        /* A redial replaces the record, but what the person set on the call
         * (callbox save, rename) is the call's, not the run's: carried over
         * from the record as it is now, under the lock save and rename
         * write under, so one made during this launch is kept too. Why: a
         * redialed call that lost its saved flag was deleted by the next
         * plain purge. */
        record_lock = hold_store_lock(store->record_dir);
        struct hold_run_record cur;
        char cur_path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(store->record_dir, id, &cur, cur_path, sizeof(cur_path)) == 0) {
            r.saved = cur.saved;
            r.has_name = cur.has_name;
            r.name[0] = '\0';
            if (cur.has_name) snprintf(r.name, sizeof(r.name), "%s", cur.name);
            hold_free_run_record(&cur);
        }
    }
    int write_rc = hold_write_record_atomic(store->record_dir, &r, argc, launch_argv, record_path, sizeof(record_path));
    hold_store_unlock(record_lock);
    if (write_rc != 0) {
        launch_die(&u, "callbox: failed to write record");
    }
    if (!restarting_existing) {
        u.record_path = record_path;
    }
    /* Committed: the record write removed the claim's file; a waiting
     * redial now finds this call running. */
    hold_release_existing_run(store, id, u.claim_fd);
    u.claim_fd = -1;

    if (store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user) {
        /* sudo-routed user-local launch: hand the artifacts back. */
        int chown_rc = 0;
        if (record_path[0] &&
            lchown(record_path, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (lchown(log_path, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (console_sock[0] && hold_path_exists(console_sock) &&
            lchown(console_sock, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (chown_rc != 0) {
            if (errno == 0) errno = EIO;
            launch_die(&u, "callbox: failed to set user-local ownership");
        }
    }
    if (store->kind == STORE_SYSTEM_MANAGED) {
        int public_rc = 0;
#ifdef HOLD_TESTING
        if (getenv("HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE")) {
            errno = EIO;
            public_rc = -1;
        } else
#endif
        if (hold_write_public_index_atomic(store, &r, NULL) != 0) {
            public_rc = -1;
        }
        if (public_rc != 0) {
            int saved = errno ? errno : EIO;
            char public_path[HOLD_PATH_MAX];
            if (hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                unlink(public_path);
            }
            errno = saved;
            launch_die(&u, "callbox: failed to write public index");
        }
    }

    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    /* Docker's dialect: detach prints the full ID alone; foreground prints
     * nothing of its own (the call's output is what follows). */
    if (!tail) printf("%s\n", r.id);
    fflush(stdout);

    if (tail) {
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        int tail_rc = 0;
        if (brokered) {
            /* placed from inside another call: this attach is nested in it */
            struct hold_store home_store;
            const char *home = getenv("HOME");
            bool have_home = home && *home && hold_init_user_store_from_home(home, &home_store) == 0;
            hold_note_enclosing_call(store, have_home ? &home_store : NULL);
            hold_console_set_composited(opts->chat);
            hold_console_set_line_mode(pipe_mode);
            hold_console_set_stream(!(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)));
            hold_console_set_forward_term(true);
            /* A program on pipes printed its output into this terminal as it
             * went; there is no screen to print again when it ends. */
            bool on_terminal = console_mode && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
            tail_rc = hold_run_native_console(r.console_sock);
            if (hold_console_reader_gone()) {
                /* `callbox foo | head`: the reader has what it wanted. foo is
                 * not ours to end; say where it is, then end as a writer to
                 * a closed pipe does (141). */
                const char *label = r.has_name ? r.name : display_id;
                if (hold_call_still_running(store, id))
                    hold_sig_note(inv, "callbox: %s keeps running; end it with: callbox end %s\n", label, label);
                signal(SIGPIPE, SIG_DFL);
                raise(SIGPIPE);
            }
            /* A program that is over before the attach gets there is not an
             * error: it ran, and what it printed and how it exited follow. */
            /* From a script, a call that was over before the stream reached
             * it (it sent nothing) is shown from the log, too. */
            bool streamed = !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
            bool unseen = hold_console_was_gone() || (streamed && tail_rc == 0 && !hold_console_heard_output());
            if (unseen) tail_rc = 0;
            /* Detached, or over (an attach that failed has said so itself).
             * An ended call's exit status is callbox's own, as for a launch
             * without -t. */
            if (tail_rc == 0 && hold_after_attach(inv, store, &r, r.has_name ? r.name : display_id, on_terminal, unseen))
                tail_rc = foreground_exit_status(store, r.id);
        } else {
            tail_rc = hold_follow_launched_call(inv, &r, r.has_name ? r.name : display_id);
            if (tail_rc == 0) tail_rc = foreground_exit_status(store, r.id);
        }
        if (opts->auto_remove && hold_call_still_running(store, r.id)) {
            /* Detached (Ctrl-P Ctrl-Q) or interrupted while it runs on: it
             * is removed when it ends, as a -d --rm call is. */
            spawn_auto_remove_watcher(store, &r);
        } else if (opts->auto_remove) {
            char boot[128];
            const char *watch_boot = hold_boot_id_or_null(boot);
            bool removed = false;
            int prune_rc = hold_prune_one_run(store, r.id, watch_boot, true, &removed);
            /* 5: already gone (removed meanwhile) - what --rm wanted */
            if (tail_rc == 0 && prune_rc != 0 && prune_rc != 5) tail_rc = prune_rc;
        }
        hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
        return tail_rc;
    }
    if (opts->auto_remove) {
        spawn_auto_remove_watcher(store, &r);
    }
    hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
    free_launch_and_observed_argv(launch_argv, observed_argv, argc);
    return 0;
}

int hold_ensure_start_store_for_command(const struct hold_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct hold_store *store) {
    bool wants_system_store = (inv && inv->euid_root) || requested_system;

    if (wants_system_store &&
        hold_start_target_is_within_invoking_home(inv, owned, command, argc, argv)) {
        if (inv && inv->euid_root && inv->have_sudo_user) {
            return hold_ensure_invoking_user_store(inv, store);
        }
        return hold_ensure_user_store_for_current_user(store);
    }

    if (wants_system_store) {
        return hold_ensure_system_store(store);
    }
    return hold_ensure_user_store_for_current_user(store);
}
