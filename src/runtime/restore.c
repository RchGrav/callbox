/* runtime/restore.c - rebuilding a terminal on recall from what was observed.
 *
 * Why: a saved terminal is the person's work, not its launch line. A bash
 * launched in ~ that was running claude in ~/callbox comes back as bash in
 * ~/callbox running claude, not as an empty bash in ~ (directive 01 §3).
 *
 * From the record's terminal state (store/term_state.c): the program starts
 * in the directory it was last observed in. A hosting shell then gets,
 * typed at its prompt by the broker, its environment source (set NAME
 * source=FILE) and the workload it was running, with the workload's exact
 * argv, in the workload's own directory when that differs from the shell's
 * (through sh, so the shell's directory stays put); the shell stays after
 * the workload exits. What cannot be rebuilt is said, never guessed: a
 * pipeline (one command line cannot rebuild it), an argument holding a
 * control character (typing it would submit or edit the line), a csh. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"

struct sbuf {
    char *s;
    size_t n, cap;
    bool failed;
};

static void put(struct sbuf *b, const char *s, size_t n) {
    if (b->failed) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) {
            b->failed = true;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void puts_(struct sbuf *b, const char *s) { put(b, s, strlen(s)); }

/* One word, single-quoted for the shell it is typed into: POSIX shells
 * end the quote to add a quote ('\''); fish escapes quote and backslash. */
static void put_word(struct sbuf *b, const char *w, bool fish) {
    puts_(b, "'");
    for (const char *p = w; *p; p++) {
        if (*p == '\'') puts_(b, fish ? "\\'" : "'\\''");
        else if (fish && *p == '\\') puts_(b, "\\\\");
        else put(b, p, 1);
    }
    puts_(b, "'");
}

static bool has_control_char(int argc, char **argv) {
    for (int i = 0; i < argc; i++)
        for (const unsigned char *p = (const unsigned char *)argv[i]; p && *p; p++)
            if (*p < 0x20 || *p == 0x7f) return true;
    return false;
}

static const char *program_name(const char *argv0) {
    const char *b = strrchr(argv0, '/');
    b = b ? b + 1 : argv0;
    return *b == '-' ? b + 1 : b;
}

void hold_restore_free(struct hold_restore *p) {
    free(p->input);
    p->input = NULL;
}

int hold_restore_plan(const struct hold_run_record *r, struct hold_restore *p) {
    memset(p, 0, sizeof(*p));
    if (!r->recipe.mode_tty || !r->has_term) return 0;
    const struct hold_term_state *t = &r->term;
    struct stat st;
    if (t->cwd[0] && stat(t->cwd, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(p->dir, sizeof(p->dir), "%s", t->cwd);
    else if (t->cwd[0])
        snprintf(p->note, sizeof(p->note), "its directory %s is gone; it starts where it was launched", t->cwd);
    if (!t->hosting_shell || r->recipe.argc < 1 || !r->recipe.argv) return 0;
    const char *shell = program_name(r->recipe.argv[0]);
    bool fish = !strcmp(shell, "fish");
    bool csh = !strcmp(shell, "csh") || !strcmp(shell, "tcsh");
    bool source = r->recipe.has_env_source;
    bool work = !strcmp(t->foreground, "workload") && t->workload_argc > 0;
    if (!strcmp(t->foreground, "pipeline"))
        snprintf(p->note, sizeof(p->note), "the pipeline it ran was not started again: one command line cannot rebuild it");
    if (work && has_control_char(t->workload_argc, t->workload_argv)) {
        snprintf(p->note, sizeof(p->note), "its workload was not started again: an argument holds a control character");
        work = false;
    }
    if ((source || work) && csh) {
        snprintf(p->note, sizeof(p->note), "csh is not typed into: its environment source and workload were not run");
        return 0;
    }
    if (!source && !work) return 0;
    /* Each line starts with a space: bash (ignorespace), zsh
     * (HIST_IGNORE_SPACE) and fish leave such lines out of the history. */
    struct sbuf b = {0};
    if (source) {
        puts_(&b, fish ? " source " : " . ");
        put_word(&b, r->recipe.env_source, fish);
        puts_(&b, "\r");
    }
    if (work) {
        puts_(&b, " ");
        const char *here = p->dir[0] ? p->dir : t->cwd;
        if (t->workload_cwd[0] && strcmp(t->workload_cwd, here) != 0) {
            puts_(&b, "sh -c 'cd \"$1\" && shift && exec \"$@\"' sh ");
            put_word(&b, t->workload_cwd, fish);
            puts_(&b, " ");
        }
        for (int i = 0; i < t->workload_argc; i++) {
            put_word(&b, t->workload_argv[i] ? t->workload_argv[i] : "", fish);
            puts_(&b, i + 1 < t->workload_argc ? " " : "\r");
        }
    }
    if (b.failed) {
        free(b.s);
        return -1;
    }
    p->input = b.s;
    return 0;
}
