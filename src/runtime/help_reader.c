/* runtime/help_reader.c - who is reading help, so help can be for them.
 *
 * Why: an agent pays for every token it reads, and a page written for
 * someone else's case reads to it as a missing feature. Everything here is
 * read, never made: help changes nothing (no inbox, no record, no
 * directory).
 *
 * - Inside or outside backmux: BACKMUX_ID or BACKMUX_NAME set, the rule msg
 *   uses to choose its sender, so help says what msg will do.
 * - Agent or person: a known harness among help's ancestors (the list chat
 *   uses, plus the config's "agents"), up to the backmux broker, the
 *   process's edge. A harness is told by its name, its argv, or a
 *   directory of its executable's path: Claude Code's installed binary is
 *   named for its version (~/.local/share/claude/versions/2.1.332).
 * - Facts a page can name: the process's name and who started it, who
 *   last wrote to it, an outside agent's inbox. Names show in their polite
 *   form ("Sleek Carbon"); the _raw keys hold the name commands take. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/help_text.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"

/* A word, or any directory of a path, that names a harness. */
static bool names_harness(const char *word) {
    char part[HOLD_PATH_MAX], who[AGENT_NAME_MAX];
    if (!word || !*word) return false;
    if (hold_agent_name(word, who, sizeof(who))) return true;
    for (const char *p = word; *p;) {
        size_t len = strcspn(p, "/");
        if (len > 0 && len < sizeof(part)) {
            memcpy(part, p, len);
            part[len] = '\0';
            if (hold_agent_name(part, who, sizeof(who))) return true;
        }
        p += len;
        while (*p == '/') p++;
    }
    return false;
}

static bool agent_ancestor(void) {
    pid_t pid = getppid();
    for (int depth = 0; depth < 16 && pid > 1; depth++) {
        pid_t ppid = 0;
        char comm[64], cmd[4096], who[AGENT_NAME_MAX];
        if (hold_proc_parent_and_name(pid, &ppid, comm, sizeof(comm)) != 0) return false;
        if (!strcmp(comm, "backmux")) return false; /* the broker: outside this process */
        if (names_harness(comm)) return true;
        size_t len = 0;
        if (hold_proc_cmdline(pid, cmd, sizeof(cmd), &len) == 0 && len > 0) {
            char *argv[64];
            int argc = 0;
            for (size_t i = 0; i < len && argc < 63; i += strlen(cmd + i) + 1) argv[argc++] = cmd + i;
            argv[argc] = NULL;
            if (hold_agent_of_argv(argv, argc, who, sizeof(who)) || names_harness(argv[0])) return true;
        }
        pid = ppid;
    }
    return false;
}

static bool reader_is_agent(const char *home) {
#ifdef HOLD_TESTING
    /* The suite's results must not depend on who runs it (an agent's shell
     * would make every help test read the agent's pages). */
    const char *as = getenv("HOLD_TEST_HELP_READER");
    if (as && !strcmp(as, "agent")) return true;
    if (as && !strcmp(as, "person")) return false;
#endif
    char list[1024] = "";
    if (home && *home && hold_settings_get(home, "agents", list, sizeof(list)) == 0) hold_agents_set_extra(list);
    return agent_ancestor();
}

/* A process's current name by id (a rename since its launch shows), or the
 * word as it is. */
static void current_name(const struct hold_store *s, const char *word, char *out, size_t n) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    snprintf(out, n, "%s", word ? word : "");
    if (!word || !hold_valid_id(word) || hold_load_record_by_id(s->record_dir, word, &r, path, sizeof(path)) != 0) return;
    if (r.has_name && r.name[0]) snprintf(out, n, "%s", r.name);
    hold_free_run_record(&r);
}

/* Who last wrote to the process: the newest line of its .msgs log. */
static void last_message(const struct hold_store *s, const char *id, char *from, size_t fn, char *at, size_t an) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    from[0] = at[0] = '\0';
    if (!id || !hold_valid_id(id) || hold_load_record_by_id(s->record_dir, id, &r, path, sizeof(path)) != 0) return;
    size_t ll = r.has_log ? strlen(r.log_path) : 0;
    FILE *f = NULL;
    if (ll > 4 && !strcmp(r.log_path + ll - 4, ".log") &&
        hold_checked_snprintf(path, sizeof(path), "%.*s.msgs", (int)(ll - 4), r.log_path) == 0) {
        int fd = hold_open_owned_file_no_symlink(path, NULL);
        if (fd >= 0 && !(f = fdopen(fd, "r"))) close(fd);
    }
    hold_free_run_record(&r);
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char mid[8], who[ALIAS_MAX_LEN + ID_STR_LEN + 1];
        long long when;
        int flag;
        if (sscanf(line, "%7s %lld %d %128s", mid, &when, &flag, who) != 4 || !strcmp(who, "-")) continue;
        time_t tt = (time_t)when;
        struct tm tmv;
        if (strlen(who) >= fn) continue; /* an id, not a name: nothing to show */
        memcpy(from, who, strlen(who) + 1);
        if (localtime_r(&tt, &tmv)) strftime(at, an, "%H:%M", &tmv);
    }
    free(line);
    fclose(f);
}

void hold_help_reader_view(struct hold_help_view *view, bool full) {
    static char name[ALIAS_MAX_LEN + 1], name_d[ALIAS_MAX_LEN + 1], parent[ALIAS_MAX_LEN + 1],
        parent_d[ALIAS_MAX_LEN + 1], from[ALIAS_MAX_LEN + 1], from_d[ALIAS_MAX_LEN + 1], at[8],
        inbox[ALIAS_MAX_LEN + 1], inbox_d[ALIAS_MAX_LEN + 1], chat[ALIAS_MAX_LEN + 1];
    static struct hold_help_var vars[11];
    memset(view, 0, sizeof(*view));
    view->full = full;
    if (full) return;
    const char *home = getenv("HOME");
    const char *id = getenv("BACKMUX_ID"), *env_name = getenv("BACKMUX_NAME");
    view->inside = (id && *id) || (env_name && *env_name);
    view->agent = reader_is_agent(home);
    name[0] = parent[0] = from[0] = at[0] = inbox[0] = chat[0] = '\0';
    struct hold_store s;
    bool have_store = home && *home && hold_init_user_store_from_home(home, &s) == 0;
    if (view->inside) {
        if (have_store && id && *id) current_name(&s, id, name, sizeof(name));
        if (!name[0] || hold_valid_id(name)) snprintf(name, sizeof(name), "%s", env_name ? env_name : "");
        const char *p = getenv("BACKMUX_PARENT");
        if (p && *p && have_store) current_name(&s, p, parent, sizeof(parent));
        if (have_store) last_message(&s, id, from, sizeof(from), at, sizeof(at));
    } else if (have_store && hold_operator_name_peek(&s, false, inbox, sizeof(inbox)) != 0) {
        inbox[0] = '\0';
    }
    bool from_person = from[0] && have_store && hold_operator_name_peek(&s, true, chat, sizeof(chat)) == 0 &&
                       !strcmp(chat, from);
    hold_friendly_display(name, name_d, sizeof(name_d));
    hold_friendly_display(parent, parent_d, sizeof(parent_d));
    hold_friendly_display(from, from_d, sizeof(from_d));
    hold_friendly_display(inbox, inbox_d, sizeof(inbox_d));
    size_t k = 0;
    vars[k++] = (struct hold_help_var){"name", name_d};
    vars[k++] = (struct hold_help_var){"name_raw", name};
    vars[k++] = (struct hold_help_var){"parent", parent_d};
    vars[k++] = (struct hold_help_var){"parent_raw", parent};
    vars[k++] = (struct hold_help_var){"last_from", from_d};
    vars[k++] = (struct hold_help_var){"last_from_raw", from};
    vars[k++] = (struct hold_help_var){"last_at", at};
    vars[k++] = (struct hold_help_var){"inbox", inbox_d};
    vars[k++] = (struct hold_help_var){"inbox_raw", inbox};
    vars[k++] = (struct hold_help_var){"next", from_person ? "assist" : ""}; /* only a real next step: a person asked */
    /* A person inside backmux: the chat name agents reach them by. */
    if (!view->agent && view->inside && have_store && hold_operator_name_peek(&s, true, chat, sizeof(chat)) != 0) chat[0] = '\0';
    vars[k++] = (struct hold_help_var){"chat", view->agent ? "" : chat};
    view->vars = vars;
    view->nvars = k;
}
