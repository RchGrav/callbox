/* runtime/mux_cmd.c - what the compositor needs from the runtime: the
 * person's chat name, their agent list and slide setting (read when a
 * terminal is prepared for panes), --agent remembering a harness, and
 * backmux swap, which finds the terminal showing the panes and asks it. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"

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
    char slide[8];
    hold_console_set_slide(!(home && hold_settings_get(home, "slide", slide, sizeof(slide)) == 0 && !strcmp(slide, "off")));
}

struct mux_seen {
    pid_t pid;
    bool has_here;
};

/* Ask the terminal showing panes: from inside a pane, that pane's terminal;
 * otherwise the one terminal with panes (several: the caller must say from
 * inside one). The request goes in chat/mux.<pid>.ask; the answer comes in
 * chat/mux.<pid>.result within wait_ms. 0 and *answer "ok..." or "error
 * WHY"; -1 when there was nobody to ask (said on stderr). */
static int ask_terminal(const struct hold_store *user_store, const char *body, int wait_ms, const char *verb,
                        char *answer, size_t an) {
    answer[0] = '\0';
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/chat", user_store->base) != 0) return -1;
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
        char path[HOLD_PATH_MAX], text[16384] = ""; /* 36 panes, base places and the view */
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
        if (nseen == 0) fprintf(stderr, "backmux: no terminal is showing panes (backmux shell, or Ctrl-Space Enter while attached)\n");
        else fprintf(stderr, "backmux: several terminals have panes; run backmux %s inside the one you mean\n", verb);
        return -1;
    }
    char req[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX + 8], res[HOLD_PATH_MAX];
    if (hold_checked_snprintf(req, sizeof(req), "%s/mux.%d.ask", dir, (int)seen[pick].pid) != 0 ||
        hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp", req) != 0 ||
        hold_checked_snprintf(res, sizeof(res), "%s/mux.%d.result", dir, (int)seen[pick].pid) != 0)
        return -1;
    unlink(res); /* an answer left from before is not this one's */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || hold_write_all(fd, body, strlen(body)) != 0 || close(fd) != 0 || rename(tmp, req) != 0) {
        if (fd >= 0) unlink(tmp);
        fprintf(stderr, "backmux: error: cannot reach the terminal: %s\n", strerror(errno));
        return -1;
    }
    /* the compositor answers from its event loop */
    for (int waited = 0; waited < wait_ms; waited += 50) {
        int rfd = open(res, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (rfd >= 0) {
            ssize_t n = read(rfd, answer, an - 1);
            close(rfd);
            unlink(res);
            answer[n > 0 ? n : 0] = '\0';
            answer[strcspn(answer, "\n")] = '\0';
            return 0;
        }
        struct timespec sl = {0, 50 * 1000000L};
        nanosleep(&sl, NULL);
    }
    snprintf(answer, an, "error the terminal did not answer");
    return 0;
}

/* backmux swap: two places, names or ids, by the terminal showing them (it
 * resolves them: a place is that screen's, and only it knows which). */
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
            fprintf(stderr, "usage: backmux swap TARGET,TARGET | TARGET TARGET   (places like 2C, process names or IDs)\n");
            return HOLD_EXIT_USAGE;
        }
        snprintf(a, sizeof(a), "%.*s", (int)(comma - argv[0]), argv[0]);
        snprintf(b, sizeof(b), "%s", comma + 1);
    } else {
        snprintf(a, sizeof(a), "%s", argv[0]);
        snprintf(b, sizeof(b), "%s", argv[1]);
    }
    char body[220], answer[240];
    snprintf(body, sizeof(body), "swap %s %s\n", a, b);
    if (ask_terminal(user_store, body, 2000, "swap", answer, sizeof(answer)) != 0) return 1;
    if (!strncmp(answer, "ok", 2)) {
        hold_sig_note(inv, "backmux: swapped %s and %s\n", a, b);
        return 0;
    }
    fprintf(stderr, "backmux: %s\n", !strncmp(answer, "error ", 6) ? answer + 6 : answer);
    return 1;
}

/* backmux layout: the saved layouts (~/.config/backmux/layouts/NAME). list
 * and rm and default work on the files and the config; save and load ask
 * the terminal showing panes, which holds the base. */
int hold_cmd_layout_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc, char **argv) {
    static const char usage[] = "usage: backmux layout list | save NAME | load NAME | rm NAME | default [NAME]\n";
    const char *home = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_home[0] ? inv->invoking_home : getenv("HOME");
    char cfg[HOLD_PATH_MAX], dir[HOLD_PATH_MAX];
    if (argc < 1 || !home || hold_settings_path(home, cfg, sizeof(cfg)) != 0) {
        fputs(usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    *strrchr(cfg, '/') = '\0';
    if (hold_checked_snprintf(dir, sizeof(dir), "%s/layouts", cfg) != 0) return 1;
    const char *verb = argv[0], *name = argc > 1 ? argv[1] : NULL;
    char deflt[ALIAS_MAX_LEN + 1] = "";
    if (hold_settings_get(home, "layout", deflt, sizeof(deflt)) != 0) deflt[0] = '\0';
    if (!strcmp(verb, "list") && !name) {
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (hold_valid_alias(e->d_name)) printf("%s%s\n", e->d_name, !strcmp(e->d_name, deflt) ? "  (default)" : "");
        if (d) closedir(d);
        return 0;
    }
    if (!strcmp(verb, "default") && !name) {
        if (!deflt[0]) return 1;
        printf("%s\n", deflt);
        return 0;
    }
    if (!name || !hold_valid_alias(name) || argc > 2) {
        fputs(usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s", dir, name) != 0) return 1;
    if (!strcmp(verb, "rm")) {
        if (unlink(path) != 0) {
            fprintf(stderr, "backmux: no layout %s\n", name);
            return 1;
        }
        hold_sig_note(inv, "backmux: removed layout %s\n", name);
        return 0;
    }
    if (!strcmp(verb, "default")) {
        if (access(path, R_OK) != 0) {
            fprintf(stderr, "backmux: no layout %s\n", name);
            return 1;
        }
        if (hold_settings_put(home, "layout", name) != 0) {
            fprintf(stderr, "backmux: error: cannot write the config: %s\n", strerror(errno));
            return 1;
        }
        hold_sig_note(inv, "backmux: backmux shell opens with layout %s\n", name);
        return 0;
    }
    if (strcmp(verb, "save") && strcmp(verb, "load")) {
        fputs(usage, stderr);
        return HOLD_EXIT_USAGE;
    }
    char body[160], answer[240];
    snprintf(body, sizeof(body), "layout %s %s\n", verb, name);
    /* a load may start ended processes again: give it time */
    if (ask_terminal(user_store, body, 15000, "layout", answer, sizeof(answer)) != 0) return 1;
    if (!strncmp(answer, "ok", 2)) {
        hold_sig_note(inv, "backmux: %s layout %s\n", !strcmp(verb, "save") ? "saved" : "loaded", name);
        return 0;
    }
    fprintf(stderr, "backmux: %s\n", !strncmp(answer, "error ", 6) ? answer + 6 : answer);
    return 1;
}
