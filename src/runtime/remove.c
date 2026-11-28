/* runtime/remove.c - removing named managed processes: the removal behind the
 * native delete, and the Docker-style rm.
 *
 * Removing a process removes its record, log, saved screens and message
 * history, and frees its name. purge is a different operation (a sweep of
 * the whole store, list.c); it never takes a name. */
#include "backmux/config.h"
#include <signal.h>
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"

/* The name to report a process by: its name, else its short ID. */
static void label_for(const char *record_dir, const char *id, char *out, size_t n) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    char display[ID_DISPLAY_HEX_LEN + 1];
    if (hold_load_record_by_id(record_dir, id, &r, path, sizeof(path)) != 0) {
        snprintf(out, n, "%s", hold_run_id_display(id, display));
        return;
    }
    snprintf(out, n, "%s", r.has_name && r.name[0] ? r.name : hold_run_id_display(r.id, display));
    hold_free_run_record(&r);
}

int hold_remove_target(const struct hold_invocation *inv, const struct hold_store *user_store,
                       const struct hold_store *system_store, const char *token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(token);
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        char label[ALIAS_MAX_LEN + 1];
        label_for(targets[i].store.record_dir, targets[i].id, label, sizeof(label));
        struct hold_run_record r;
        char rp[HOLD_PATH_MAX];
        bool running = false;
        if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) == 0) {
            running = hold_eval_state(&r, boot_id) == STATE_RUNNING;
            hold_free_run_record(&r);
        }
        /* delete refuses a process it finds running; one found running on
         * this second look (relaunched in between) is ended before removal,
         * never removed while it runs. */
        if (running) {
            char *one[1] = {targets[i].id};
            int stop_rc = hold_cmd_signal_action(inv, user_store, system_store, "stop", 1, one, SIGTERM, true, false, false);
            if (stop_rc != 0) {
                if (stop_rc > worst) worst = stop_rc;
                continue;
            }
        }
        bool removed = false;
        rc = hold_prune_one_run_ex(&targets[i].store, targets[i].id, boot_id, true, true, &removed);
        if (removed) hold_sig_note(inv, "Removed %s.\n", label);
        if (rc > worst) worst = rc;
    }
    free(targets);
    return worst;
}

/* One rm operand. Docker's contract: a running process is refused unless
 * forced; forced, it is killed (SIGKILL, as docker rm -f) and awaited, then
 * removed. A saved process is removed like any other: saving protects a
 * process from purge, and rm names what it removes. Returns 0 when every
 * process the operand names was removed, 1 on a failure (reported), and 2
 * when a forced rm skipped an operand that names nothing. */
static int rm_one(const struct hold_invocation *inv, const struct hold_store *user_store,
                  const struct hold_store *system_store, const char *operand, bool force,
                  const char *boot_id) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token_quiet(inv, user_store, system_store, "inspect", operand, &targets, &ntargets);
    if (rc == 5 || (rc == 0 && ntargets == 0)) {
        free(targets);
        /* docker rm -f skips a name that matches nothing, silently */
        if (force) return 2;
        fprintf(stderr, "Error: No such managed process: %s\n", operand);
        return 1;
    }
    if (rc != 0) { /* ambiguity and the like, already reported */
        free(targets);
        return 1;
    }
    struct hold_invocation quiet = *inv;
    quiet.quiet = true;
    int failed = 0;
    for (int i = 0; i < ntargets && !failed; i++) {
        if (targets[i].requires_root) {
            (void)hold_report_requires_root(operand);
            failed = 1;
            break;
        }
        struct hold_run_record r;
        char rp[HOLD_PATH_MAX];
        if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) != 0) {
            if (!force) {
                fprintf(stderr, "Error: No such managed process: %s\n", operand);
                failed = 1;
            }
            continue;
        }
        enum run_state st = hold_eval_state(&r, boot_id);
        hold_free_run_record(&r);
        if (!force && (st == STATE_RUNNING || st == STATE_UNKNOWN)) {
            fprintf(stderr,
                    "Error: cannot remove managed process \"%s\": %s: stop the process before removing or force remove\n",
                    operand, st == STATE_RUNNING ? "process is running" : "its state cannot be determined");
            failed = 1;
            break;
        }
        if (st == STATE_RUNNING) {
            char *one[1] = {targets[i].id};
            /* graceful=true here only means "wait until the group is gone":
             * the signal sent is SIGKILL from the start. */
            if (hold_cmd_signal_action(&quiet, user_store, system_store, "kill", 1, one, SIGKILL, true, false, false) != 0) {
                failed = 1;
                break;
            }
        }
        bool removed = false;
        if (hold_prune_one_run_ex(&targets[i].store, targets[i].id, boot_id, true, force, &removed) != 0 || !removed)
            failed = 1;
    }
    free(targets);
    return failed;
}

int hold_cmd_rm_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                       const struct hold_store *system_store, int argc, char **argv, bool force) {
    if (argc == 0) {
        /* docker rm's own message for a missing operand (cobra's
         * RequiresMinArgs in docker/cli), exit 1. */
        fprintf(stderr,
                "backmux: 'backmux rm' requires at least 1 argument\n\n"
                "Usage:  backmux rm [OPTIONS] TARGET [TARGET...]\n\n"
                "See 'backmux rm --help' for more information\n");
        return 1;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    int failed = 0;
    /* Each operand on its own, as docker rm a b c does: one that fails is
     * reported on stderr and the rest still go; each removed operand is
     * printed on stdout as it was typed; any failure makes the exit 1. */
    for (int i = 0; i < argc; i++) {
        int rc = rm_one(inv, user_store, system_store, argv[i], force, boot_id);
        if (rc == 0) {
            printf("%s\n", argv[i]);
            fflush(stdout);
        } else if (rc == 1) {
            failed = 1;
        }
    }
    return failed;
}
