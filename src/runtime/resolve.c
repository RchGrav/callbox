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

static int resolve_action_token(const struct hold_invocation *inv,
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
    if (scope == ID_TOKEN_INVALID || (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom))) return true; /* let the caller report it */
    struct hold_resolved_target *targets = NULL;
    int count = 0;
    int rc = resolve_action_token(inv, current_user_store, system_store, command, token, false, &targets, &count);
    free(targets);
    return rc != 5;
}

/* ---- a shortened word at the top level ------------------------------------
 *
 * `backmux <word>` where word is no verb: a process by its whole name or an id
 * prefix (redial), else a program on PATH, else - only then - the start of
 * a verb or of a process's name, when exactly one fits. In that order so that
 * nothing that worked before changes meaning: a whole name, an id, and a
 * program all still win; a shortened word is only ever what would have been
 * "command not found". Why at all: less typing (`backmux li`, `backmux
 * pee term`, `backmux hel` for helper), and never a guess - several fits are
 * listed instead, and nothing runs. */

struct short_name_walk {
    const char *prefix;
    int matches;
    char name[ALIAS_MAX_LEN + 1];
    char shown[8][ALIAS_MAX_LEN + 1];
};

static int short_name_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    (void)path;
    struct short_name_walk *w = vctx;
    if (!r || !r->has_name || strncmp(r->name, w->prefix, strlen(w->prefix)) != 0) return 0;
    for (int i = 0; i < w->matches && i < 8; i++)
        if (!strcmp(w->shown[i], r->name)) return 0; /* the same name in both stores */
    if (w->matches < 8) snprintf(w->shown[w->matches], sizeof(w->shown[0]), "%s", r->name);
    snprintf(w->name, sizeof(w->name), "%s", r->name);
    w->matches++;
    return 0;
}

int hold_shorten_word(const char *word, bool lone, const char *const *verbs, size_t nverbs,
                      const char **verb_out, char *name_out, size_t name_n) {
    *verb_out = NULL;
    if (name_out && name_n) name_out[0] = '\0';
    if (!word || !*word || strchr(word, '/') || word[0] == '-') return 0;
    struct hold_invocation inv;
    if (hold_detect_invocation(&inv, false) != 0) return 0;
    struct hold_store user_store, system_store;
    memset(&user_store, 0, sizeof(user_store));
    memset(&system_store, 0, sizeof(system_store));
    (void)hold_init_system_store(&system_store);
    const char *home = inv.euid_root && inv.have_sudo_user && inv.invoking_home[0] ? inv.invoking_home : getenv("HOME");
    if (home && *home) (void)hold_init_user_store_from_home(home, &user_store);

    /* What already has a meaning keeps it. */
    if (lone && (hold_valid_alias(word) || hold_valid_id_prefix(word)) &&
        hold_token_resolves(&inv, &user_store, &system_store, "start-existing", word))
        return 0;
    char found[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(word, found, sizeof(found)) == 0) return 0;

    size_t wl = strlen(word);
    const char *verb_hits[16];
    size_t nverb_hits = 0;
    for (size_t i = 0; i < nverbs; i++)
        if (!strncmp(verbs[i], word, wl) && nverb_hits < 16) verb_hits[nverb_hits++] = verbs[i];
    /* A process's name counts only for a lone word: with arguments after it,
     * the word is a program to run (a redial takes no arguments). */
    struct short_name_walk w = {word, 0, {0}, {{0}}};
    if (lone && hold_valid_alias(word)) {
        if (user_store.record_dir[0]) hold_for_each_record(user_store.record_dir, short_name_cb, &w);
        if (inv.euid_root && system_store.record_dir[0]) hold_for_each_record(system_store.record_dir, short_name_cb, &w);
    }
    size_t total = nverb_hits + (size_t)w.matches;
    if (total == 0) return 0;
    if (total == 1) {
        if (nverb_hits == 1) {
            *verb_out = verb_hits[0];
            return 1;
        }
        snprintf(name_out, name_n, "%s", w.name);
        return 2;
    }
    fprintf(stderr, "backmux: error: ambiguous command or process-name prefix '%s':\n", word);
    if (nverb_hits) {
        fprintf(stderr, "  commands:");
        for (size_t i = 0; i < nverb_hits; i++) fprintf(stderr, " %s", verb_hits[i]);
        fprintf(stderr, "\n");
    }
    if (w.matches) {
        fprintf(stderr, "  processes:");
        for (int i = 0; i < w.matches && i < 8; i++) fprintf(stderr, " %s", w.shown[i]);
        if (w.matches > 8) fprintf(stderr, " ... and %d more", w.matches - 8);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  use a longer prefix or the full name\n");
    return 6;
}
