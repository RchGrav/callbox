#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/runtime.h"
#include "callbox/runtime_internal.h"
#include "callbox/core.h"
#include "callbox/platform.h"
#include "callbox/store.h"
#include "callbox/console.h"
#include "callbox/access.h"

/* How a person refers to a call: its name, else the short id. */
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

/* The console is someone else's just now. Say where: a call's program and
 * everything it runs share its session, so the holder's session tells which
 * call (if any) it is attached from inside. */
void hold_note_attached_elsewhere(const char *label, pid_t by, const struct hold_store *a, const struct hold_store *b) {
    struct session_owner o = {.sid = by > 0 ? getsid(by) : -1};
    const struct hold_store *stores[2] = {a, b};
    for (int i = 0; i < 2 && o.sid > 0 && !o.label[0]; i++)
        if (stores[i] && stores[i]->record_dir[0]) (void)hold_for_each_record(stores[i]->record_dir, match_session, &o);
    if (o.label[0])
        fprintf(stderr, "callbox: %s is already attached, from inside the call %s (pid %ld). Detach it there: Ctrl-P Ctrl-Q.\n",
                label, o.label, (long)by);
    else if (by > 0)
        fprintf(stderr, "callbox: %s is already attached in another terminal (pid %ld). Detach it there: Ctrl-P Ctrl-Q.\n",
                label, (long)by);
    else
        fprintf(stderr, "callbox: %s is already attached in another terminal. Detach it there: Ctrl-P Ctrl-Q.\n", label);
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

/* An ended TTY call's broker records its exit and then saves the complete
 * last screen: wait (a moment) for the record, quietly - a call removed
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

bool hold_after_attach(const struct hold_invocation *inv, const struct hold_store *store,
                       const struct hold_run_record *r, const char *label, bool on_terminal, bool unseen) {
    struct stat st;
    bool running = hold_call_still_running(store, r->id);
    /* The broker removes the console's endpoint as soon as no more output
     * can come: the program ended, or closed its terminal and runs on. */
    bool closed = unseen || !running || (lstat(r->console_sock, &st) != 0 && errno == ENOENT);
    /* A console that closed under a running program is most often a program
     * on its way out: give it a moment to be gone before saying it runs on. */
    for (int i = 0; running && closed && i < 10; i++) {
        nap_ms(100);
        running = hold_call_still_running(store, r->id);
    }
    if (!running) await_exit_record(store, r->id);
    if (closed) {
        if (on_terminal) (void)hold_print_final_screen(store, r->id, stdout);
        else if (unseen) (void)hold_tail_log_until_exit(r, false, false);
    }
    if (running && closed)
        hold_sig_note(inv, "callbox: %s has closed its terminal and runs on; there is nothing to attach - see 'callbox logs %s'\n",
                      label, label);
    else if (running) hold_note_detached(inv, label);
    return !running;
}

void hold_note_detached(const struct hold_invocation *inv, const char *label) {
    hold_sig_note(inv, "callbox: detached from %s; it is still running. Pick it back up: callbox attach %s\n", label, label);
}

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_run_record *r,
                                 enum run_state st,
                                 const struct hold_store *user_store,
                                 const struct hold_store *system_store) {
    char label[ALIAS_MAX_LEN + 1];
    call_label(r, label, sizeof(label));
    if (st != STATE_RUNNING) {
        hold_sig_note(inv, "callbox: %s has ended - see 'callbox logs %s'\n", label, label);
        return 0;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "callbox: %s has no console (start with -it)\n", label);
        return 0;
    }
    /* Attaching to the call this very command runs in would show a screen
     * inside itself. */
    const char *here = getenv("CALLBOX_ID");
    if (here && !strcmp(here, r->id)) {
        fprintf(stderr, "callbox: you are inside %s already\n", label);
        return 3;
    }
    /* A callbox shell's broker reads the lead key itself; any other call's keys
     * are read here, by the attach client. */
    hold_console_set_composited(r->recipe.mode_chat && r->recipe.has_keys);
    bool on_terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    int rc = hold_run_native_console(r->console_sock);
    pid_t by = 0;
    if (hold_console_was_refused(&by)) hold_note_attached_elsewhere(label, by, user_store, system_store);
    else if (hold_console_was_gone()) {
        /* Its console closed between the look at its record and the attach. */
        if (hold_after_attach(inv, store, r, label, on_terminal, true))
            hold_sig_note(inv, "callbox: %s has ended - see 'callbox logs %s'\n", label, label);
        rc = 0;
    } else if (rc == 0) (void)hold_after_attach(inv, store, r, label, on_terminal, false);
    return rc;
}

void hold_note_enclosing_call(const struct hold_store *a, const struct hold_store *b) {
    const char *id = getenv("CALLBOX_ID");
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
        hold_sig_note(inv, "callbox: nothing to attach\n");
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
    printf("callbox %s - see, drive, and test TUIs from scripts and AI agents\n\n"
           "Place a command as a durable call: see its screen, type into it, wait on\n"
           "it, list it, pick it back up, end it politely. No daemon, no config server.\n\n"
           "PLACE A CALL\n"
           "  callbox <cmd> [args...]         foreground: stream its output\n"
           "  callbox -d <cmd> [args...]      detached: print the bare 64-hex call id\n"
           "  callbox -dw <cmd> [args...]     ...plus a waiter that records the exit code\n"
           "  callbox -it <cmd> [args...]     attached on a PTY (Ctrl-P Ctrl-Q detaches)\n"
           "  callbox -dt --size 120 40 <cmd> detached on a PTY of that size, to drive\n"
           "  callbox shell [shell args...]   your shell, with a chat window to your agents\n"
           "  callbox <id|name>               redial: restart a call from its recipe\n"
           "  callbox --name web -d <cmd>...  name the call and place it in the background\n\n"
           "DRIVE A TTY CALL (scripts and agents)\n"
           "  callbox peek <target>           the screen as text (--json, --image out.png)\n"
           "  callbox send <target> TEXT      type into it (-k Enter, --paste, --mouse)\n"
           "  callbox wait <target>           until it settles (--idle, --match, exit)\n"
           "  callbox resize <target> 120 40  change its terminal size\n"
           "  callbox msg <target> TEXT       message an agent (it replies the same way)\n\n"
           "MANAGE YOUR CALLS\n"
           "  callbox list                    your ledger: your calls, live and past\n"
           "  callbox ps                      the Docker view: running calls, machine-wide\n"
           "  callbox attach <target>         pick the call back up (Ctrl-P Ctrl-Q detaches)\n"
           "  callbox logs <target> [-f]      print captured output (-n N newest lines)\n"
           "  callbox tail <target>           watch a running call's output from now on\n"
           "  callbox inspect <target>        structured JSON details\n"
           "  callbox ports <target>          listening sockets in use by the call\n"
           "  callbox stats <target>          live CPU, memory, and pid usage\n"
           "  callbox end <target>...         end calls politely: TERM, then KILL\n"
           "  callbox kill <target>...        KILL now, when they won't listen\n"
           "  callbox save <target>           protect a call from purge\n"
           "  callbox rename <target> <name>  name a call; naming it also saves it\n"
           "  callbox purge [<target>]        the removal verb (-a stale, --force saved)\n\n"
           "  target = call id, id prefix, or call name\n\n"
           "MORE\n"
           "  callbox help launch             every flag for placing a call\n"
           "  callbox help targets            id and scope resolution\n"
           "  callbox help scripting          exit codes, --print, --quiet, stdout\n"
           "  callbox config set detach-size 120 40   the size detached TTY calls start at\n"
           "  callbox <command> -h            help for one command\n\n"
           "  callbox --version\n",
           HOLD_VERSION);
}
