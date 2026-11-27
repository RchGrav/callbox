/* console/agents.c - which processes are agent harnesses.
 *
 * Chat lists only agent harnesses (Claude Code, Codex, ...). Why: chat is
 * for talking to agents; a shell, an editor or a server put on the list was
 * one more name to skip past, and a message typed into one is not a
 * conversation. A process is an agent when the program it was started as,
 * or the program in its terminal's foreground now (a harness started from a
 * backmux shell), is a known harness, or one the person named with
 * --agent (kept in ~/.config/backmux/config as "agents").
 *
 * How a program is named: its executable's file name, without an extension
 * (.js, .py, .exe), or for a script run by an interpreter (node, python,
 * bun, npx, uv ...) the script's - its file name or a directory of its path
 * (node_modules/@github/copilot/index.js is copilot). A known name matches
 * whole, or as the start of a longer one before - or _ (codex-x86_64-linux,
 * gemini-cli, claude-code). */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"

/* The harnesses backmux knows without being told. */
static const char *const known[] = {
    "claude", "codex", "agy", "antigravity", "pi", "hermes", "crush", "opencode", "gemini", "aider", "goose", "copilot",
};

#define AGENTS_EXTRA_MAX 32
#define AGENTS_REREAD_MS 3000
static char g_extra[AGENTS_EXTRA_MAX][AGENT_NAME_MAX];
static int g_nextra;
static char g_home[HOLD_PATH_MAX]; /* whose config to read the list from again */
static int64_t g_read_at;

static int64_t now_ms(void);

void hold_agents_set_extra(const char *list) {
    g_nextra = 0;
    for (const char *p = list ? list : ""; *p && g_nextra < AGENTS_EXTRA_MAX;) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        size_t l = strcspn(p, " ,\t");
        if (l > 0 && l < AGENT_NAME_MAX) {
            memcpy(g_extra[g_nextra], p, l);
            g_extra[g_nextra][l] = '\0';
            g_nextra++;
        }
        p += l;
    }
}

/* A word as a program name: its last path part, lower case, without an
 * extension a launcher adds. */
static void normalize(const char *word, size_t len, char *out, size_t n) {
    const char *slash = NULL;
    for (size_t i = 0; i < len; i++)
        if (word[i] == '/') slash = word + i;
    if (slash) {
        len -= (size_t)(slash + 1 - word);
        word = slash + 1;
    }
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < n; i++) out[k++] = (char)tolower((unsigned char)word[i]);
    out[k] = '\0';
    static const char *const ext[] = {".js", ".mjs", ".cjs", ".ts", ".py", ".exe", ".sh", ".cmd"};
    for (size_t e = 0; e < sizeof(ext) / sizeof(ext[0]); e++) {
        size_t el = strlen(ext[e]);
        if (k > el && !strcmp(out + k - el, ext[e])) {
            out[k - el] = '\0';
            break;
        }
    }
}

static bool name_matches(const char *word, const char *name) {
    size_t l = strlen(name);
    return !strncmp(word, name, l) && (word[l] == '\0' || word[l] == '-' || word[l] == '_');
}

const char *hold_console_home(void) {
    return g_home[0] ? g_home : NULL;
}

void hold_agents_set_home(const char *home) {
    snprintf(g_home, sizeof(g_home), "%s", home ? home : "");
    g_read_at = 0;
}

/* The list again every few seconds: --agent in another terminal adds a
 * harness that a chat already open should list without a restart. */
static void reread_extra(void) {
    if (!g_home[0]) return;
    int64_t now = now_ms();
    if (g_read_at && now - g_read_at < AGENTS_REREAD_MS) return;
    g_read_at = now;
    char list[1024];
    if (hold_settings_get(g_home, "agents", list, sizeof(list)) != 0) list[0] = '\0';
    hold_agents_set_extra(list);
}

bool hold_agent_name(const char *word, char *out, size_t n) {
    char w[HOLD_PATH_MAX];
    if (!word || !*word) return false;
    reread_extra();
    normalize(word, strlen(word), w, sizeof(w));
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (name_matches(w, known[i])) {
            if (out) snprintf(out, n, "%s", known[i]);
            return true;
        }
    for (int i = 0; i < g_nextra; i++)
        if (name_matches(w, g_extra[i])) {
            if (out) snprintf(out, n, "%s", g_extra[i]);
            return true;
        }
    return false;
}

/* Every directory of a script's path, from the file outwards: a package
 * names its directory, not its entry file (.../@github/copilot/index.js). */
static bool path_names_agent(const char *path, char *out, size_t n) {
    if (hold_agent_name(path, out, n)) return true;
    char copy[HOLD_PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *s = strrchr(copy, '/'); s; s = strrchr(copy, '/')) {
        *s = '\0';
        if (copy[0] && hold_agent_name(copy, out, n)) return true;
    }
    return false;
}

static bool is_interpreter(const char *w) {
    static const char *const run[] = {"node", "nodejs", "bun", "bunx", "deno", "npx", "pnpx", "pnpm", "yarn", "tsx",
                                      "ts-node", "uv", "uvx", "pipx", "env", "ruby", "perl"};
    for (size_t i = 0; i < sizeof(run) / sizeof(run[0]); i++)
        if (!strcmp(w, run[i])) return true;
    /* python, python3, python3.12, pypy3 */
    return !strncmp(w, "python", 6) || !strncmp(w, "pypy", 4) || (!strncmp(w, "node", 4) && isdigit((unsigned char)w[4]));
}

static bool is_shell(const char *w) {
    static const char *const sh[] = {"sh", "bash", "zsh", "dash", "fish", "ksh", "mksh"};
    for (size_t i = 0; i < sizeof(sh) / sizeof(sh[0]); i++)
        if (!strcmp(w, sh[i])) return true;
    return false;
}

/* A runner's words that are not the program: its options, a subcommand
 * (uv run, npx exec, pipx run), an environment assignment. */
static bool runner_word(const char *a) {
    static const char *const sub[] = {"run", "exec", "x", "tool", "dlx", "-m"};
    if (a[0] == '-' || strchr(a, '=')) return true;
    for (size_t i = 0; i < sizeof(sub) / sizeof(sub[0]); i++)
        if (!strcmp(a, sub[i])) return true;
    return false;
}

/* The first word of a shell's -c command that runs something: past exec,
 * env and assignments. */
static bool command_names_agent(const char *cmd, char *out, size_t n) {
    char word[HOLD_PATH_MAX];
    for (const char *p = cmd; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '(' || *p == '{') p++;
        size_t l = strcspn(p, " \t;&|)");
        if (l == 0) break;
        snprintf(word, sizeof(word), "%.*s", (int)l, p);
        if (strcmp(word, "exec") && strcmp(word, "env") && strcmp(word, "command") && !strchr(word, '='))
            return path_names_agent(word, out, n);
        p += l;
    }
    return false;
}

bool hold_agent_of_argv(char *const *argv, int argc, char *out, size_t n) {
    if (argc < 1 || !argv || !argv[0]) return false;
    if (hold_agent_name(argv[0], out, n)) return true;
    char w[HOLD_PATH_MAX];
    normalize(argv[0], strlen(argv[0]), w, sizeof(w));
    if (is_shell(w)) {
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-lc") || !strcmp(argv[i], "-ic"))
                return command_names_agent(argv[i + 1], out, n);
        return false;
    }
    if (!is_interpreter(w)) return false;
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || runner_word(argv[i])) continue;
        return path_names_agent(argv[i], out, n);
    }
    return false;
}

bool hold_agent_word(char *const *argv, int argc, char *out, size_t n) {
    if (argc < 1 || !argv || !argv[0]) return false;
    char w[HOLD_PATH_MAX];
    normalize(argv[0], strlen(argv[0]), w, sizeof(w));
    /* a script's own name, not its interpreter's: node is not an agent */
    if (is_interpreter(w))
        for (int i = 1; i < argc; i++)
            if (argv[i] && !runner_word(argv[i])) {
                normalize(argv[i], strlen(argv[i]), w, sizeof(w));
                break;
            }
    if (!w[0] || strlen(w) >= AGENT_NAME_MAX || strchr(w, ' ') || strchr(w, ',')) return false;
    snprintf(out, n, "%s", w);
    return true;
}

static bool pid_runs_agent(pid_t pid, char *out, size_t n) {
    char buf[8192];
    size_t len = 0;
    if (pid <= 0 || hold_proc_cmdline(pid, buf, sizeof(buf), &len) != 0) return false;
    char *argv[64];
    int argc = 0;
    for (size_t i = 0; i < len && argc < 63;) {
        argv[argc++] = buf + i;
        i += strlen(buf + i) + 1;
    }
    argv[argc] = NULL;
    return hold_agent_of_argv(argv, argc, out, n);
}

struct child_hunt {
    char *out;
    size_t n;
    bool found;
};

static int child_cb(pid_t child, void *ctx) {
    struct child_hunt *h = ctx;
    h->found = pid_runs_agent(child, h->out, h->n);
    return h->found;
}

static bool children_run_agent(pid_t pid, char *out, size_t n) {
    struct child_hunt h = {out, n, false};
    (void)hold_proc_for_each_child(pid, child_cb, &h);
    return h.found;
}

/* A few seconds of memory per process: chat's list and the bar ask about
 * every process each second, and a harness does not come and go faster. */
#define AGENT_CACHE 64
#define AGENT_CACHE_MS 2000
static struct {
    pid_t pid;
    int64_t at;
    bool agent;
    char name[AGENT_NAME_MAX];
} g_cache[AGENT_CACHE];

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool hold_agent_of_process(pid_t pid, char *out, size_t n) {
    if (pid <= 0) return false;
    int64_t now = now_ms();
    int slot = (int)((unsigned)pid % AGENT_CACHE);
    if (g_cache[slot].pid == pid && now - g_cache[slot].at < AGENT_CACHE_MS) {
        if (g_cache[slot].agent && out) snprintf(out, n, "%s", g_cache[slot].name);
        return g_cache[slot].agent;
    }
    char name[AGENT_NAME_MAX] = "";
    /* the program itself; what its terminal runs in the foreground now (a
     * shell's job); and a child of either (sh -c 'claude' keeps sh's group) */
    bool agent = pid_runs_agent(pid, name, sizeof(name));
    pid_t fg = 0;
    if (!agent && hold_proc_foreground_group(pid, &fg) == 0 && fg != pid)
        agent = pid_runs_agent(fg, name, sizeof(name)) || children_run_agent(fg, name, sizeof(name));
    if (!agent) agent = children_run_agent(pid, name, sizeof(name));
    g_cache[slot].pid = pid;
    g_cache[slot].at = now;
    g_cache[slot].agent = agent;
    snprintf(g_cache[slot].name, sizeof(g_cache[slot].name), "%s", name);
    if (agent && out) snprintf(out, n, "%s", name);
    return agent;
}

bool hold_agent_of_record(const struct hold_run_record *r, char *out, size_t n) {
    if (!r) return false;
    if (hold_agent_of_argv(r->recipe.argv, r->recipe.argc, out, n)) return true;
    return r->pid > 0 && !r->has_exit_code && hold_agent_of_process(r->pid, out, n);
}
