#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/term.h"

/* ---- signal-target safety chain -------------------------------------- */

enum signal_validation_state { SIGNAL_TARGET_RUNNING, SIGNAL_TARGET_EXITED };

static int refuse_signal(const char *id, const char *why) {
    fprintf(stderr, "backmux: error: process %s %s\n", id, why);
    return 2;
}

/* The safety chain: boot-id -> starttime -> exe dev/ino -> pgid/sid. A process
 * is signaled only when the live process tree still matches the recorded
 * identity; a record with no identity token is refused outright. */
static int validate_signal_target(const char *id,
                                  const struct hold_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out) {
    if (state_out) *state_out = SIGNAL_TARGET_RUNNING;
    if (r->pgid <= 1) {
        fprintf(stderr, "backmux: error: invalid pgid %ld in record file\n", (long)r->pgid);
        return 5;
    }
    if (r->sid <= 0) {
        fprintf(stderr, "backmux: error: invalid sid %ld in record file\n", (long)r->sid);
        return 5;
    }

    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    if (r->has_boot && boot_id && strcmp(r->boot_id, boot_id) != 0)
        return refuse_signal(id, "is stale and cannot be signaled");

    bool have_identity_token = r->proc_starttime_ticks != 0 || (r->exe_dev != 0 && r->exe_ino != 0);
    char leader_state = 0;
    uint64_t leader_starttime = 0;
    bool have_leader_stat = hold_read_proc_stat_tokens(r->pid, &leader_state, &leader_starttime) == 0;
    if (have_leader_stat && leader_state != 'Z') {
        if (r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks)
            return refuse_signal(id, "process identity differs from the record and cannot be signaled");
        if (r->proc_starttime_ticks == 0 && r->exe_dev != 0 && r->exe_ino != 0) {
            uint64_t exe_dev = 0;
            uint64_t exe_ino = 0;
            if (hold_read_proc_exe(r->pid, &exe_dev, &exe_ino) == 0 &&
                (exe_dev != r->exe_dev || exe_ino != r->exe_ino))
                return refuse_signal(id, "process identity differs from the record and cannot be signaled");
        }
        pid_t current_pgid = getpgid(r->pid);
        pid_t current_sid = getsid(r->pid);
        if ((current_pgid >= 0 && current_pgid != r->pgid) ||
            (current_sid >= 0 && current_sid != r->sid))
            return refuse_signal(id, "process group/session differs from the record and cannot be signaled");
        if (current_pgid >= 0 && current_sid >= 0) {
            if (!have_identity_token)
                return refuse_signal(id, "has no recorded process identity token and cannot be signaled");
            return 0;
        }
    }
    if (have_leader_stat && r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks)
        return refuse_signal(id, "process identity differs from the record and cannot be signaled");

    /* Leader gone, group live: there is nothing left to compare the recorded
     * identity against, and no atomic verify-and-signal exists for a process
     * group. Matching the recorded pgid+sid within the same boot is the
     * deliberate best effort SPEC.md states; the zombie-leader test pins it. */
    enum group_liveness gl = hold_group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_LIVE) {
        if (!have_identity_token)
            return refuse_signal(id, "has no recorded process identity token and cannot be signaled");
        return 0;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        if (require_live)
            return refuse_signal(id, "is not running and cannot be printed as a signal command");
        if (state_out) *state_out = SIGNAL_TARGET_EXITED;
        return 0;
    }

    return refuse_signal(id, "could not be tied to its recorded process group/session and cannot be signaled");
}

/* ---- end/stop/kill ---------------------------------------------------- */

/* A console process running where this shell cannot see its pid (another PID
 * namespace): follow the chain of command and ask its broker - which can see
 * its program - to signal it. 0 when asked, -1 when no broker answers. */
static int signal_through_broker(const struct hold_run_record *r, int sig) {
    char req[32];
    snprintf(req, sizeof(req), "signal sig=%d", sig);
    unsigned char *reply = NULL;
    size_t len = 0;
    bool ok = hold_console_request(r->console_sock, req, strlen(req), &reply, &len) == 0 && reply &&
              strncmp((const char *)reply, "ok", 2) == 0;
    free(reply);
    return ok ? 0 : -1;
}

/* Why this shell cannot end a process it can see in the store but not as a
 * process: it runs (or ran) in another PID namespace. */
static void not_running_here(const struct hold_run_record *r, bool can_tell) {
    const char *label = r->has_name && r->name[0] ? r->name : r->id;
    if (can_tell)
        fprintf(stderr,
                "backmux: %s is not running in this environment: it runs in another PID namespace (the shell that "
                "started it), and can be ended only from there. Processes started with -dt can be ended from any shell.\n",
                label);
    else
        fprintf(stderr,
                "backmux: %s is not running in this environment, and this shell cannot tell whether it still runs in "
                "the one that started it (no exit was recorded). End it from there; to drop the record, "
                "backmux purge %s --force.\n", label, label);
}

/* Whether the process has stopped running, as this shell can tell, within ms. */
static bool call_ended_within(const struct hold_store *store, const char *id, int ms) {
    for (int waited = 0;; waited += 50) {
        struct hold_run_record cur;
        char path[HOLD_PATH_MAX], boot[128];
        if (hold_load_record_by_id(store->record_dir, id, &cur, path, sizeof(path)) != 0) {
            int code;
            if (hold_auto_removed_exit(store, id, &code)) return true;
        } else {
            enum run_state st = hold_eval_state(&cur, hold_boot_id_or_null(boot));
            hold_free_run_record(&cur);
            if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) return true;
        }
        if (waited >= ms) return false;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
        nanosleep(&sl, NULL);
    }
}

/* One target: load, validate, then either print the equivalent kill command
 * (--print revalidates with require_live) or deliver sig with the graceful
 * TERM -> revalidate -> KILL escalation. */
static int signal_one(const struct hold_store *store,
                      const char *id,
                      int sig,
                      bool graceful,
                      bool print_cmd,
                      bool *already_done) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    *already_done = false;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return 5;
    char boot[128];
    int rc = 0;
    uint64_t here = hold_current_pid_ns();
    bool foreign = r.pid_ns != 0 && here != 0 && r.pid_ns != here;
    if (foreign) {
        enum run_state st = hold_eval_state(&r, hold_boot_id_or_null(boot));
        if (st == STATE_STALE) {
            rc = refuse_signal(id, "is stale and cannot be signaled");
            goto out;
        }
        if (st == STATE_EXITED || st == STATE_FAILED) {
            if (print_cmd) rc = refuse_signal(id, "is not running and cannot be printed as a signal command");
            else *already_done = true;
            goto out;
        }
        if (!r.has_console || print_cmd) {
            /* A -d process has no broker to ask (it stays the minimal mode), and
             * there is no kill command to print for a pid that means nothing
             * here. */
            not_running_here(&r, st == STATE_RUNNING);
            rc = 3;
            goto out;
        }
        if (signal_through_broker(&r, sig) != 0) {
            if (call_ended_within(store, id, 0)) *already_done = true;
            else {
                fprintf(stderr, "backmux: cannot reach %s's console to signal it; its exit is not confirmed\n",
                        r.has_name ? r.name : r.id);
                rc = 3;
            }
            goto out;
        }
        if (!graceful || call_ended_within(store, id, STOP_TIMEOUT_MS)) goto out;
        if (signal_through_broker(&r, SIGKILL) != 0) {
            if (!call_ended_within(store, id, 0)) {
                fprintf(stderr, "backmux: cannot reach %s's console to finish stopping it; its exit is not confirmed\n",
                        r.has_name ? r.name : r.id);
                rc = 3;
            }
            goto out;
        }
        if (call_ended_within(store, id, 1000)) goto out;
        rc = 4;
        goto out;
    }
    enum signal_validation_state state = SIGNAL_TARGET_RUNNING;
    rc = validate_signal_target(id, &r, print_cmd, &state);
    if (rc != 0) goto out;
    if (print_cmd) {
        printf("kill -%s -- -%ld\n", sig == SIGKILL ? "KILL" : "TERM", (long)r.pgid);
        goto out;
    }
    if (state == SIGNAL_TARGET_EXITED) { *already_done = true; goto out; }
    if (kill(-r.pgid, sig) != 0) {
        if (errno == ESRCH) *already_done = true;
        else rc = errno == EPERM ? 3 : 4;
        goto out;
    }
    if (!graceful) { hold_report_session_escapees(&r); goto out; }
    if (hold_wait_target_group_gone(&r, STOP_TIMEOUT_MS)) { hold_report_session_escapees(&r); goto out; }
    state = SIGNAL_TARGET_RUNNING;
    rc = validate_signal_target(id, &r, false, &state);
    if (rc != 0) goto out;
    if (state == SIGNAL_TARGET_EXITED) { *already_done = true; goto out; }
    if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
        rc = errno == EPERM ? 3 : 4;
        goto out;
    }
    if (hold_wait_target_group_gone(&r, 1000)) { hold_report_session_escapees(&r); goto out; }
    rc = 4;
out:
    hold_free_run_record(&r);
    return rc;
}

int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd) {
    if (argc <= 0) {
        fprintf(stderr, "usage: backmux %s <target>...\n", command);
        return HOLD_EXIT_USAGE;
    }
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    /* Each target on its own, as docker stop a b c does: one that cannot be
     * resolved (or reached) is reported and skipped, the rest are still
     * ended, and the exit code is the worst of them. Why: `backmux end a b
     * c` with one mistyped name used to end none, leaving the others running
     * while it looked like a typo. */
    int worst = 0;
    bool noted_ended = false; /* said "already exited": not also "nothing to" */
    for (int i = 0; i < argc; i++) {
        struct hold_resolved_target *one = NULL;
        int none = 0;
        /* A process from another PID namespace that this shell cannot judge does
         * not match a running-only target; say why instead of "no match". */
        if (!print_cmd && !hold_token_resolves(inv, user_store, system_store, command, argv[i]) &&
            hold_token_resolves(inv, user_store, system_store, "inspect", argv[i])) {
            struct hold_resolved_target t;
            bool hit = false;
            if (hold_resolve_single(inv, user_store, system_store, "inspect", "inspect", argv[i], &t, &hit) == 0 && hit) {
                struct hold_run_record r;
                char path[HOLD_PATH_MAX], boot[128];
                if (hold_load_record_by_id(t.store.record_dir, t.id, &r, path, sizeof(path)) == 0) {
                    uint64_t here = hold_current_pid_ns();
                    bool foreign = r.pid_ns != 0 && here != 0 && r.pid_ns != here;
                    enum run_state st = hold_eval_state(&r, hold_boot_id_or_null(boot));
                    bool unknown = st == STATE_UNKNOWN;
                    /* A known process that has already ended: nothing to do,
                     * which is success (backmux help scripting: "includes
                     * known name with nothing to do"). Why: "no process
                     * matches" denied a process that list -a and purge find, and
                     * exit 5 failed a cleanup step whose goal already held. */
                    bool ended = st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
                    if (foreign && unknown) not_running_here(&r, false);
                    if (ended) {
                        char display[ID_DISPLAY_HEX_LEN + 1];
                        hold_run_id_display(r.id, display);
                        hold_sig_note(inv, "backmux: %s already exited\n", r.has_name ? r.name : display);
                    }
                    hold_free_run_record(&r);
                    if (foreign && unknown) {
                        if (3 > worst) worst = 3;
                        continue;
                    }
                    if (ended) {
                        noted_ended = true;
                        continue;
                    }
                }
            }
        }
        int rc = hold_resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            if (rc > worst) worst = rc;
            continue;
        }
        if (none > 0) {
            struct hold_resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
            if (!next) {
                free(one);
                free(targets);
                return 3;
            }
            targets = next;
            memcpy(targets + ntargets, one, (size_t)none * sizeof(*targets));
            ntargets += none;
        }
        free(one);
    }
    if (ntargets == 0) {
        free(targets);
        if (worst == 0 && !noted_ended) hold_sig_note(inv, "backmux: nothing to %s\n", command);
        return worst;
    }
    /* Root-managed targets a non-root caller cannot signal: reported, and
     * dropped from the list, the rest still ended. */
    int kept = 0;
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            int rc = hold_report_requires_root(targets[i].id);
            if (rc > worst) worst = rc;
            continue;
        }
        targets[kept++] = targets[i];
    }
    ntargets = kept;
    /* What each process is known by, read before it is signalled: an --rm process's
     * record is gone by the time there is something to say about it. */
    char (*labels)[ALIAS_MAX_LEN + 1] = calloc(ntargets > 0 ? (size_t)ntargets : 1, sizeof(*labels));
    if (!labels) {
        free(targets);
        return 3;
    }
    for (int i = 0; i < ntargets; i++) {
        struct hold_run_record r;
        char path[HOLD_PATH_MAX];
        snprintf(labels[i], sizeof(labels[i]), "%.12s", targets[i].id);
        if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, path, sizeof(path)) == 0) {
            if (r.has_name && r.name[0]) snprintf(labels[i], sizeof(labels[i]), "%s", r.name);
            hold_free_run_record(&r);
        }
    }
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = signal_one(&targets[i].store, targets[i].id, sig, graceful, print_cmd, &already_done);
        /* --rm: removed before we return, so the name is free for a rerun. */
        if (!print_cmd && rc == 0) hold_auto_remove_settle(&targets[i].store, targets[i].id, 3000);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                hold_sig_note(inv, "backmux: %s already exited\n", labels[i]);
            } else {
                hold_sig_note(inv, "backmux: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", labels[i]);
            }
        }
        if (rc > worst) worst = rc;
    }
    free(labels);
    free(targets);
    return worst;
}

/* ---- single-target prologue (tail / inspect / logs) ------------------- */

/* Resolve a single-target token; on a hit the target is copied out and the
 * list freed. *hit false means the caller returns rc as its exit status
 * (covers resolve errors and the friendly "nothing to <verb>" no-op). */
int hold_resolve_single(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *command,
                          const char *verb,
                          const char *token,
                          struct hold_resolved_target *out,
                          bool *hit) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    *hit = false;
    int rc = hold_resolve_action_token(inv, user_store, system_store, command, token, false, &targets, &ntargets);
    if (rc == 0 && ntargets == 0) {
        hold_sig_note(inv, "backmux: nothing to %s\n", verb);
    } else if (rc == 0) {
        *out = targets[0];
        *hit = true;
    }
    free(targets);
    return rc;
}

/* Requires-root gate + record load; *ok false means the caller returns rc. */
int hold_load_target_record(const struct hold_resolved_target *target,
                              bool need_log,
                              struct hold_run_record *r,
                              char *path,
                              size_t path_len,
                              bool *ok) {
    *ok = false;
    if (target->requires_root) return hold_report_requires_root(target->id);
    if (hold_load_record_by_id(target->store.record_dir, target->id, r, path, path_len) != 0) return 5;
    if (need_log && !r->has_log) {
        fprintf(stderr, "backmux: record has no log path: %s\n", target->id);
        hold_free_run_record(r);
        return 5;
    }
    *ok = true;
    return 0;
}

/* ---- raw tail ---------------------------------------------------------- */

struct tail_state {
    const struct hold_run_record *r;
    const char *boot_id;
};

static int tail_call_ended(void *ctx) {
    const struct tail_state *t = ctx;
    return hold_eval_state(t->r, t->boot_id) != STATE_RUNNING;
}

/* This run's output, from where it starts in the log (redials append to one
 * log; the earlier runs are not this run's). */
int hold_print_run_log(const struct hold_run_record *r) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (r->log_start > 0) (void)lseek(fd, (off_t)r->log_start, SEEK_SET);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) break;
    close(fd);
    return 0;
}

int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) hold_die_errno("backmux: failed to open log for tail");
    char boot[128] = {0};
    struct tail_state t = {.r = r, .boot_id = r->has_boot ? hold_boot_id_or_null(boot) : NULL};
    if (from_end) lseek(fd, 0, SEEK_END);
    /* Logs are the raw bytes the program wrote; following is a straight copy
     * to stdout, through the one client loop. Ctrl-C (SIGINT) stops following,
     * never the process: a follower is watching a log, not on the process. */
    struct hold_client c = {
        .out_fd = fd,
        .out_is_file = true,
        .file_done = follow_until_exit ? tail_call_ended : NULL,
        .in_fd = -1,
        .want_sigint = true,
        .ctx = &t,
    };
    enum hold_client_end end = hold_client_run(&c);
    int run_errno = errno;
    close(fd);
    if (end == HOLD_CLIENT_ERROR && run_errno == EPIPE) {
        /* The reader went away (`backmux logs x | head`): end quietly, as a
         * writer to a closed pipe does. */
        signal(SIGPIPE, SIG_DFL);
        raise(SIGPIPE);
    }
    errno = run_errno;
    if (end == HOLD_CLIENT_ERROR) hold_die_errno("backmux: failed while tailing log");
    if (end == HOLD_CLIENT_SIGNALED) hold_term_reraise_stop();
    return 0;
}

/* ---- following a process you launched ---------------------------------------
 * A plain process launched in the foreground: backmux follows its log, and the
 * keys mean what they mean everywhere you are on a process - Ctrl-C goes to the
 * program, Ctrl-Space q leaves it running. At a terminal the keys are read
 * one by one (output is untouched); without one, a SIGINT sent to backmux is
 * passed on, as docker run's sig-proxy does. */

struct launch_follow {
    struct tail_state t;
    bool detached;
};

static int follow_interrupt(void *ctx) {
    struct launch_follow *f = ctx;
    enum signal_validation_state st;
    /* the same safety chain as backmux end: never signal a reused pid */
    if (validate_signal_target(f->t.r->id, f->t.r, false, &st) == 0 && st == SIGNAL_TARGET_RUNNING)
        (void)kill(-f->t.r->pgid, SIGINT);
    return 0;
}

static int follow_detach(void *ctx) {
    ((struct launch_follow *)ctx)->detached = true;
    return 1;
}

int hold_follow_launched_call(const struct hold_invocation *inv, const struct hold_run_record *r, const char *label) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) hold_die_errno("backmux: failed to open log for tail");
    /* This run's output only: a redial appends to the same log, and what a
     * launch shows is the run it started, not every earlier run's too. */
    if (r->log_start > 0) (void)lseek(fd, (off_t)r->log_start, SEEK_SET);
    char boot[128] = {0};
    struct launch_follow f = {.t = {.r = r, .boot_id = r->has_boot ? hold_boot_id_or_null(boot) : NULL}};
    /* The keyboard is ours only when we are the terminal's foreground job:
     * `backmux -w cmd &` must not touch the terminal (the shell would stop
     * it for doing so). */
    bool keyboard = isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) == getpgrp();
    struct hold_term_saved saved;
    memset(&saved, 0, sizeof(saved));
    struct hold_term_detach keys;
    unsigned char dk[HOLD_TERM_DETACH_MAX_KEYS];
    size_t ndk = hold_console_get_detach_keys(dk, sizeof(dk));
    hold_term_detach_init(&keys, dk, ndk);
    hold_term_detach_set_interrupt(&keys, 0x03); /* Ctrl-C */
    if (keyboard) (void)hold_term_input_enter(STDIN_FILENO, HOLD_TERM_INPUT_KEYS, &saved);
    struct hold_client c = {
        .out_fd = fd,
        .out_is_file = true,
        .file_done = tail_call_ended,
        .in_fd = keyboard ? STDIN_FILENO : -1,
        .keys = keyboard ? &keys : NULL,
        .on_detach = follow_detach,
        .on_interrupt = follow_interrupt,
        .want_sigint = true, /* SIGINT from outside is passed on, too */
        .ctx = &f,
    };
    enum hold_client_end end = hold_client_run(&c);
    int run_errno = errno;
    hold_term_input_restore(&saved);
    close(fd);
    if (end == HOLD_CLIENT_SIGNALED) hold_term_reraise_stop();
    if (end == HOLD_CLIENT_ERROR && run_errno == EPIPE) {
        /* `backmux -w cmd | head`: the reader has what it wanted. The process is
         * not ours to end; say where it is, then end as a writer to a closed
         * pipe does (141) - the same as an attached launch. */
        if (!tail_call_ended(&f.t))
            hold_sig_note(inv, "backmux: %s keeps running; end it with: backmux end %s\n", label, label);
        signal(SIGPIPE, SIG_DFL);
        raise(SIGPIPE);
    }
    errno = run_errno;
    if (end == HOLD_CLIENT_ERROR) hold_die_errno("backmux: failed while tailing log");
    if (f.detached)
        hold_sig_note(inv, "backmux: %s keeps running; follow it again: backmux --tail %s\n", label, label);
    return 0;
}

int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "tail", "tail", id_token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, true, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    bool running = hold_eval_state(&r, boot_id) == STATE_RUNNING;
    rc = hold_tail_log_until_exit(&r, running, running);
    hold_free_run_record(&r);
    return rc;
}

int hold_cmd_tail_if_call(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *word) {
    if (!word || strchr(word, '/') || !hold_token_resolves(inv, user_store, system_store, "tail", word)) return -1;
    return hold_cmd_tail_action(inv, user_store, system_store, word);
}

/* ---- inspect ------------------------------------------------------------ */

/* Build the neutral fd-target object for a live process's leader: where fds 0/1/2
 * actually point right now (a pipe, the pty, /dev/null, a redirected file).
 * Returns a heap string like "  \"Stdio\": {\n ... }" or NULL when the process
 * is gone or none of its fds could be read. Caller frees. */
static char *build_stdio_object(pid_t pid) {
    static const struct { int fd; const char *key; } slots[] = {
        {0, "Stdin"}, {1, "Stdout"}, {2, "Stderr"}
    };
    char targets[3][HOLD_PATH_MAX];
    bool any = false;
    for (size_t i = 0; i < 3; i++) {
        if (hold_proc_fd_target(pid, slots[i].fd, targets[i], sizeof(targets[i])) == 0) any = true;
        else targets[i][0] = '\0';
    }
    if (!any) return NULL;
    size_t cap = 4096;
    char *obj = malloc(cap);
    if (!obj) return NULL;
    FILE *f = fmemopen(obj, cap, "w");
    if (!f) {
        free(obj);
        return NULL;
    }
    fprintf(f, "  \"Stdio\": {\n");
    for (size_t i = 0; i < 3; i++) {
        fprintf(f, "    \"%s\": \"", slots[i].key);
        hold_json_escape(f, targets[i]);
        fprintf(f, "\"%s\n", i < 2 ? "," : "");
    }
    fprintf(f, "  }");
    if (fclose(f) != 0) {
        free(obj);
        return NULL;
    }
    return obj;
}

/* Print the record file as a one-object JSON array. With a live "Stdio"
 * object, splice it in before the object's closing brace: the record is a
 * single top-level object, so the last '}' in the file is its terminator. */
/* The record as stored, with what backmux observes now: "state" is the evaluated
 * state (as backmux list shows it) and, when the record says otherwise - a process
 * killed before its exit was stamped still reads "running" - the stored value
 * is kept as "recorded_state". A running process also gets its live stdio. */
static int write_inspect(const char *path, const char *observed, const char *stdio_obj) {
    char *buf = NULL;
    if (hold_read_owned_file_no_symlink(path, &buf) != 0 || !buf) return -1;
    char recorded[32] = "";
    char *val = strstr(buf, "\"state\": \"");
    if (val) {
        val += strlen("\"state\": \"");
        char *q = strchr(val, '"');
        if (q && (size_t)(q - val) < sizeof(recorded) && strncmp(val, observed, (size_t)(q - val)) != 0) {
            memcpy(recorded, val, (size_t)(q - val));
            recorded[q - val] = '\0';
        }
    }
    char *close = strrchr(buf, '}');
    if (!close) {
        free(buf);
        return -1;
    }
    /* Trim whitespace before the closing brace so spliced fields follow the
     * last existing value directly. */
    while (close > buf && (close[-1] == '\n' || close[-1] == ' ' || close[-1] == '\t' || close[-1] == '\r')) close--;
    *close = '\0';
    int rc = 0;
    if (fputs("[\n", stdout) == EOF) rc = -1;
    if (rc == 0 && recorded[0] && val) {
        /* the stored value, replaced by the observed one */
        char *q = strchr(val, '"');
        if (fwrite(buf, 1, (size_t)(val - buf), stdout) != (size_t)(val - buf) || fputs(observed, stdout) == EOF ||
            fputs(q, stdout) == EOF)
            rc = -1;
        if (rc == 0 && printf(",\n  \"recorded_state\": \"%s\"", recorded) < 0) rc = -1;
    } else if (rc == 0 && fputs(buf, stdout) == EOF) {
        rc = -1;
    }
    if (rc == 0 && stdio_obj && (fputs(",\n", stdout) == EOF || fputs(stdio_obj, stdout) == EOF)) rc = -1;
    if (rc == 0 && fputs("\n}\n]\n", stdout) == EOF) rc = -1;
    free(buf);
    return rc;
}

int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "inspect", id_token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    /* Live fd targets are observable state, added only for a running process whose
     * leader still exists; ended processes fall back to the verbatim record. */
    char *stdio_obj = st == STATE_RUNNING && r.pid > 1 ? build_stdio_object(r.pid) : NULL;
    rc = write_inspect(path, hold_state_str(st), stdio_obj);
    free(stdio_obj);
    hold_free_run_record(&r);
    if (rc != 0) hold_die_errno("backmux: failed to inspect run record");
    return 0;
}

/* ---- logs ----------------------------------------------------------------- */

/* Offset where the newest `lines` lines begin (a trailing newline does not
 * start an empty line). Scans backward in chunks; 0 when the file is shorter. */
static off_t offset_of_last_lines(int fd, long lines) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) return 0;
    char last = 0;
    if (pread(fd, &last, 1, end - 1) != 1) return 0;
    long seen = last == '\n' ? -1 : 0; /* the final newline only ends the last line */
    char buf[8192];
    off_t pos = end;
    while (pos > 0) {
        size_t chunk = pos < (off_t)sizeof(buf) ? (size_t)pos : sizeof(buf);
        pos -= (off_t)chunk;
        ssize_t got = pread(fd, buf, chunk, pos);
        if (got <= 0) return 0;
        for (ssize_t i = got - 1; i >= 0; i--) {
            if (buf[i] == '\n' && ++seen == lines) return pos + i + 1;
        }
    }
    return 0;
}

static const char logs_usage[] = "usage: backmux logs <target> [--follow|-f] [--tail|-n N]\n";

/* backmux logs: the process's captured output as plain bytes. -n N starts at the
 * newest N lines; -f keeps following until the process ends (Ctrl-C stops). */
int hold_cmd_view_action(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL;
    bool follow = false;
    long tail = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        if (!strcmp(a, "--follow") || !strcmp(a, "-f")) {
            follow = true;
        } else if (!strcmp(a, "--tail") || !strcmp(a, "-n")) {
            if (++i >= argc) {
                fputs(logs_usage, stderr);
                return HOLD_EXIT_USAGE;
            }
            v = argv[i];
        } else if (!strncmp(a, "--tail=", 7)) {
            v = a + 7;
        } else if (!strcmp(a, "--plain") || !strcmp(a, "--print") || !strcmp(a, "-p")) {
            /* output is always plain; accepted for older scripts */
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            return HOLD_RC_SHOW_HELP;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "backmux: error: unknown flag '%s'\n%s", a, logs_usage);
            return HOLD_EXIT_USAGE;
        } else if (!token) {
            token = a;
        } else {
            fputs(logs_usage, stderr);
            return HOLD_EXIT_USAGE;
        }
        if (v) {
            char *end = NULL;
            errno = 0;
            tail = strtol(v, &end, 10);
            if (!*v || *end || errno || tail < 0) {
                fprintf(stderr, "backmux: error: invalid log line limit\n");
                return HOLD_EXIT_USAGE;
            }
        }
    }
    if (!token) {
        fputs(logs_usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "view", "log", token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, true, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    bool running = hold_eval_state(&r, hold_boot_id_or_null(boot)) == STATE_RUNNING;
    if (tail >= 0) {
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) hold_die_errno("backmux: failed to open log");
        off_t from = tail == 0 ? lseek(fd, 0, SEEK_END) : offset_of_last_lines(fd, tail);
        char buf[8192];
        ssize_t got;
        while ((got = pread(fd, buf, sizeof(buf), from)) > 0) {
            if (hold_write_all(STDOUT_FILENO, buf, (size_t)got) != 0) hold_die_errno("backmux: failed writing log");
            from += got;
        }
        close(fd);
        rc = follow ? hold_tail_log_until_exit(&r, true, running) : 0;
    } else {
        rc = hold_tail_log_until_exit(&r, false, follow && running);
    }
    hold_free_run_record(&r);
    return rc;
}
