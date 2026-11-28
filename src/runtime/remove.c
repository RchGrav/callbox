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
        rc = hold_prune_one_run_ex(&targets[i].store, targets[i].id, boot_id, true, true, "delete", &removed);
        if (removed) hold_sig_note(inv, "Removed %s.\n", label);
        if (rc > worst) worst = rc;
    }
    free(targets);
    return worst;
}

/* Docker's lookup, not the native resolver's: an exact full ID, else an
 * exact name, else an ID prefix that fits one process (moby's GetContainer).
 * No name prefixes, places or polite names: a script written for docker rm
 * must remove exactly what docker rm would. Searched: the caller's store, and
 * the root-managed store for root. */
enum rm_lookup { RM_FOUND, RM_NONE, RM_AMBIGUOUS };

struct rm_walk {
    const char *operand;
    size_t len;
    bool hex;
    const struct hold_store *store;
    int exact_id, exact_name, prefix;
    struct hold_resolved_target id_hit, name_hit, prefix_hit;
};

static int rm_walk_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct rm_walk *w = ctx;
    struct hold_resolved_target t = {0};
    t.store = *w->store;
    snprintf(t.id, sizeof(t.id), "%s", id);
    if (!strcmp(id, w->operand)) {
        w->exact_id++;
        w->id_hit = t;
    } else if (r && r->has_name && !strcmp(r->name, w->operand)) {
        w->exact_name++;
        w->name_hit = t;
    } else if (w->hex && !strncmp(id, w->operand, w->len)) {
        w->prefix++;
        w->prefix_hit = t;
    }
    return 0;
}

static enum rm_lookup rm_lookup(const struct hold_invocation *inv, const struct hold_store *user_store,
                                const struct hold_store *system_store, const char *operand,
                                struct hold_resolved_target *out) {
    struct rm_walk w = {.operand = operand, .len = strlen(operand), .hex = true};
    for (const char *p = operand; *p; p++) w.hex = w.hex && isxdigit((unsigned char)*p);
    const struct hold_store *stores[2] = {user_store, inv->euid_root ? system_store : NULL};
    for (int i = 0; i < 2; i++) {
        if (!stores[i]) continue;
        w.store = stores[i];
        (void)hold_for_each_record(stores[i]->record_dir, rm_walk_cb, &w);
    }
    if (w.exact_id == 1) return *out = w.id_hit, RM_FOUND;
    if (w.exact_name == 1) return *out = w.name_hit, RM_FOUND;
    if (w.exact_id > 1 || w.exact_name > 1 || w.prefix > 1) return RM_AMBIGUOUS;
    if (w.prefix == 1) return *out = w.prefix_hit, RM_FOUND;
    return RM_NONE;
}

/* One rm operand, in Docker's contract: a running process is refused unless
 * forced; forced, it is killed (SIGKILL, as docker rm -f) and awaited, then
 * removed. A saved process is removed like any other: saving protects a
 * process from purge, and rm names what it removes. Returns 0 removed, 1 a
 * failure (reported on stderr), 2 a forced rm skipping a name that matches
 * nothing. Only "no such process" is skipped under -f, as docker rm -f skips
 * only not-found: an empty or ambiguous operand is an error either way. */
static int rm_one(const struct hold_invocation *inv, const struct hold_store *user_store,
                  const struct hold_store *system_store, const char *typed, bool force,
                  const char *boot_id) {
    /* docker rm trims the slashes off a name ("/web" is web) */
    char operand[HOLD_PATH_MAX];
    const char *s = typed;
    while (*s == '/') s++;
    snprintf(operand, sizeof(operand), "%s", s);
    for (size_t n = strlen(operand); n > 0 && operand[n - 1] == '/'; n--) operand[n - 1] = '\0';
    if (!operand[0]) {
        fprintf(stderr, "Error: managed process name cannot be empty\n");
        return 1;
    }
    struct hold_resolved_target t;
    switch (rm_lookup(inv, user_store, system_store, operand, &t)) {
    case RM_NONE:
        if (force) return 2;
        fprintf(stderr, "Error: No such managed process: %s\n", operand);
        return 1;
    case RM_AMBIGUOUS:
        fprintf(stderr, "Error: multiple IDs found with provided prefix: %s\n", operand);
        return 1;
    case RM_FOUND:
        break;
    }
    if (t.store.kind == STORE_SYSTEM_MANAGED && !inv->euid_root) {
        (void)hold_report_requires_root(operand);
        return 1;
    }
    struct hold_run_record r;
    char rp[HOLD_PATH_MAX];
    if (hold_load_record_by_id(t.store.record_dir, t.id, &r, rp, sizeof(rp)) != 0) {
        if (force) return 2;
        fprintf(stderr, "Error: No such managed process: %s\n", operand);
        return 1;
    }
    enum run_state st = hold_eval_state(&r, boot_id);
    hold_free_run_record(&r);
    if (!force && (st == STATE_RUNNING || st == STATE_UNKNOWN)) {
        fprintf(stderr,
                "Error: cannot remove managed process \"%s\": %s: stop the process before removing or force remove\n",
                operand, st == STATE_RUNNING ? "process is running" : "its state cannot be determined");
        return 1;
    }
    if (st == STATE_RUNNING) {
        struct hold_invocation quiet = *inv;
        quiet.quiet = true;
        char *one[1] = {t.id};
        /* graceful=true here only means "wait until the group is gone": the
         * signal sent is SIGKILL from the start. */
        if (hold_cmd_signal_action(&quiet, user_store, system_store, "kill", 1, one, SIGKILL, true, false, false) != 0) {
            fprintf(stderr, "Error: cannot remove managed process \"%s\": it could not be stopped\n", operand);
            return 1;
        }
    }
    bool removed = false;
    int rc = hold_prune_one_run_ex(&t.store, t.id, boot_id, true, force, NULL, &removed);
    if (rc == 0 && removed) return 0;
    if (rc == 4)
        fprintf(stderr, "Error: cannot remove managed process \"%s\": its record could not be removed: %s\n", operand,
                strerror(errno));
    else if (rc == 2)
        fprintf(stderr, "Error: cannot remove managed process \"%s\": process is running: stop the process before "
                        "removing or force remove\n", operand);
    else
        fprintf(stderr, "Error: No such managed process: %s\n", operand);
    return 1;
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
