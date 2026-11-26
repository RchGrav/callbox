#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/help_text.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/access.h"

/* How a person refers to a process: its name, else the short id. */
static void call_label(const struct hold_run_record *r, char *out, size_t n) {
    if (r->has_name && r->name[0]) snprintf(out, n, "%s", r->name);
    else snprintf(out, n, "%.12s", r->id);
}

struct session_owner {
    pid_t sid;
    char label[ALIAS_MAX_LEN + 1];
};

static int match_session(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)id;
    (void)path;
    struct session_owner *o = ctx;
    if (!r || r->sid != o->sid || r->has_exit_code) return 0;
    call_label(r, o->label, sizeof(o->label));
    return 1;
}

/* Match the attached client's session ID to a managed process when possible,
 * so the diagnostic can identify a nested attachment. */
void hold_note_attached_elsewhere(const char *label, pid_t by, const struct hold_store *a, const struct hold_store *b) {
    struct session_owner o = {.sid = by > 0 ? getsid(by) : -1};
    const struct hold_store *stores[2] = {a, b};
    for (int i = 0; i < 2 && o.sid > 0 && !o.label[0]; i++)
        if (stores[i] && stores[i]->record_dir[0]) (void)hold_for_each_record(stores[i]->record_dir, match_session, &o);
    if (o.label[0])
        fprintf(stderr, "backmux: %s is already attached, from inside the process %s (PID %ld). Detach it there: Ctrl-Space q.\n",
                label, o.label, (long)by);
    else if (by > 0)
        fprintf(stderr, "backmux: %s is already attached in another terminal (PID %ld). Detach it there: Ctrl-Space q.\n",
                label, (long)by);
    else
        fprintf(stderr, "backmux: %s is already attached in another terminal. Detach it there: Ctrl-Space q.\n", label);
}

bool hold_call_still_running(const struct hold_store *store, const char *id) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX], boot[128];
    if (!store || hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return false;
    bool running = hold_eval_state(&r, hold_boot_id_or_null(boot)) == STATE_RUNNING;
    hold_free_run_record(&r);
    return running;
}

static void nap_ms(long ms) {
    struct timespec sl = {.tv_sec = 0, .tv_nsec = ms * 1000000L};
    while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
        continue;
    }
}

/* An ended TTY process's broker records its exit and then saves the complete
 * last screen: wait (a moment) for the record, quietly - a process removed
 * meanwhile (--rm) has nothing more to wait for. */
static void await_exit_record(const struct hold_store *store, const char *id) {
    for (int i = 0; i < 30; i++) {
        struct hold_run_record r;
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return;
        bool marked = r.has_exit_code;
        hold_free_run_record(&r);
        if (marked) return;
        nap_ms(100);
    }
}

/* The broker stamps the program's exit in the record when it reaps it. A
 * stamped process has ended even when something it started lives on in its
 * process group (a background child on pipes keeps the group alive, and no
 * SIGHUP clears it). */
static enum run_state program_state(const struct hold_store *store, const char *id) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX], boot[128];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        int code;
        return hold_auto_removed_exit(store, id, &code) ? STATE_EXITED : STATE_UNKNOWN;
    }
    enum run_state st = r.has_exit_code ? STATE_EXITED : hold_eval_state(&r, hold_boot_id_or_null(boot));
    hold_free_run_record(&r);
    return st;
}

static bool program_ended(enum run_state st) {
    return st == STATE_EXITED || st == STATE_STALE || st == STATE_FAILED;
}

static enum run_state after_attach(const struct hold_invocation *inv, const struct hold_store *store,
                                  const struct hold_run_record *r, const char *label, bool on_terminal, bool unseen,
                                  bool *console_closed) {
    struct stat st;
    enum run_state state = program_state(store, r->id);
    /* The broker removes the console's endpoint as soon as no more output
     * can come: the program ended, or closed its terminal and runs on. */
    bool closed = unseen || program_ended(state) || (lstat(r->console_sock, &st) != 0 && errno == ENOENT);
    /* A console that closed under a running program is most often a program
     * on its way out: give it a moment to be gone before saying it runs on. */
    for (int i = 0; !program_ended(state) && closed && i < 10; i++) {
        nap_ms(100);
        state = program_state(store, r->id);
    }
    if (program_ended(state)) await_exit_record(store, r->id);
    if (closed) {
        /* Inline, the last screen is on the person's terminal already. */
        if (on_terminal && !hold_console_was_inline()) (void)hold_print_final_screen(store, r->id, stdout);
        else if (unseen) (void)hold_print_run_log(r);
    }
    if (state == STATE_RUNNING && closed)
        hold_sig_note(inv, "backmux: %s is still running but its console is unavailable; see 'backmux logs %s'\n",
                      label, label);
    /* A detach says so, in one line: inline, the screen stays as it was,
     * and without the line nothing shows that anything happened. */
    else if (state == STATE_RUNNING && !closed && hold_console_was_detached(NULL)) {
        const char *names = "";
        (void)hold_console_was_detached(&names);
        hold_sig_note(inv, "backmux: detached %s\n", names[0] ? names : label);
    } else if (state == STATE_UNKNOWN && closed)
        hold_sig_note(inv, "backmux: cannot reach %s's console; its process state is unknown - see 'backmux logs %s'\n",
                      label, label);
    else if (state == STATE_UNKNOWN)
        hold_sig_note(inv, "backmux: detached from %s; its process state is unknown here. Reconnect: backmux attach %s\n",
                      label, label);
    if (console_closed) *console_closed = closed;
    return state;
}

bool hold_after_attach(const struct hold_invocation *inv, const struct hold_store *store,
                       const struct hold_run_record *r, const char *label, bool on_terminal, bool unseen) {
    return program_ended(after_attach(inv, store, r, label, on_terminal, unseen, NULL));
}


static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_run_record *r,
                                 enum run_state st,
                                 const struct hold_store *user_store,
                                 const struct hold_store *system_store) {
    char label[ALIAS_MAX_LEN + 1];
    call_label(r, label, sizeof(label));
    /* How an attach ended, as its exit code - so a script that attached can
     * tell what it came back to: 0 detached without a confirmed exit; 8 the process
     * has ended (before or while attached; the same "ended" as wait's 8);
     * 3 unreachable or refused (attached elsewhere, or this is that process); 10 the process has
     * no console to attach to (the same "wrong kind of process" as peek's). */
    if (program_ended(st)) {
        hold_sig_note(inv, "backmux: %s has ended - see 'backmux logs %s'\n", label, label);
        return 8;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "backmux: %s has no console (start with -it)\n", label);
        return HOLD_EXIT_NO_SCREEN;
    }
    /* Attaching to the process this very command runs in would show a screen
     * inside itself. */
    const char *here = getenv("BACKMUX_ID");
    if (here && !strcmp(here, r->id)) {
        fprintf(stderr, "backmux: cannot attach to %s from inside the same managed process\n", label);
        return 3;
    }
    /* Retained broker-chat profiles interpret keys in the broker; other
     * attachments interpret them in the client. */
    hold_console_set_composited(r->recipe.mode_chat && r->recipe.has_keys);
    /* A process with a console but no PTY runs on pipes (backmux -i): attach in
     * line mode. */
    hold_console_set_line_mode(!r->recipe.mode_tty);
    /* A process on pipes printed its output into this terminal as it went:
     * there is no screen to print again when it ends. */
    bool on_terminal = r->recipe.mode_tty && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    hold_prepare_mux(inv, store);
    int rc = hold_run_native_console(r->console_sock);
    pid_t by = 0;
    if (hold_console_was_refused(&by)) {
        hold_note_attached_elsewhere(label, by, user_store, system_store);
        rc = 3;
    } else if (hold_console_was_gone()) {
        /* Its console closed between the look at its record and the attach. */
        enum run_state state = after_attach(inv, store, r, label, on_terminal, true, NULL);
        bool ended = program_ended(state);
        if (ended) hold_sig_note(inv, "backmux: %s has ended - see 'backmux logs %s'\n", label, label);
        rc = ended ? 8 : state == STATE_UNKNOWN ? 3 : 0;
    } else if (rc == 0) {
        bool closed = false;
        enum run_state state = after_attach(inv, store, r, label, on_terminal, false, &closed);
        rc = program_ended(state) ? 8 : state == STATE_UNKNOWN && closed ? 3 : 0;
    }
    return rc;
}

void hold_note_enclosing_call(const struct hold_store *a, const struct hold_store *b) {
    const char *id = getenv("BACKMUX_ID");
    const struct hold_store *stores[2] = {a, b};
    if (!id || !hold_valid_id(id)) return;
    for (int i = 0; i < 2; i++) {
        char path[HOLD_PATH_MAX];
        struct stat st;
        if (!stores[i] || !stores[i]->console_dir[0]) continue;
        if (hold_format_console_sock_path(stores[i], id, path, sizeof(path)) == 0 && lstat(path, &st) == 0) {
            hold_console_set_enclosing(path);
            return;
        }
    }
}

int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "backmux: nothing to attach\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    hold_note_enclosing_call(user_store, system_store);
    rc = attach_console_record(inv, &target.store, &r, st, user_store, system_store);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}

void hold_usage(void) {
    /* Keep the introduction in the compiled plain-text overview. */
    printf("backmux %s\n", HOLD_VERSION);
    (void)hold_help_print("overview");
}
