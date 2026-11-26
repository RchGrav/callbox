#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/access.h"

static bool target_group_gone(const struct hold_run_record *r);

void hold_report_session_escapees(const struct hold_run_record *r) {
    int escaped = hold_count_session_escapees(r->sid, r->pgid);
    if (escaped > 0) {
        fprintf(stderr,
                "backmux: warning: %d process(es) escaped process-group %ld but remain in session %ld\n",
                escaped, (long)r->pgid, (long)r->sid);
    }
}

static enum run_state eval_local_state(const struct hold_run_record *r, const char *current_boot) {
    if ((r->has_state && strcmp(r->state, "failed") == 0) || (r->has_launch_error && r->launch_error[0] != '\0')) {
        return STATE_FAILED;
    }
    if (r->pgid <= 1 || r->sid <= 0) {
        return STATE_UNKNOWN;
    }
    if (r->has_boot && current_boot && strcmp(r->boot_id, current_boot) != 0) {
        return STATE_STALE;
    }

    char state = 0;
    uint64_t now_starttime = 0;
    bool has_stat = hold_read_proc_stat_tokens(r->pid, &state, &now_starttime) == 0;
    bool present = has_stat || hold_leader_present(r->pid);
    enum group_liveness gl = hold_group_session_liveness(r->pgid, r->sid);

    if (has_stat && state == 'Z') {
        if (gl == GROUP_LIVE) {
            return STATE_RUNNING;
        }
        if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
            return STATE_EXITED;
        }
        return STATE_UNKNOWN;
    }

    if (present) {
        if (r->proc_starttime_ticks && has_stat) {
            if (now_starttime != r->proc_starttime_ticks) {
                return STATE_STALE;
            }
        } else if (r->exe_dev && r->exe_ino) {
            uint64_t d, i;
            if (hold_read_proc_exe(r->pid, &d, &i) == 0 && (d != r->exe_dev || i != r->exe_ino)) {
                return STATE_STALE;
            }
        }
        return STATE_RUNNING;
    }

    if (gl == GROUP_LIVE) {
        return STATE_RUNNING;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return STATE_EXITED;
    }

    int g = hold_group_exists(r->pgid);
    if (g == 0) {
        return STATE_EXITED;
    }
    return STATE_UNKNOWN;
}

/* Under the socket-denied fallback, a process's broker or waiter holds a shared
 * lock on the process's log for as long as it lives. A process in another PID
 * namespace (some sandboxes give each shell its own) cannot see the process's
 * pid, but it shares the filesystem, and the lock is in the kernel: held
 * means the process is alive somewhere. A failed observation is not a released
 * lock: the log may be missing or unreadable while the supervisor runs. */
enum log_lock_state { LOG_LOCK_UNKNOWN, LOG_LOCK_HELD, LOG_LOCK_RELEASED };

static enum log_lock_state probe_log_lock(const struct hold_run_record *r) {
    if (!r->has_log || !r->log_path[0]) return LOG_LOCK_UNKNOWN;
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return LOG_LOCK_UNKNOWN;
    enum log_lock_state state = LOG_LOCK_UNKNOWN;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        state = LOG_LOCK_RELEASED;
        (void)flock(fd, LOCK_UN);
    } else if (errno == EWOULDBLOCK) {
        state = LOG_LOCK_HELD;
    }
    close(fd);
    return state;
}

/* What this process can tell about a process. Launched in another PID namespace,
 * the process's pid means nothing here - it may even name an unrelated process -
 * so it is never consulted: an exit stamp says the process ended, and otherwise
 * its supervisor's lock on the log says whether it still runs (*elsewhere).
 * In the launching namespace (and where there are none, macOS) local
 * evidence decides, as always: a process that just exited is ended even before
 * its waiter stamps it. */
static enum run_state eval_state(const struct hold_run_record *r, const char *current_boot, bool *elsewhere) {
    if (elsewhere) *elsewhere = false;
    uint64_t here = hold_current_pid_ns();
    bool foreign = r->pid_ns != 0 && here != 0 && r->pid_ns != here;
    if (!foreign) return eval_local_state(r, current_boot);
    if ((r->has_state && strcmp(r->state, "failed") == 0) || (r->has_launch_error && r->launch_error[0] != '\0'))
        return STATE_FAILED;
    if (r->has_boot && current_boot && strcmp(r->boot_id, current_boot) != 0) return STATE_STALE;
    if (r->has_exit_code) return STATE_EXITED;
    enum log_lock_state lock = probe_log_lock(r);
    if (lock == LOG_LOCK_HELD) {
        if (elsewhere) *elsewhere = true;
        return STATE_RUNNING;
    }
    /* Only a successfully observed release of a lifetime lock proves exit.
     * An unavailable probe, or a plain -d process closing its own output, tells
     * this namespace nothing about whether the program is still running. */
    return lock == LOG_LOCK_RELEASED && r->liveness_lock ? STATE_EXITED : STATE_UNKNOWN;
}

enum run_state hold_eval_state(const struct hold_run_record *r, const char *current_boot) {
    return eval_state(r, current_boot, NULL);
}

bool hold_call_records_exit(const struct hold_run_record *r) {
    return r->has_console || r->recipe.mode_wait;
}

bool hold_call_elsewhere(const struct hold_run_record *r, const char *current_boot) {
    bool elsewhere = false;
    (void)eval_state(r, current_boot, &elsewhere);
    return elsewhere;
}

void hold_rollback_spawned_group(pid_t pid, pid_t pgid) {
    if (pgid > 1) {
        kill(-pgid, SIGKILL);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

static bool target_group_gone(const struct hold_run_record *r) {
    enum group_liveness gl = hold_group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return true;
    }
    if (gl == GROUP_LIVE) {
        return false;
    }
    return hold_group_exists(r->pgid) == 0;
}

bool hold_wait_target_group_gone(const struct hold_run_record *r, int timeout_ms) {
    int waited = 0;
    while (waited <= timeout_ms) {
        if (target_group_gone(r)) {
            return true;
        }
        if (waited == timeout_ms) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_MS * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
        waited += POLL_SLEEP_MS;
    }
    return false;
}

const char *hold_state_str(enum run_state s) {
    switch (s) {
    case STATE_RUNNING:
        return "running";
    case STATE_EXITED:
        return "exited";
    case STATE_STALE:
        return "stale";
    case STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
