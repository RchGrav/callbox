#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/names/adjectives.h"
#include "backmux/names/nouns.h"

/* Process-record operations split out of start.c: redial resolution, save, rename,
 * and generated process-name assignment. The start machinery (spawn, log capture,
 * restart supervisor) stays in start.c; these share only hold_perform_start_options
 * and the public runtime API. */

struct name_exists_walk {
    const char *name;
    const char *ignore_id;
};

static int name_exists_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)path;
    const struct name_exists_walk *w = vctx;
    if (w->ignore_id && strcmp(id, w->ignore_id) == 0) return 0;
    return r && r->has_name && strcmp(r->name, w->name) == 0;
}

/* Pending launches reserve names in .<id>.reserve under the store lock.
 * Reservations modified less than 600 seconds ago block name reuse. */
static bool name_reserved_in_store(const struct hold_store *store, const char *name, const char *ignore_id) {
    DIR *d = opendir(store->record_dir);
    if (!d) return false;
    bool taken = false;
    struct dirent *de;
    size_t suffix = strlen(".reserve");
    while (!taken && (de = readdir(d))) {
        size_t len = strlen(de->d_name);
        if (de->d_name[0] != '.' || len != 1 + ID_HEX_LEN + suffix || strcmp(de->d_name + 1 + ID_HEX_LEN, ".reserve"))
            continue;
        if (ignore_id && !strncmp(de->d_name + 1, ignore_id, ID_HEX_LEN)) continue;
        char path[HOLD_PATH_MAX], held[ALIAS_MAX_LEN + 2];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, de->d_name) != 0) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct stat st;
        ssize_t n = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && time(NULL) - st.st_mtime < 600)
                        ? read(fd, held, sizeof(held) - 1) : -1;
        close(fd);
        if (n <= 0) continue;
        held[n] = '\0';
        taken = !strcmp(held, name);
    }
    closedir(d);
    return taken;
}

static int run_name_exists_in_store(const struct hold_store *store, const char *name, const char *ignore_id) {
    if (!store || !hold_valid_alias(name)) return 0;
    struct name_exists_walk w = { name, ignore_id };
    int rc = hold_for_each_record(store->record_dir, name_exists_cb, &w);
    if (rc == 0 && name_reserved_in_store(store, name, ignore_id)) rc = 1;
    if (rc == 0 && hold_inbox_live(store, name)) rc = 1;
    return rc;
}

/* Claims NAME for the process mid-launch: written into its .<id>.reserve, which
 * the record write removes. */
static void reserve_run_name(const struct hold_store *store, const char *id, const char *name) {
    char path[HOLD_PATH_MAX];
    if (!id || hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) != 0) return;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    (void)hold_write_all(fd, name, strlen(name));
    close(fd);
}

struct restart_walk {
    const char *token;
    bool id_like;
    bool name_like;
    int matches;
    char *out;
};

static int restart_match_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)path;
    struct restart_walk *w = vctx;
    bool hit = w->id_like && strncmp(id, w->token, strlen(w->token)) == 0;
    if (!hit && w->name_like) hit = r && r->has_name && strcmp(r->name, w->token) == 0;
    if (!hit) return 0;
    w->matches++;
    return hold_checked_snprintf(w->out, ID_STR_LEN, "%s", id) != 0 ? -1 : 0;
}

static int find_restart_record(const struct hold_store *store, const char *token, char out[ID_STR_LEN]) {
    if (!store || !token || !*token || !out) return 0;
    struct restart_walk w = { token, hold_valid_id_prefix(token), hold_valid_alias(token), 0, out };
    if (!w.id_like && !w.name_like) return 0;
    if (hold_for_each_record(store->record_dir, restart_match_cb, &w) != 0) return -1;
    if (w.matches == 0) return 0;
    if (w.matches > 1) {
        (void)hold_report_ambiguous(store, token, false);
        return -2;
    }
    return 1;
}

static int restart_existing_run(const struct hold_invocation *inv,
                                const struct hold_store *store,
                                bool tail,
                                bool console_mode,
                                bool auto_remove,
                                bool wait,
                                bool interactive_stdin,
                                bool explicit_session_mode,
                                const char *restart_policy,
                                int restart_delay_seconds,
                                int size_cols,
                                int size_rows,
                                const char *id) {
    struct hold_run_record old;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &old, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    /* Redial honors the recipe's recorded session mode: a -it recipe reattaches,
     * a -d recipe detaches (printing the id), a foreground recipe streams. An
     * explicit mode flag on the redial invocation overrides the recipe. */
    if (!explicit_session_mode) {
        tail = !old.recipe.mode_detach;
        console_mode = old.recipe.mode_tty;
        interactive_stdin = old.recipe.mode_interactive;
        wait = wait || old.recipe.mode_wait;
    }
    /* Retained broker-chat profiles default to an attached PTY. */
    if (old.recipe.mode_chat && !explicit_session_mode) {
        tail = true;
        console_mode = true;
        interactive_stdin = false;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&old, boot_id);
    if (st == STATE_RUNNING) {
        char display[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(old.id, display);
        fprintf(stderr, "backmux: error: process %s is already running\n", display);
        hold_free_run_record(&old);
        return 6;
    }
    if (!old.has_log || !old.log_path[0]) {
        fprintf(stderr, "backmux: error: process %s has no retained log path\n", id);
        hold_free_run_record(&old);
        return 5;
    }
    char *j = NULL;
    char **argv = NULL;
    int argc = 0;
    int rc = 0;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0 ||
        hold_json_get_argv_alloc(j, &argv, &argc) != 0) {
        fprintf(stderr, "backmux: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "backmux: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    /* Recall uses the recorded working directory unless that setting was unset. */
    if (old.recipe.has_working && chdir(old.recipe.working) != 0) {
        fprintf(stderr, "backmux: error: cannot start %s in %s: %s\n"
                        "  backmux set %s working=DIR changes the directory; backmux unset %s working uses the current directory\n",
                old.has_name ? old.name : id, old.recipe.working, strerror(errno),
                old.has_name ? old.name : id, old.has_name ? old.name : id);
        rc = 5;
        goto out;
    }
    struct hold_start_options opts = {
        .no_working = !old.recipe.has_working,
        .tail = tail,
        .console_mode = console_mode,
        .auto_remove = auto_remove,
        .wait = wait,
        .interactive_stdin = interactive_stdin,
        .argc = argc,
        .argv = argv,
        .exec_path = argv[0],
        .envc = old.recipe.envc,
        .env = old.recipe.env,
        .restart_policy = restart_policy ? restart_policy : (old.recipe.has_restart_policy ? old.recipe.restart_policy : NULL),
        .restart_delay_seconds = restart_delay_seconds != HOLD_RESTART_BACKOFF
                                     ? restart_delay_seconds
                                     : (!restart_policy && old.recipe.has_restart_delay ? old.recipe.restart_delay_seconds
                                                                                        : HOLD_RESTART_BACKOFF),
        /* The recipe's --size replays (on a PTY again) unless this
         * invocation gave its own. */
        .size_cols = size_cols ? size_cols : (console_mode && old.recipe.has_size ? old.recipe.size_cols : 0),
        .size_rows = size_cols ? size_rows : (console_mode && old.recipe.has_size ? old.recipe.size_rows : 0),
        /* The chat window needs a terminal to be drawn on: redialed from a
         * script, a user-mode process comes back attached without it. */
        .chat = old.recipe.mode_chat && !explicit_session_mode && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO),
        /* the intent survives a redial from a script, where chat cannot be drawn */
        .user_mode = old.recipe.mode_chat && !explicit_session_mode,
        .shell = old.recipe.mode_shell,
        .chat_key = old.recipe.has_chat_key ? (unsigned char)old.recipe.chat_key : 0,
        .existing_id = old.id,
        .existing_log_path = old.log_path,
        .existing_run_name = old.has_name ? old.name : NULL,
        .existing_created_unix_ns = old.created_unix_ns,
        .existing_created_at = old.has_created_at ? old.created_at : NULL,
    };
    rc = hold_perform_start_options(inv, store, &opts);
out:
    hold_free_argv_alloc(argv, argc);
    free(j);
    hold_free_run_record(&old);
    return rc;
}

int hold_cmd_redial(const struct hold_invocation *inv,
                      const struct hold_store *user_store,
                      const struct hold_store *system_store,
                      bool tail,
                      bool console_mode,
                      bool auto_remove,
                      bool wait,
                      bool interactive_stdin,
                      bool explicit_session_mode,
                      const char *restart_policy,
                      int restart_delay_seconds,
                      int size_cols,
                      int size_rows,
                      const char *token,
                      bool *redialed) {
    *redialed = false;
    char restart_id[ID_STR_LEN];
    const struct hold_store *restart_store = NULL;
    int match = 0;
    if (user_store && user_store->record_dir[0]) {
        match = find_restart_record(user_store, token, restart_id);
        if (match == 1) restart_store = user_store;
    }
    if (match == 0 && system_store && system_store->record_dir[0] && inv->euid_root) {
        match = find_restart_record(system_store, token, restart_id);
        if (match == 1) restart_store = system_store;
    }
    if (match == -2) {
        *redialed = true;
        return 6;
    }
    if (match < 0) {
        *redialed = true;
        return 3;
    }
    if (!restart_store) {
        /* Not a retained process; the caller launches it as a command instead. */
        return 0;
    }
    *redialed = true;
    /* A running target is attached when it has a console, otherwise its log
     * is followed. An explicit detached invocation returns its ID instead
     * of starting another instance. */
    struct hold_run_record cur;
    char cur_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(restart_store->record_dir, restart_id, &cur, cur_path, sizeof(cur_path)) == 0) {
        char boot[128];
        bool running = hold_eval_state(&cur, hold_boot_id_or_null(boot)) == STATE_RUNNING;
        bool console = cur.has_console;
        char display[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(cur.id, display);
        hold_free_run_record(&cur);
        if (running) {
            if (explicit_session_mode && !tail) {
                printf("%s\n", restart_id);
                fprintf(stderr, "backmux: %s is already running\n", token);
                return 0;
            }
            return console ? hold_cmd_console_action(inv, user_store, system_store, restart_id)
                           : hold_cmd_tail_action(inv, user_store, system_store, restart_id);
        }
    }
    return restart_existing_run(inv, restart_store, tail, console_mode, auto_remove, wait,
                                interactive_stdin, explicit_session_mode, restart_policy,
                                restart_delay_seconds, size_cols, size_rows, restart_id);
}

/* Resolve and atomically update the saved flag and optional name, then refresh
 * the public index for system-managed records. new_name==NULL saves the record;
 * a non-NULL name renames it and sets the saved flag. */
static int protect_record_action(const struct hold_invocation *inv,
                                 const struct hold_store *user_store,
                                 const struct hold_store *system_store,
                                 const char *target_token,
                                 const char *new_name) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", target_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(target_token);
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    /* Saving keeps the terminal as it is now, not only as it was launched.
     * Before the lock below: the refresh takes it itself (never nested). */
    (void)hold_refresh_terminal(target.store.record_dir, target.id);
    /* The store lock serializes this read-modify-write with the exit stamp
     * (hold_mark_run_finished), which otherwise could rewrite the record
     * from a copy read before this save and drop the saved flag or name. */
    int lock_fd = hold_store_lock(target.store.record_dir);
    if (new_name && run_name_exists_in_store(&target.store, new_name, target.id)) {
        fprintf(stderr, "backmux: error: process name '%s' already exists\n", new_name);
        hold_store_unlock(lock_fd);
        free(targets);
        return HOLD_EXIT_NAME_TAKEN;
    }
    struct hold_run_record r;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        hold_store_unlock(lock_fd);
        free(targets);
        return 5;
    }
    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    /* Saving is idempotent: an already-saved process needs no rewrite, but still
     * confirms the protected state on stderr and exits 0. */
    if (!new_name && r.saved) {
        hold_store_unlock(lock_fd);
        hold_sig_note(inv, "backmux: saved %s (%s)\n", display_id, r.has_name ? r.name : display_id);
        hold_free_run_record(&r);
        free(targets);
        return 0;
    }
    char *j = NULL;
    char **argv = NULL;
    int argc = 0;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0 ||
        hold_json_get_argv_alloc(j, &argv, &argc) != 0) {
        fprintf(stderr, "backmux: error: failed to load process record for %s\n", target.id);
        hold_store_unlock(lock_fd);
        free(j);
        hold_free_run_record(&r);
        free(targets);
        return 5;
    }
    bool newly_saved = !r.saved;
    if (new_name) {
        snprintf(r.name, sizeof(r.name), "%s", new_name);
        r.has_name = true;
    }
    r.saved = true;
    char out_path[HOLD_PATH_MAX];
    if (hold_write_record_atomic(target.store.record_dir, &r, argc, argv, out_path, sizeof(out_path)) != 0) {
        hold_die_errno(new_name ? "backmux: failed to write renamed process record"
                                : "backmux: failed to write saved process record");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        (void)hold_write_public_index_atomic(&target.store, &r, NULL);
    }
    hold_store_unlock(lock_fd);
    if (new_name) {
        hold_sig_note(inv, "backmux: renamed %s to %s%s\n", display_id, new_name,
                      newly_saved ? " (saved)" : "");
    } else {
        hold_sig_note(inv, "backmux: saved %s (%s)\n", display_id, r.has_name ? r.name : display_id);
    }
    hold_free_argv_alloc(argv, argc);
    free(j);
    hold_free_run_record(&r);
    free(targets);
    return 0;
}

int hold_cmd_rename_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *target_token,
                             const char *new_name) {
    if (!hold_valid_alias(new_name)) {
        fprintf(stderr, "backmux: error: invalid process name '%s'\n", new_name);
        return 5;
    }
    return protect_record_action(inv, user_store, system_store, target_token, new_name);
}

int hold_cmd_save_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *target_token) {
    return protect_record_action(inv, user_store, system_store, target_token, NULL);
}

int hold_pick_run_name_locked(const struct hold_store *store, const char *id, const char *requested,
                              char out[ALIAS_MAX_LEN + 1]) {
    if (requested && *requested) {
        if (!hold_valid_alias(requested)) {
            fprintf(stderr, "backmux: error: invalid process name '%s'\n", requested);
            return 5;
        }
        if (run_name_exists_in_store(store, requested, id)) {
            fprintf(stderr, "backmux: error: process name '%s' already exists\n", requested);
            return HOLD_EXIT_NAME_TAKEN;
        }
        snprintf(out, ALIAS_MAX_LEN + 1, "%s", requested);
        return 0;
    }

    size_t adj_n = adjectives_count;
    size_t noun_n = nouns_count;
    size_t total = adj_n * noun_n;
    /* The word lists are 256 each, so the id's first byte picks the adjective
     * and its second byte the noun: an uncontested name spells the id's first
     * four hex digits. A taken name steps to the next adjective. */
    _Static_assert(sizeof(adjectives) / sizeof(adjectives[0]) == 256, "adjectives must hold 256 words");
    _Static_assert(sizeof(nouns) / sizeof(nouns[0]) == 256, "nouns must hold 256 words");
    unsigned long long seed = 0;
    for (size_t i = 0; id && id[i] && i < 4; i++) {
        unsigned nibble = (unsigned)(id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
        seed |= (unsigned long long)nibble << (i < 2 ? (1 - i) * 4 : 8 + (3 - i) * 4);
    }
    for (size_t tries = 0; tries < total; tries++) {
        unsigned long long candidate = seed + tries;
        size_t adj_idx = (size_t)(candidate % adj_n);
        size_t noun_idx = (size_t)((candidate / adj_n) % noun_n);
        if (hold_checked_snprintf(out,
                                  ALIAS_MAX_LEN + 1,
                                  "%s_%s",
                                  adjectives[adj_idx],
                                  nouns[noun_idx]) != 0) {
            return 3;
        }
        if (!run_name_exists_in_store(store, out, id)) return 0;
    }
    char display[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(id, display);
    return hold_checked_snprintf(out, ALIAS_MAX_LEN + 1, "run_%s", display) == 0 ? 0 : 3;
}

/* The check and the claim happen under the store lock, so two launches at
 * once cannot both take one name. */
int hold_generate_run_name_for_id(const struct hold_store *store, const char *id, const char *requested, char out[ALIAS_MAX_LEN + 1]) {
    int lock_fd = store ? hold_store_lock(store->record_dir) : -1;
    int rc = hold_pick_run_name_locked(store, id, requested, out);
    if (rc == 0 && store) reserve_run_name(store, id, out);
    hold_store_unlock(lock_fd);
    return rc;
}
