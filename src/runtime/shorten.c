/* runtime/shorten.c - a word at the top level that is the start of exactly
 * one verb or one process name (backmux li for list, backmux hel for
 * helper): what it stands for, or the matches listed when it could be
 * several. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/access.h"

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
