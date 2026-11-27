/* Process settings and related commands:
 * set/unset (working, program, args, name), info, recall, delete,
 * and purge all.
 *
 * Settings can change while a process is running or after it ends. name
 * changes target resolution immediately; working, program, and args affect
 * the next launch. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"

static const char set_usage[] =
    "usage: backmux set <name|id> <setting>[=<value>]\n"
    "       backmux unset <name|id> <setting>\n"
    "settings: working=DIR, program=PROGRAM, args=ARGS (or: args -- ARG...), name=NAME\n";

static const char *label_of(const struct hold_run_record *r, char *display, size_t n) {
    if (r->has_name && r->name[0]) return r->name;
    hold_run_id_display(r->id, display);
    (void)n;
    return display;
}

/* args= splits on spaces and tabs. args -- preserves the argument
 * boundaries supplied by the invoking shell, including embedded spaces. */
static int split_words(const char *s, char ***out, int *outc) {
    *out = NULL;
    *outc = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char *start = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        char **next = realloc(*out, ((size_t)*outc + 1) * sizeof(*next));
        if (!next) return -1;
        *out = next;
        if (!((*out)[*outc] = strndup(start, (size_t)(s - start)))) return -1;
        (*outc)++;
    }
    return 0;
}

int hold_cmd_set_action(const struct hold_invocation *inv,
                        const struct hold_store *user_store,
                        const struct hold_store *system_store,
                        int argc,
                        char **argv,
                        bool unset) {
    if (argc < 2) {
        fputs(set_usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    const char *token = argv[0];
    char key[32];
    const char *value = NULL;
    const char *eq = strchr(argv[1], '=');
    size_t kl = eq ? (size_t)(eq - argv[1]) : strlen(argv[1]);
    if (kl == 0 || kl >= sizeof(key)) {
        fputs(set_usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    memcpy(key, argv[1], kl);
    key[kl] = '\0';
    if (eq) value = eq + 1;
    bool is_args = !strcmp(key, "args");
    /* Words after the setting belong only to "args" (args -- a b c). */
    int extra_first = 2;
    if (is_args && !eq && argc > 2 && !strcmp(argv[2], "--")) extra_first = 3;
    if (!(is_args && !eq && !unset) && argc > 2) {
        fputs(set_usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    if (strcmp(key, "working") && strcmp(key, "program") && !is_args && strcmp(key, "name")) {
        fprintf(stderr, "backmux: error: unknown setting '%s'\n%s", key, set_usage);
        return HOLD_EXIT_USAGE;
    }
    if (unset && value) {
        fprintf(stderr, "backmux: error: unset accepts a setting name without a value: backmux unset %s %s\n", token, key);
        return HOLD_EXIT_USAGE;
    }
    if (unset && (!strcmp(key, "program") || !strcmp(key, "name"))) {
        fprintf(stderr, "backmux: error: %s cannot be unset: %s\n", key,
                !strcmp(key, "program") ? "a launch profile requires a program; use backmux set NAME program=..."
                                        : "the process name is required; use backmux set NAME name=...");
        return HOLD_EXIT_USAGE;
    }
    if (!unset && (!strcmp(key, "working") || !strcmp(key, "program") || !strcmp(key, "name")) && (!value || !*value)) {
        fprintf(stderr, "backmux: error: %s requires a value: backmux set %s %s=...\n", key, token, key);
        return HOLD_EXIT_USAGE;
    }
    /* Renaming releases the previous name for reuse. */
    if (!strcmp(key, "name")) return hold_cmd_rename_action(inv, user_store, system_store, token, value);

    char new_working[HOLD_PATH_MAX] = "";
    char new_program[HOLD_PATH_MAX] = "";
    if (!unset && !strcmp(key, "working")) {
        char cwd[HOLD_PATH_MAX], abs[HOLD_PATH_MAX];
        struct stat st;
        if (!getcwd(cwd, sizeof(cwd)) ||
            hold_absolute_existing_path_from_cwd(value, cwd, abs, sizeof(abs)) != 0 || !realpath(abs, new_working) ||
            stat(new_working, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "backmux: error: working: cannot resolve '%s' to an existing directory\n", value);
            return 5;
        }
    }
    if (!unset && !strcmp(key, "program") && hold_find_binary_path(value, new_program, sizeof(new_program)) != 0) {
        fprintf(stderr, "backmux: error: program: '%s' %s\n", value,
                errno == EACCES ? "is not executable" : "was not found (as a path, or on PATH)");
        return errno == EACCES ? 126 : 127;
    }

    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "set", token, &target, &hit);
    if (!hit) return rc;
    if (target.requires_root) return hold_report_requires_root(target.id);
    /* The store lock serializes this read-modify-write with the exit stamp,
     * which could otherwise rewrite the record from an older copy. */
    int lock_fd = hold_store_lock(target.store.record_dir);
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) {
        hold_store_unlock(lock_fd);
        return rc;
    }
    char *j = NULL;
    char **old_argv = NULL, **new_argv = NULL, **words = NULL;
    int old_argc = 0, new_argc = 0, nwords = 0;
    rc = 0;
    if (hold_read_owned_file_no_symlink(path, &j) != 0 || hold_json_get_argv_alloc(j, &old_argv, &old_argc) != 0 ||
        old_argc < 1) {
        fprintf(stderr, "backmux: error: cannot read %s's record\n", token);
        rc = 5;
        goto out;
    }
    new_argv = old_argv;
    new_argc = old_argc;
    const char *when = "from the next launch (backmux recall)";
    if (!strcmp(key, "working")) {
        r.recipe.has_working = !unset;
        snprintf(r.recipe.working, sizeof(r.recipe.working), "%s", unset ? "" : new_working);
    } else if (!strcmp(key, "program")) {
        free(old_argv[0]);
        if (!(old_argv[0] = strdup(new_program))) {
            rc = 1;
            goto out;
        }
    } else { /* args */
        if (!unset) {
            if (eq) {
                if (split_words(value, &words, &nwords) != 0) {
                    rc = 1;
                    goto out;
                }
            } else {
                nwords = argc - extra_first;
                words = calloc((size_t)nwords + 1, sizeof(*words));
                if (!words) {
                    rc = 1;
                    goto out;
                }
                for (int i = 0; i < nwords; i++)
                    if (!(words[i] = strdup(argv[extra_first + i]))) {
                        rc = 1;
                        goto out;
                    }
            }
        }
        new_argc = 1 + nwords;
        new_argv = calloc((size_t)new_argc + 1, sizeof(*new_argv));
        if (!new_argv) {
            rc = 1;
            goto out;
        }
        new_argv[0] = old_argv[0];
        for (int i = 0; i < nwords; i++) new_argv[1 + i] = words[i];
    }
    char out_path[HOLD_PATH_MAX];
    if (hold_write_record_atomic(target.store.record_dir, &r, new_argc, new_argv, out_path, sizeof(out_path)) != 0) {
        fprintf(stderr, "backmux: error: cannot write %s's record: %s\n", token, strerror(errno));
        rc = 1;
        goto out;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) (void)hold_write_public_index_atomic(&target.store, &r, NULL);
    char display[ID_DISPLAY_HEX_LEN + 1];
    const char *label = label_of(&r, display, sizeof(display));
    if (unset)
        hold_sig_note(inv, "backmux: %s: %s unset, %s\n", label, key, when);
    else
        hold_sig_note(inv, "backmux: %s: %s set, %s\n", label, key, when);
out:
    hold_store_unlock(lock_fd);
    if (new_argv != old_argv) free(new_argv); /* its strings belong to old_argv and words */
    hold_free_argv_alloc(old_argv, old_argc);
    hold_free_argv_alloc(words, nwords);
    free(j);
    hold_free_run_record(&r);
    return rc;
}

/* Describe the stored launch mode using the corresponding command name. */
static const char *call_kind(const struct hold_run_record *r) {
    if (r->recipe.mode_shell) return "shell";
    if (r->recipe.mode_detach) return r->recipe.mode_tty ? "detach" : "server";
    if (r->recipe.mode_tty) return "call";
    return "server";
}

int hold_cmd_info_action(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         const char *token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "info", token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    enum run_state st = hold_eval_state(&r, hold_boot_id_or_null(boot));
    char args[HOLD_PATH_MAX] = "";
    if (r.recipe.argc > 1) (void)hold_format_argv_human(args, sizeof(args), r.recipe.argc - 1, r.recipe.argv + 1);
    printf("name:      %s\n", r.has_name ? r.name : "-");
    printf("id:        %s\n", r.id);
    printf("kind:      %s\n", call_kind(&r));
    printf("state:     %s", hold_state_str(st));
    if (st != STATE_RUNNING && r.has_exit_code) printf(" (exit %d)", r.exit_code);
    if (st != STATE_RUNNING && r.has_term_signal && r.term_signal) printf(" (signal %d)", r.term_signal);
    putchar('\n');
    printf("program:   %s\n", r.recipe.argc > 0 && r.recipe.argv ? r.recipe.argv[0] : "?");
    printf("args:      %s\n", args[0] ? args : "-");
    printf("working:   %s\n", r.recipe.has_working ? r.recipe.working : "- (recall uses the current working directory)");
    if (r.has_observed && r.observed_cwd[0]) printf("started in: %s\n", r.observed_cwd);
    printf("saved:     %s\n", r.saved ? "yes" : "no");
    if (r.has_started_at) printf("started:   %s\n", r.started_at);
    if (r.has_ended_at && st != STATE_RUNNING) printf("ended:     %s\n", r.ended_at);
    if (st == STATE_RUNNING) printf("pid:       %d\n", (int)r.pid);
    if (r.recipe.has_size) printf("size:      %dx%d\n", r.recipe.size_cols, r.recipe.size_rows);
    if (r.recipe.has_restart_policy) printf("restart:   %s\n", r.recipe.restart_policy);
    /* Omit environment values from this summary; inspect shows the record. */
    for (int i = 0; i < r.recipe.envc; i++) {
        const char *e = r.recipe.env[i];
        const char *q = strchr(e, '=');
        printf("%s%.*s\n", i == 0 ? "env:       " : "           ", q ? (int)(q - e) : (int)strlen(e), e);
    }
    if (r.has_log) printf("log:       %s\n", r.log_path);
    if (r.has_console) printf("console:   %s\n", r.console_sock);
    printf("store:     %s\n", target.store.kind == STORE_SYSTEM_MANAGED ? "system" : "user");
    hold_free_run_record(&r);
    return 0;
}

int hold_cmd_recall_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "recall", token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    bool running = hold_eval_state(&r, hold_boot_id_or_null(boot)) == STATE_RUNNING;
    char display[ID_DISPLAY_HEX_LEN + 1];
    char label[ALIAS_MAX_LEN + 1];
    snprintf(label, sizeof(label), "%s", label_of(&r, display, sizeof(display)));
    hold_free_run_record(&r);
    /* Reject a confirmed running target before attempting a relaunch. */
    if (running) {
        fprintf(stderr, "backmux: %s is running; use backmux attach %s (console) or backmux tail <target> (log)\n", label, label);
        return 1;
    }
    bool redialed = false;
    rc = hold_cmd_redial(inv, user_store, system_store, true, false, false, false, false, false, NULL,
                         HOLD_RESTART_BACKOFF, 0, 0, target.id, &redialed);
    if (!redialed) return hold_report_not_found(token);
    return rc;
}

int hold_cmd_delete_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "delete", token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    bool running = hold_eval_state(&r, hold_boot_id_or_null(boot)) == STATE_RUNNING;
    char display[ID_DISPLAY_HEX_LEN + 1];
    char label[ALIAS_MAX_LEN + 1];
    snprintf(label, sizeof(label), "%s", label_of(&r, display, sizeof(display)));
    hold_free_run_record(&r);
    /* Reject a confirmed running target before removing its retained profile. */
    if (running) {
        fprintf(stderr, "backmux: %s is running; run backmux end %s before deleting its profile\n", label, label);
        return 1;
    }
    return hold_cmd_purge_action(inv, user_store, system_store, target.id, false, true,
                                 target.store.kind == STORE_SYSTEM_MANAGED);
}

/* purge all selects unsaved records, stopping running processes first. */
struct purge_all_walk {
    const char *boot_id;
    int running, ended;
    char (*ids)[ID_STR_LEN];
    char (*labels)[ALIAS_MAX_LEN + 1];
    bool *live;
    int n, cap;
};

static int purge_all_cb(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct purge_all_walk *w = ctx;
    if (!r || r->saved) return 0;
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 16;
        void *a = realloc(w->ids, (size_t)cap * sizeof(*w->ids));
        if (!a) return -1;
        w->ids = a;
        void *b = realloc(w->labels, (size_t)cap * sizeof(*w->labels));
        if (!b) return -1;
        w->labels = b;
        void *c = realloc(w->live, (size_t)cap * sizeof(*w->live));
        if (!c) return -1;
        w->live = c;
        w->cap = cap;
    }
    bool live = hold_eval_state(r, w->boot_id) == STATE_RUNNING;
    snprintf(w->ids[w->n], ID_STR_LEN, "%s", id);
    if (r->has_name && r->name[0]) snprintf(w->labels[w->n], ALIAS_MAX_LEN + 1, "%s", r->name);
    else hold_run_id_display(id, w->labels[w->n]);
    w->live[w->n] = live;
    if (live) w->running++;
    else w->ended++;
    w->n++;
    return 0;
}

int hold_cmd_purge_all_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              bool force,
                              bool system_scope) {
    const struct hold_store *store = system_scope ? system_store : user_store;
    char boot[128];
    struct purge_all_walk w = {.boot_id = hold_boot_id_or_null(boot)};
    if (hold_for_each_record(store->record_dir, purge_all_cb, &w) != 0) {
        fprintf(stderr, "backmux: error: cannot read process records\n");
        return 1;
    }
    int rc = 0;
    if (w.n == 0) {
        hold_sig_note(inv, "backmux: nothing to purge (saved processes are kept)\n");
        goto out;
    }
    /* The CLI operation does not prompt; saved records were excluded above. */
    (void)force;

    int removed = 0;
    for (int i = 0; i < w.n; i++) {
        if (w.live[i]) {
            char *one[1] = {w.ids[i]};
            /* Suppress per-target success notes; failures remain visible. */
            struct hold_invocation quiet = *inv;
            quiet.quiet = true;
            int src = hold_cmd_signal_action(&quiet, user_store, system_store, "stop", 1, one, SIGTERM, true, false, false);
            if (src != 0) {
                if (src > rc) rc = src;
                continue;
            }
        }
        bool gone = false;
        int prc = hold_prune_one_run_ex(store, w.ids[i], w.boot_id, true, true, &gone);
        if (gone) removed++;
        if (prc > rc) rc = prc;
    }
    if (w.running > 0)
        hold_sig_note(inv, "Selected %d running process%s for purge; removed %d record%s.\n", w.running, w.running == 1 ? "" : "es", removed,
                      removed == 1 ? "" : "s");
    else
        hold_sig_note(inv, "Removed %d record%s.\n", removed, removed == 1 ? "" : "s");
out:
    free(w.ids);
    free(w.labels);
    free(w.live);
    return rc;
}

/* --agent: the program's name joins the person's list of agent harnesses
 * (the config's "agents"), so chat lists it - this time and every time it
 * runs from now on, however it is started. Why a list kept, not a mark on
 * one process: what the person said is that this program is an agent, and
 * a harness started again tomorrow is the same agent. */
void hold_remember_agent(const struct hold_invocation *inv, int argc, char **argv) {
    char word[AGENT_NAME_MAX];
    if (!hold_agent_word(argv, argc, word, sizeof(word))) return;
    if (inv && inv->euid_root && inv->have_sudo_user) {
        hold_sig_note(inv, "backmux: --agent is remembered in your own config: run it without sudo\n");
        return;
    }
    const char *home = getenv("HOME");
    char list[1024] = "";
    if (!home || !*home) return;
    if (hold_settings_get(home, "agents", list, sizeof(list)) != 0) list[0] = '\0';
    hold_agents_set_extra(list);
    if (hold_agent_name(word, NULL, 0)) return; /* an agent already: known by name, or listed */
    char next[1100];
    snprintf(next, sizeof(next), "%s%s%s", list, list[0] ? " " : "", word);
    if (hold_settings_put(home, "agents", next) != 0) {
        hold_sig_note(inv, "backmux: could not remember %s as an agent: %s\n", word, strerror(errno));
        return;
    }
    hold_agents_set_extra(next);
    hold_sig_note(inv, "backmux: %s is an agent from now on (backmux config get agents)\n", word);
}

/* ---- the compositor's needs, and backmux swap -------------------------- */

void hold_prepare_mux(const struct hold_invocation *inv, const struct hold_store *store) {
    static struct hold_store kept;
    if (!store || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
    kept = *store;
    char own[ALIAS_MAX_LEN + 1] = "", name[ALIAS_MAX_LEN + 1] = "";
    const char *home = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_home[0] ? inv->invoking_home
                                                                                           : getenv("HOME");
    if (home && hold_settings_get(home, "chat-name", own, sizeof(own)) != 0) own[0] = '\0';
    if (hold_person_chat_name(&kept, own, name, sizeof(name)) != 0) name[0] = '\0';
    hold_console_set_mux(&kept, name);
    /* the harnesses the person added with --agent: chat lists them too */
    hold_agents_set_home(home);
}

struct mux_seen {
    pid_t pid;
    bool has_here;
};

/* backmux swap targets panes by process name or ID, not keyboard slot labels.
 * From inside a pane it selects that terminal; otherwise exactly one terminal
 * must be displaying panes. Multiple eligible terminals produce an error. */
int hold_cmd_swap_action(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    (void)system_store;
    char a[96] = "", b[96] = "";
    if (argc == 1) {
        const char *comma = strchr(argv[0], ',');
        if (!comma || comma == argv[0] || !comma[1] || (size_t)(comma - argv[0]) >= sizeof(a)) {
            fprintf(stderr, "usage: backmux swap TARGET,TARGET | TARGET TARGET   (process names or IDs)\n");
            return HOLD_EXIT_USAGE;
        }
        snprintf(a, sizeof(a), "%.*s", (int)(comma - argv[0]), argv[0]);
        snprintf(b, sizeof(b), "%s", comma + 1);
    } else {
        snprintf(a, sizeof(a), "%s", argv[0]);
        snprintf(b, sizeof(b), "%s", argv[1]);
    }
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/chat", user_store->base) != 0) return 1;
    DIR *d = opendir(dir);
    const char *here = getenv("BACKMUX_ID");
    struct mux_seen seen[16];
    int nseen = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && nseen < 16) {
        int pid = 0;
        char tail[8] = "";
        if (sscanf(e->d_name, "mux.%d%7s", &pid, tail) != 1 || pid <= 1) continue;
        if (kill(pid, 0) != 0 && errno == ESRCH) continue;
        char path[HOLD_PATH_MAX], text[4096] = "";
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) != 0) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        ssize_t n = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (n < 0) continue;
        text[n] = '\0';
        seen[nseen].pid = pid;
        seen[nseen].has_here = here && *here && strstr(text, here) != NULL;
        nseen++;
    }
    if (d) closedir(d);
    int pick = -1;
    for (int i = 0; i < nseen; i++)
        if (seen[i].has_here) pick = i;
    if (pick < 0 && nseen == 1) pick = 0;
    if (pick < 0) {
        fprintf(stderr, nseen == 0 ? "backmux: no active pane session found; press Ctrl-Space p a while attached to add a pane\n"
                                   : "backmux: multiple terminals have panes; run backmux swap inside the target terminal\n");
        return 1;
    }
    char req[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX + 8], body[200];
    if (hold_checked_snprintf(req, sizeof(req), "%s/mux.%d.swap", dir, (int)seen[pick].pid) != 0 ||
        hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp", req) != 0)
        return 1;
    int k = snprintf(body, sizeof(body), "%s %s\n", a, b);
    char stale[HOLD_PATH_MAX];
    if (hold_checked_snprintf(stale, sizeof(stale), "%s/mux.%d.result", dir, (int)seen[pick].pid) == 0) unlink(stale);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || hold_write_all(fd, body, (size_t)k) != 0 || close(fd) != 0 || rename(tmp, req) != 0) {
        if (fd >= 0) unlink(tmp);
        fprintf(stderr, "backmux: error: cannot send the swap request to the terminal: %s\n", strerror(errno));
        return 1;
    }
    /* The compositor processes the request in its event loop. Poll for a
     * response for up to two seconds. */
    char res[HOLD_PATH_MAX];
    if (hold_checked_snprintf(res, sizeof(res), "%s/mux.%d.result", dir, (int)seen[pick].pid) != 0) return 1;
    for (int waited = 0; waited < 2000; waited += 50) {
        char answer[200] = "";
        int rfd = open(res, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (rfd >= 0) {
            ssize_t n = read(rfd, answer, sizeof(answer) - 1);
            close(rfd);
            unlink(res);
            answer[n > 0 ? n : 0] = '\0';
            if (!strncmp(answer, "ok", 2)) {
                hold_sig_note(inv, "backmux: swapped %s and %s\n", a, b);
                return 0;
            }
            fprintf(stderr, "backmux: cannot swap %s and %s: %.*s\n", a, b, (int)strcspn(answer + 6, "\n"),
                    n > 6 ? answer + 6 : "unknown");
            return 1;
        }
        struct timespec sl = {0, 50 * 1000000L};
        nanosleep(&sl, NULL);
    }
    unlink(req);
    fprintf(stderr, "backmux: terminal did not respond to the swap request within 2 seconds\n");
    return 1;
}
