#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/access.h"

/* Target resolution: the privilege/scope/intent matrix. The store owns raw
 * id/prefix scanning (hold_resolve_record_id); this file owns WHICH stores a
 * token may reach — root-vs-nonroot × plain/user:/system: — with alias
 * intent, sudo provenance for user:, and ambiguity rc 6. The candidate list
 * is built once and shared by the alias and id-prefix passes (alias wins). */

enum id_token_scope hold_parse_id_token(const char *token, const char **id_out) {
    if (!token || !*token) {
        return ID_TOKEN_INVALID;
    }
    if (strncmp(token, "user:", 5) == 0) {
        *id_out = token + 5;
        return **id_out ? ID_TOKEN_USER : ID_TOKEN_INVALID;
    }
    if (strncmp(token, "system:", 7) == 0) {
        *id_out = token + 7;
        return **id_out ? ID_TOKEN_SYSTEM : ID_TOKEN_INVALID;
    }
    *id_out = token;
    return ID_TOKEN_PLAIN;
}

/* A public-projection id resolves only when the projection is root-managed. */
static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    int rc = hold_resolve_record_id(store->public_dir, id, resolved, n);
    if (rc != 0) {
        return rc;
    }
    struct hold_public_index pi;
    if (hold_load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        /* A console can answer even when this namespace cannot observe its
         * process or log lock. signal_one routes foreign processes to the broker. */
        return st == STATE_RUNNING || (st == STATE_UNKNOWN && r->has_console);
    }
    /* tail of an ended process prints its whole log (backmux help tail), so it
     * finds any process with a log, as logs does - and --tail of an ended process
     * must find it rather than fall through to a redial. */
    if (!strcmp(command, "view") || !strcmp(command, "tail")) {
        return r->has_log;
    }
    /* Screen verbs, and attach, resolve any matching process and then explain
     * what it lacks (no TTY, already ended) rather than claiming nothing
     * matched. */
    if (!strcmp(command, "inspect") || !strcmp(command, "wait") || !strcmp(command, "peek") ||
        !strcmp(command, "send") || !strcmp(command, "resize") || !strcmp(command, "msg") ||
        !strcmp(command, "console")) {
        return true;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static bool hold_record_matches_run_name_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start-existing")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE || st == STATE_RUNNING;
    }
    return hold_record_matches_alias_intent(command, r, st);
}

static int append_resolved_target(struct hold_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct hold_store *store,
                                  const char *id,
                                  bool requires_root) {
    struct hold_resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    struct hold_resolved_target *out = &(*targets)[*count];
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->requires_root = requires_root;
    hold_checked_snprintf(out->id, sizeof(out->id), "%s", id);
    (*count)++;
    return 0;
}

/* ---- several processes match ------------------------------------------------ */

enum ambiguous_kind {
    MATCH_ID_OR_NAME,   /* an id prefix, or a name exactly */
    MATCH_NAME_PREFIX,  /* the start of a name */
};

struct ambiguous_walk {
    const char *token;
    enum ambiguous_kind kind;
    int shown;
};

static int ambiguous_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)path;
    struct ambiguous_walk *w = vctx;
    size_t tl = strlen(w->token);
    bool name_hit = r && r->has_name &&
                    (w->kind == MATCH_NAME_PREFIX ? strncmp(r->name, w->token, tl) == 0 : strcmp(r->name, w->token) == 0);
    bool id_hit = w->kind == MATCH_ID_OR_NAME && strncmp(id, w->token, tl) == 0;
    if (!name_hit && !id_hit) return 0;
    /* Enough to choose from; a token that matches dozens is too short. */
    if (w->shown++ >= 8) return 0;
    char display[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(id, display);
    fprintf(stderr, "  %s  %s\n", display, r && r->has_name ? r->name : "");
    return 0;
}

/* Several processes match: say so, and which, so the next try can be exact -
 * "no process matches" would be false, and acting on one of them a guess. */
int hold_report_ambiguous(const struct hold_store *store, const char *token, bool name_prefix) {
    fprintf(stderr, "backmux: error: more than one process matches '%s':\n", token ? token : "");
    if (store && store->record_dir[0] && token) {
        struct ambiguous_walk w = {token, name_prefix ? MATCH_NAME_PREFIX : MATCH_ID_OR_NAME, 0};
        hold_for_each_record(store->record_dir, ambiguous_cb, &w);
        if (w.shown > 8) fprintf(stderr, "  ... and %d more\n", w.shown - 8);
    }
    fprintf(stderr, "  use a longer process ID prefix or the full process name\n");
    return 6;
}

/* ---- the start of a name ------------------------------------------------- */

struct prefix_walk {
    const char *prefix;
    const char *command;
    const char *boot_id;
    char matched[ID_STR_LEN];
    int matches;
};

static int prefix_match_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    (void)path;
    struct prefix_walk *w = vctx;
    if (!r || !r->has_name || strncmp(r->name, w->prefix, strlen(w->prefix)) != 0) return 0;
    if (w->command && !hold_record_matches_alias_intent(w->command, r, hold_eval_state(r, w->boot_id))) return 0;
    w->matches++;
    snprintf(w->matched, sizeof(w->matched), "%s", r->id);
    return 0;
}

/* How many processes in store have a name starting with prefix (that the
 * command could act on; NULL: any); the last one's id in matched. */
int hold_count_name_prefix(const struct hold_store *store, const char *prefix, const char *command,
                           char matched[ID_STR_LEN]) {
    char boot[128];
    struct prefix_walk w = {prefix, command, hold_boot_id_or_null(boot), {0}, 0};
    if (store && store->record_dir[0] && prefix && *prefix) hold_for_each_record(store->record_dir, prefix_match_cb, &w);
    if (matched) snprintf(matched, ID_STR_LEN, "%s", w.matched);
    return w.matches;
}

struct name_walk {
    const char *name;
    const char *command;
    const char *boot_id;
    char matched[ID_STR_LEN];
    int matches;
};

static int name_match_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    (void)path;
    struct name_walk *w = vctx;
    if (!r || !r->has_name || strcmp(r->name, w->name) != 0) return 0;
    if (hold_record_matches_run_name_intent(w->command, r, hold_eval_state(r, w->boot_id))) {
        w->matches++;
        snprintf(w->matched, sizeof(w->matched), "%s", r->id);
    }
    return 0;
}

static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command) {
    char boot[128];
    struct name_walk w = { name, command, hold_boot_id_or_null(boot), {0}, 0 };
    hold_for_each_record(store->record_dir, name_match_cb, &w);
    if (w.matches == 0) return 0;
    if (w.matches > 1) {
        (void)hold_report_ambiguous(store, name, false);
        return -2;
    }
    return append_resolved_target(targets, count, scope, store, w.matched, false) == 0 ? 1 : -1;
}

static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name) {
    DIR *d = opendir(system_store->public_dir);
    if (!d) return 0;
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) continue;
        struct hold_public_index pi;
        if (hold_load_public_index_by_id(system_store, id, &pi) == 0 &&
            pi.has_name && strcmp(pi.name, name) == 0) {
            matches++;
            snprintf(matched, sizeof(matched), "%s", id);
        }
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        /* (the public projection is not walked for the list: other users'
         * processes are named, not shown) */
        fprintf(stderr, "backmux: error: more than one process is named '%s'\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, system_store, matched, true) == 0 ? 1 : -1;
}

int hold_report_not_found(const char *token) {
    fprintf(stderr, "backmux: error: no process matches '%s'\n", token ? token : "");
    return 5;
}

int hold_report_requires_root(const char *token) {
    fprintf(stderr, "backmux: error: '%s' is root-managed; operate on it as root\n", token ? token : "");
    return 3;
}

/* One candidate store the token's scope may reach, in resolution order. */
struct resolve_candidate {
    enum resolve_scope scope;
    struct hold_store store;
    bool public_scan; /* match via the public projection; hits require root */
};

/* The matrix. Root reads private stores only (system, plus the invoking
 * user's under sudo provenance — user: DEMANDS that provenance); non-root
 * reads its own store, then falls back to the redacted public projection.
 * Returns the candidate count, or -1 with the provenance error printed. */
static int build_scope_candidates(const struct hold_invocation *inv,
                                  const struct hold_store *current_user_store,
                                  const struct hold_store *system_store,
                                  enum id_token_scope scope,
                                  const char *atom,
                                  struct resolve_candidate cands[2]) {
    int n = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "backmux: error: user:%s requires sudo provenance\n", atom);
                return -1;
            }
            cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, user_store, false};
            return n;
        }
        cands[n++] = (struct resolve_candidate){RESOLVE_SYSTEM_MANAGED, *system_store, false};
        if (scope == ID_TOKEN_PLAIN && inv->have_sudo_user) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, user_store, false};
            }
        }
        return n;
    }
    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, *current_user_store, false};
    }
    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        cands[n++] = (struct resolve_candidate){RESOLVE_SYSTEM_MANAGED, *system_store, true};
    }
    return n;
}

static int resolve_name_or_id(const struct hold_invocation *inv,
                              const struct hold_store *current_user_store,
                              const struct hold_store *system_store,
                              const char *command,
                              const char *token,
                              bool report_missing,
                              struct hold_resolved_target **targets_out,
                              int *count_out) {
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    if (scope == ID_TOKEN_INVALID || (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom))) {
        fprintf(stderr, "backmux: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    struct resolve_candidate cands[2];
    int ncands = build_scope_candidates(inv, current_user_store, system_store, scope, atom, cands);
    if (ncands < 0) return 5;

    if (hold_valid_alias(atom)) {
        for (int i = 0; i < ncands; i++) {
            int name_rc = cands[i].public_scan
                ? append_public_run_name_target(targets_out, count_out, system_store, atom)
                : append_private_run_name_target(targets_out, count_out, cands[i].scope, &cands[i].store, atom, command);
            if (name_rc == 1) return 0;
            if (name_rc == -2) return 6;
            if (name_rc < 0) return 3;
        }
    }

    if (hold_valid_id_prefix(atom)) {
        for (int i = 0; i < ncands; i++) {
            char resolved[ID_STR_LEN];
            int id_rc = cands[i].public_scan
                ? resolve_system_public_id(system_store, atom, resolved, sizeof(resolved))
                : hold_resolve_record_id(cands[i].store.record_dir, atom, resolved, sizeof(resolved));
            if (id_rc == 0) {
                return append_resolved_target(targets_out, count_out, cands[i].scope, &cands[i].store,
                                              resolved, cands[i].public_scan) == 0 ? 0 : 3;
            }
            if (id_rc == -2) {
                if (!report_missing) return 6;
                if (cands[i].public_scan) {
                    fprintf(stderr, "backmux: error: more than one process matches '%s'; use a longer process ID prefix\n", token);
                    return 6;
                }
                return hold_report_ambiguous(&cands[i].store, atom, false);
            }
        }
    }

    /* The start of a name, when it is unique among the processes this verb can
     * act on: less typing (`backmux logs hel` for helper), and never a
     * guess - several matches are listed instead. Not for a redial
     * ("start-existing"): there a word that names no process is a program to
     * run, and the top level weighs name starts against PATH itself. Own
     * stores only: other users' processes are reached by their whole name. */
    if (hold_valid_alias(atom) && strcmp(command, "start-existing") != 0) {
        for (int i = 0; i < ncands; i++) {
            if (cands[i].public_scan) continue;
            char matched[ID_STR_LEN];
            int n = hold_count_name_prefix(&cands[i].store, atom, command, matched);
            if (n == 1) {
                return append_resolved_target(targets_out, count_out, cands[i].scope, &cands[i].store,
                                              matched, false) == 0 ? 0 : 3;
            }
            if (n > 1) return report_missing ? hold_report_ambiguous(&cands[i].store, atom, true) : 6;
        }
    }

    return report_missing ? hold_report_not_found(token) : 5;
}

/* A word as a target: a place on the terminal showing panes ("2C", as its
 * name bars show it), or a process name or id (resolve_name_or_id). A place
 * that is also a name or the start of an id is refused, not guessed. Why
 * places: every verb takes the words a person sees on the screen. */
static int resolve_word(const struct hold_invocation *inv,
                        const struct hold_store *current_user_store,
                        const struct hold_store *system_store,
                        const char *command,
                        const char *token,
                        bool report_missing,
                        struct hold_resolved_target **targets_out,
                        int *count_out) {
    const char *atom = NULL;
    char place_id[ID_STR_LEN + 2], place_name[ALIAS_MAX_LEN + 2], resolved[ID_STR_LEN];
    if (hold_parse_id_token(token, &atom) == ID_TOKEN_PLAIN &&
        hold_place_to_id(current_user_store, atom, place_id, sizeof(place_id), place_name, sizeof(place_name)) == 0 &&
        hold_resolve_record_id(current_user_store->record_dir, place_id, resolved, sizeof(resolved)) == 0) {
        struct hold_resolved_target *other = NULL;
        int nother = 0;
        int orc = resolve_name_or_id(inv, current_user_store, system_store, command, token, false, &other, &nother);
        free(other);
        if (orc != 5) {
            if (report_missing)
                fprintf(stderr, "backmux: error: '%s' is both place %c%c (%s) and a process name or id: give more of it\n",
                        token, atom[0], toupper((unsigned char)atom[1]), place_name);
            return 6;
        }
        *targets_out = NULL;
        *count_out = 0;
        return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, resolved, false) == 0 ? 0 : 3;
    }
    return resolve_name_or_id(inv, current_user_store, system_store, command, token, report_missing, targets_out, count_out);
}

/* A word as a target, also in the polite form help shows ("Sleek Carbon"
 * for sleek_carbon): an exact name still wins, then the word read back. */
static int resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool report_missing,
                                struct hold_resolved_target **targets_out,
                                int *count_out) {
    char friendly[ALIAS_MAX_LEN + 1];
    if (hold_friendly_name(token, friendly, sizeof(friendly))) {
        if (hold_valid_alias(token)) { /* a capital only: the word may be a name as it is */
            int rc = resolve_word(inv, current_user_store, system_store, command, token, false, targets_out, count_out);
            if (rc != 5) return rc;
            free(*targets_out);
        }
        int rc = resolve_word(inv, current_user_store, system_store, command, friendly, false, targets_out, count_out);
        if (rc == 5 && report_missing) (void)hold_report_not_found(token); /* the word as it was typed */
        return rc;
    }
    return resolve_word(inv, current_user_store, system_store, command, token, report_missing, targets_out, count_out);
}

/* As hold_resolve_action_token, but nothing is printed when the token matches
 * no process (rc 5): rm -f skips such a name silently, as docker rm -f does. */
int hold_resolve_action_token_quiet(const struct hold_invocation *inv,
                                    const struct hold_store *current_user_store,
                                    const struct hold_store *system_store,
                                    const char *command,
                                    const char *token,
                                    struct hold_resolved_target **targets_out,
                                    int *count_out) {
    return resolve_action_token(inv, current_user_store, system_store, command, token, false, targets_out, count_out);
}

int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out) {
    (void)all;
    return resolve_action_token(inv, current_user_store, system_store, command, token, true, targets_out, count_out);
}

bool hold_token_resolves(const struct hold_invocation *inv,
                         const struct hold_store *current_user_store,
                         const struct hold_store *system_store,
                         const char *command,
                         const char *token) {
    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    char friendly[ALIAS_MAX_LEN + 1];
    if (scope == ID_TOKEN_INVALID ||
        (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom) && !hold_friendly_name(atom, friendly, sizeof(friendly))))
        return true; /* let the caller report it */
    struct hold_resolved_target *targets = NULL;
    int count = 0;
    int rc = resolve_action_token(inv, current_user_store, system_store, command, token, false, &targets, &count);
    free(targets);
    return rc != 5;
}
