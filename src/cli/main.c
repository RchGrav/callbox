/* cli/main.c — dispatch, the launch-option grammar, and the privilege gates.
 * Per-command flag parsing, arity, usage text, and help live in cli/specs.c;
 * this file owns the bare-form (Docker-dialect) launch path, the value
 * grammars behind the launch options, and the verb dispatch. */
#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/cli.h"
#include "callbox/runtime.h"
#include "callbox/access.h"
#include "callbox/console.h"
#include "callbox/store.h"
#include "callbox/platform.h"
#include "callbox/core.h"

struct cli_run_options {
    bool tail_flag; /* --tail/-f given: follow a named call's log, never redial it */
    bool detach;
    bool wait;
    bool interactive;
    bool tty;
    bool auto_remove;
    const char *name;
    char **env;
    int envc;
    char restart_policy[64];
    int restart_delay_seconds;
    bool restart_delay_seen;
    int size_cols, size_rows; /* --size COLS ROWS: 0 = not given */
    bool chat_key_seen;       /* --chat-key (callbox shell) */
    unsigned char chat_key;
};

static void free_run_options(struct cli_run_options *run) {
    if (!run) return;
    hold_free_argv_alloc(run->env, run->envc);
    memset(run, 0, sizeof(*run));
}

static struct hold_start_options start_options_from_run(const struct cli_run_options *run,
                                                        bool tail,
                                                        bool console_mode,
                                                        bool interactive_stdin,
                                                        int argc,
                                                        char **argv) {
    return (struct hold_start_options){
        .tail = tail,
        .console_mode = console_mode,
        .auto_remove = run->auto_remove,
        .wait = run->wait,
        .interactive_stdin = interactive_stdin,
        .argc = argc,
        .argv = argv,
        .run_name = run->name,
        .envc = run->envc,
        .env = run->env,
        .restart_policy = run->restart_policy[0] ? run->restart_policy : NULL,
        .restart_delay_seconds = run->restart_delay_seen ? run->restart_delay_seconds : HOLD_RESTART_BACKOFF,
        .size_cols = run->size_cols,
        .size_rows = run->size_rows,
    };
}

static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *wait,
                                  bool *interactive,
                                  bool *tty) {
    if (!arg || arg[0] != '-') return false;
    if (!strcmp(arg, "-d") || !strcmp(arg, "--detach")) {
        *detach = true;
        return true;
    }
    if (!strcmp(arg, "-w") || !strcmp(arg, "--wait")) {
        *wait = true;
        return true;
    }
    if (!strcmp(arg, "-i") || !strcmp(arg, "--interactive")) {
        *interactive = true;
        return true;
    }
    if (!strcmp(arg, "-t") || !strcmp(arg, "--tty")) {
        *tty = true;
        return true;
    }
    /* Clusters of the boolean short flags: -it, -dt, -dw, -dit, ... */
    if (arg[1] && arg[1] != '-' && arg[2] && strspn(arg + 1, "ditw") == strlen(arg + 1)) {
        for (const char *p = arg + 1; *p; p++) {
            if (*p == 'd') *detach = true;
            else if (*p == 'w') *wait = true;
            else if (*p == 'i') *interactive = true;
            else *tty = true;
        }
        return true;
    }
    return false;
}

/* ---- value grammars behind the launch options (spec surface; frozen) ---- */

static int parse_restart_policy_arg(const char *arg, char out[64]) {
    if (!arg || !*arg) {
        fprintf(stderr, "callbox: error: --restart requires a policy\n");
        return HOLD_EXIT_USAGE;
    }
    if (!strcmp(arg, "no") || !strcmp(arg, "always") || !strcmp(arg, "unless-stopped")) {
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    if (!strncmp(arg, "on-failure", 10) && (arg[10] == '\0' || arg[10] == ':')) {
        if (arg[10] == ':') {
            char *end = NULL;
            long n = strtol(arg + 11, &end, 10);
            if (!end || *end || n < 0 || n > INT_MAX) {
                fprintf(stderr, "callbox: error: invalid --restart on-failure retry count '%s'\n", arg + 11);
                return HOLD_EXIT_USAGE;
            }
        }
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    fprintf(stderr, "callbox: error: invalid --restart policy '%s'\n", arg);
    return HOLD_EXIT_USAGE;
}

static bool restart_policy_arg_enabled(const char *arg) {
    return arg && *arg && strcmp(arg, "no") != 0;
}

static int parse_restart_delay_arg(const char *arg, int *out) {
    if (!arg || !*arg) {
        fprintf(stderr, "callbox: error: --restart-delay requires seconds\n");
        return HOLD_EXIT_USAGE;
    }
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) {
        fprintf(stderr, "callbox: error: invalid --restart-delay '%s'\n", arg);
        return HOLD_EXIT_USAGE;
    }
    *out = (int)n;
    return 0;
}

/* One detach-key token: a literal byte, ^X, or ctrl-x / ctrl-X. */
static int detach_key_token_value(const char *token, size_t len, unsigned char *out) {
    if (!token || len == 0 || !out) return -1;
    if (len == 1) {
        *out = (unsigned char)token[0];
        return 0;
    }
    if (len == 2 && token[0] == '^' && token[1] >= '@' && token[1] <= '_') {
        *out = (unsigned char)(token[1] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-') {
        if (token[5] >= '@' && token[5] <= '_') {
            *out = (unsigned char)(token[5] - '@');
            return 0;
        }
        if (token[5] >= 'a' && token[5] <= 'z') {
            *out = (unsigned char)(token[5] - 'a' + 1);
            return 0;
        }
    }
    return -1;
}

static int configure_detach_keys_option(const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "callbox: error: --detach-keys requires a key sequence\n");
        return HOLD_EXIT_USAGE;
    }
    unsigned char keys[8];
    size_t nkeys = 0;
    const char *p = value;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        if (nkeys >= sizeof(keys) || detach_key_token_value(start, (size_t)(p - start), &keys[nkeys]) != 0) {
            fprintf(stderr, "callbox: error: invalid --detach-keys sequence '%s'\n", value);
            return HOLD_EXIT_USAGE;
        }
        nkeys++;
    }
    if (nkeys == 0 || hold_console_set_detach_keys(keys, nkeys) != 0) {
        fprintf(stderr, "callbox: error: invalid --detach-keys sequence '%s'\n", value);
        return HOLD_EXIT_USAGE;
    }
    return 0;
}

static int append_env_assignment(const char *arg, char ***env_out, int *envc_out) {
    const char *eq = arg ? strchr(arg, '=') : NULL;
    if (!arg || !*arg || !eq || eq == arg) {
        fprintf(stderr, "callbox: error: expected KEY=VALUE after --env/-e\n");
        return HOLD_EXIT_USAGE;
    }
    size_t key_len = (size_t)(eq - arg);
    char key[256];
    if (key_len >= sizeof(key)) {
        fprintf(stderr, "callbox: error: environment key is too long\n");
        return HOLD_EXIT_USAGE;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; key[i]; i++) {
        if (key[i] == '=') {
            fprintf(stderr, "callbox: error: invalid environment key\n");
            return HOLD_EXIT_USAGE;
        }
    }
    char *copy = strdup(arg);
    if (!copy) return 1;
    char **next = realloc(*env_out, ((size_t)*envc_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 1;
    }
    next[*envc_out] = copy;
    *env_out = next;
    (*envc_out)++;
    return 0;
}

static int append_env_file(const char *path, char ***env_out, int *envc_out) {
    if (!path || !*path) {
        fprintf(stderr, "callbox: error: --env-file requires a path\n");
        return HOLD_EXIT_USAGE;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "callbox: error: cannot read env file '%s': %s\n", path, strerror(errno));
        return HOLD_EXIT_USAGE;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t nr;
    unsigned long lineno = 0;
    int rc = 0;
    while ((nr = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (nr > 0 && (line[nr - 1] == '\n' || line[nr - 1] == '\r')) {
            line[--nr] = '\0';
        }
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        rc = append_env_assignment(p, env_out, envc_out);
        if (rc != 0) {
            fprintf(stderr, "callbox: error: invalid --env-file entry at %s:%lu\n", path, lineno);
            break;
        }
    }
    free(line);
    if (ferror(f) && rc == 0) {
        fprintf(stderr, "callbox: error: failed reading env file '%s': %s\n", path, strerror(errno));
        rc = HOLD_EXIT_USAGE;
    }
    fclose(f);
    return rc;
}

/* ---- launch options: one table drives both spellings of every value flag ---- */

enum run_opt_id { OPT_NAME, OPT_ENV, OPT_ENV_FILE, OPT_DETACH_KEYS, OPT_CHAT_KEY, OPT_RESTART, OPT_RESTART_DELAY };

/* Each is spelled `--flag value`; rows with eq_form also take `--flag=value`,
 * as Docker's do. */
static const struct run_value_opt {
    const char *shortf;
    const char *longf;
    bool eq_form;
    enum run_opt_id id;
} run_value_opts[] = {
    {NULL, "--name", true, OPT_NAME},
    {"-e", "--env", true, OPT_ENV},
    {NULL, "--env-file", true, OPT_ENV_FILE},
    {NULL, "--detach-keys", true, OPT_DETACH_KEYS},
    {NULL, "--chat-key", true, OPT_CHAT_KEY},
    {NULL, "--restart", true, OPT_RESTART},
    {NULL, "--restart-delay", true, OPT_RESTART_DELAY},
};

/* A call may not be named after a verb: at the top level the verb always
 * wins, so `callbox list` could never redial a call named list - it would
 * be reachable only by its id. Refused when the name is given, where the
 * fix (another name) is obvious, rather than leaving a call half-reachable.
 * Calls already named so keep their names. */
static int refuse_verb_name(const char *name) {
    if (!name || !hold_cli_command_is_parser_owned(name)) return 0;
    fprintf(stderr, "callbox: error: '%s' is a callbox command; a call named so could not be redialed by name\n",
            name);
    return HOLD_EXIT_USAGE;
}

static int apply_run_value_opt(enum run_opt_id id, const char *value, struct cli_run_options *run) {
    switch (id) {
    case OPT_NAME:
        if (refuse_verb_name(value) != 0) return HOLD_EXIT_USAGE;
        run->name = value;
        return 0;
    case OPT_ENV:
        return append_env_assignment(value, &run->env, &run->envc);
    case OPT_ENV_FILE:
        return append_env_file(value, &run->env, &run->envc);
    case OPT_DETACH_KEYS:
        return configure_detach_keys_option(value);
    case OPT_CHAT_KEY:
        /* The key pressed after the lead key to open chat (the default is
         * ctrl-t): one control key, spelled as a detach key is - ^g, ctrl-g. */
        if (!value || detach_key_token_value(value, strlen(value), &run->chat_key) != 0 || run->chat_key == 0 ||
            run->chat_key >= 0x20 || run->chat_key == '\r' || run->chat_key == '\n' || run->chat_key == '\t' ||
            run->chat_key == 0x1b) {
            fprintf(stderr, "callbox: error: --chat-key takes one control key, such as ctrl-t or ^g\n");
            return HOLD_EXIT_USAGE;
        }
        run->chat_key_seen = true;
        return 0;
    case OPT_RESTART:
        return parse_restart_policy_arg(value, run->restart_policy);
    case OPT_RESTART_DELAY: {
        int rc = parse_restart_delay_arg(value, &run->restart_delay_seconds);
        if (rc == 0) run->restart_delay_seen = true;
        return rc;
    }
    }
    return 1;
}

static int parse_run_options(int argc,
                             char **argv,
                             int *index,
                             struct cli_run_options *run,
                             bool *tail,
                             bool *console_mode,
                             bool *parsed) {
    const char *arg = argv[*index];
    *parsed = true;
    if (parse_docker_run_flag(arg, &run->detach, &run->wait, &run->interactive, &run->tty)) {
        if (run->tty) *console_mode = true;
        (*index)++;
        return 0;
    }
    /* Substrate flags Callbox cannot honor fail honestly, never accept-and-ignore. */
    if (!strcmp(arg, "-p") || !strcmp(arg, "--publish") ||
        !strcmp(arg, "-P") || !strcmp(arg, "--publish-all") ||
        !strncmp(arg, "--publish=", 10)) {
        fprintf(stderr,
                "callbox: error: Callbox is not containerized and does not publish or forward ports; listening ports are observed automatically and shown in `callbox ps`\n");
        return HOLD_EXIT_USAGE;
    }
    if (!strcmp(arg, "-v") || !strcmp(arg, "--volume") || !strncmp(arg, "--volume=", 9)) {
        fprintf(stderr,
                "callbox: error: Callbox is not containerized and does not mount or remap volumes; pass host paths directly (prefer absolute paths)\n");
        return HOLD_EXIT_USAGE;
    }
    if (!strcmp(arg, "--privileged")) {
        /* Rejected, never remapped (docs/docker-familiarity-contract.md). */
        fprintf(stderr,
                "callbox: error: --privileged is not supported; callbox launches with the invoker's rights.\n"
                "  To place a call in the root-managed store, run callbox as root (or with sudo).\n");
        return HOLD_EXIT_USAGE;
    }
    if (!strcmp(arg, "--cap-add") || !strncmp(arg, "--cap-add=", 10) ||
        !strcmp(arg, "--cap-drop") || !strncmp(arg, "--cap-drop=", 11)) {
        /* Grants-era remnant: capabilities left with that subsystem. Reject
         * honestly like the other substrate flags rather than accept-and-ignore. */
        fprintf(stderr,
                "callbox: error: %s is not supported; callbox does not manage capabilities.\n"
                "  Constrain the command itself instead, e.g. via capsh or systemd-run.\n",
                arg);
        return HOLD_EXIT_USAGE;
    }
    for (size_t i = 0; i < sizeof(run_value_opts) / sizeof(run_value_opts[0]); i++) {
        const struct run_value_opt *opt = &run_value_opts[i];
        size_t longn = strlen(opt->longf);
        int rc;
        if ((opt->shortf && !strcmp(arg, opt->shortf)) || !strcmp(arg, opt->longf)) {
            if (*index + 1 >= argc) {
                fprintf(stderr, "usage: callbox [flags] <cmd|id|name> [args...]\n");
                return HOLD_EXIT_USAGE;
            }
            rc = apply_run_value_opt(opt->id, argv[*index + 1], run);
            if (rc != 0) return rc;
            *index += 2;
            return 0;
        }
        if (opt->eq_form && !strncmp(arg, opt->longf, longn) && arg[longn] == '=') {
            rc = apply_run_value_opt(opt->id, arg + longn + 1, run);
            if (rc != 0) return rc;
            (*index)++;
            return 0;
        }
    }
    if (!strcmp(arg, "--size") || !strncmp(arg, "--size=", 7)) {
        /* --size COLS ROWS (or COLSxROWS, or --size=...): the same grammar
         * and bounds as callbox resize and the saved detach-size. */
        bool eq = arg[6] == '=';
        int at = eq ? *index : *index + 1;
        const char *first = eq ? arg + 7 : (at < argc ? argv[at] : NULL);
        const char *second = at + 1 < argc ? argv[at + 1] : NULL;
        char why[160];
        int used = hold_parse_term_size(first, second, &run->size_cols, &run->size_rows, why, sizeof(why));
        if (used < 0) {
            fprintf(stderr, "callbox: error: --size: %s\n", why);
            return HOLD_EXIT_USAGE;
        }
        *index = at + used;
        return 0;
    }
    if (!strcmp(arg, "--rm")) {
        run->auto_remove = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--tail") || !strcmp(arg, "-f")) {
        *tail = true;
        run->tail_flag = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--console")) {
        *console_mode = true;
        (*index)++;
        return 0;
    }
    *parsed = false;
    return 0;
}

/* callbox shell with no shell named: $SHELL, else the account's login shell,
 * else /bin/sh. */
static const char *default_shell(char *buf, size_t n) {
    const char *sh = getenv("SHELL");
    if (sh && sh[0] == '/' && access(sh, X_OK) == 0) return sh;
    struct hold_passwd_entry pw;
    if (hold_lookup_passwd_by_uid(getuid(), &pw) == 0 && pw.shell[0] == '/' && access(pw.shell, X_OK) == 0 &&
        hold_checked_snprintf(buf, n, "%s", pw.shell) == 0)
        return buf;
    return "/bin/sh";
}

static const char shell_usage[] =
    "usage: callbox shell [--name NAME] [--size COLS ROWS] [--chat-key KEY]\n"
    "                  [-e KEY=VALUE]... [--rm] [shell [args...]]\n";

int hold_cli_main(int argc, char **argv) {
    if (argc < 2) {
        /* Docker parity: bare invocation prints help. */
        hold_usage();
        return 0;
    }

    int argi = 1;
    bool tail = false;
    bool console_mode = false;
    bool force_raw = false;
    const char *launch_flag = NULL; /* the first option that only makes sense when placing a call */
    struct cli_run_options run = {0};
    struct hold_cli_owned_opts oo = {0};

    while (argi < argc) {
        bool parsed_run_option = false;
        const char *token = argv[argi];
        int option_rc = parse_run_options(argc, argv, &argi, &run, &tail, &console_mode, &parsed_run_option);
        if (option_rc != 0) {
            free_run_options(&run);
            return option_rc;
        }
        if (parsed_run_option) {
            /* (the detach keys are also an attach's to set) */
            if (!launch_flag && strncmp(token, "--detach-keys", 13) != 0) launch_flag = token;
            continue;
        }
        if (!strcmp(argv[argi], "--system")) {
            oo.requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--quiet")) {
            oo.quiet = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--")) {
            force_raw = true;
            argi++;
            break;
        }
        break;
    }

    /* A shortened verb or call name (see hold_shorten_word for the order and
     * the why): it becomes the whole word before anything else looks -
     * before the shell check too, so `callbox she` is callbox shell (it
     * printed the help: the word became "shell" after that check). */
    static char short_name[ALIAS_MAX_LEN + 1];
    if (!force_raw && !tail && !run.name && argi < argc &&
        !hold_cli_command_is_parser_owned(argv[argi])) {
        const char *verbs[64];
        size_t nverbs = hold_cli_verb_names(verbs, 64);
        const char *verb = NULL;
        int sr = hold_shorten_word(argv[argi], argi == argc - 1, verbs, nverbs, &verb, short_name, sizeof(short_name));
        if (sr == 6) {
            free_run_options(&run);
            return 6;
        }
        if (sr == 1) argv[argi] = (char *)verb;
        if (sr == 2) argv[argi] = short_name;
    }
    /* callbox shell [options] [shell [args...]]: a person's shell on a PTY, with
     * the chat overlay. Callbox's options end at the shell's name; every word
     * after it is the shell's own, so no `--` is needed (one is accepted). */
    bool shell_mode = false;
    char shell_buf[HOLD_PATH_MAX];
    char *shell_argv[2] = {NULL, NULL};
    if (argi < argc && !force_raw && !tail && !strcmp(argv[argi], "shell")) {
        shell_mode = true;
        argi++;
        while (argi < argc) {
            bool parsed_run_option = false;
            if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
                free_run_options(&run);
                return hold_show_help("shell");
            }
            int option_rc = parse_run_options(argc, argv, &argi, &run, &tail, &console_mode, &parsed_run_option);
            if (option_rc != 0) {
                free_run_options(&run);
                return option_rc;
            }
            if (parsed_run_option) continue;
            if (!strcmp(argv[argi], "--system")) {
                oo.requested_system = true;
                argi++;
                continue;
            }
            if (!strcmp(argv[argi], "--quiet")) {
                oo.quiet = true;
                argi++;
                continue;
            }
            if (!strcmp(argv[argi], "--")) {
                argi++;
                break;
            }
            if (argv[argi][0] == '-') {
                fprintf(stderr, "callbox: error: unknown flag '%s' before the shell's name\n%s", argv[argi], shell_usage);
                free_run_options(&run);
                return HOLD_EXIT_USAGE;
            }
            break;
        }
        if (run.detach || run.wait || tail || run.restart_policy[0]) {
            fprintf(stderr, "callbox: error: callbox shell is a shell you sit at: -d, -w, --tail and --restart do not apply "
                            "(a program to drive from a script is callbox -dt <cmd>)\n");
            free_run_options(&run);
            return HOLD_EXIT_USAGE;
        }
        run.tty = true;
        console_mode = true;
    } else if (run.chat_key_seen) {
        fprintf(stderr, "callbox: error: --chat-key applies to callbox shell\n");
        free_run_options(&run);
        return HOLD_EXIT_USAGE;
    }

    if (argi >= argc && !shell_mode) {
        fprintf(stderr, "usage: callbox [flags] <cmd|id|name> [args...]\n");
        return HOLD_EXIT_USAGE;
    }
    /* A word that starts with a dash where the command belongs is a mistyped
     * flag, not a program to run. */
    if (!shell_mode && !force_raw && argv[argi][0] == '-' && argv[argi][1] && strcmp(argv[argi], "--version") != 0 &&
        strcmp(argv[argi], "-V") != 0 && strcmp(argv[argi], "--help") != 0 && strcmp(argv[argi], "-h") != 0) {
        fprintf(stderr, "callbox: error: unknown flag '%s'\n"
                        "  callbox help launch lists the flags for placing a call;\n"
                        "  to run a command whose name starts with a dash, put -- before it\n",
                argv[argi]);
        free_run_options(&run);
        return HOLD_EXIT_USAGE;
    }

    /* Whether the invocation named a session mode explicitly, captured before the
     * bare-form tail defaulting below rewrites `tail`. On redial this decides
     * whether the recipe's recorded mode applies or the invocation overrides it. */
    bool explicit_session_mode = run.detach || run.interactive || run.tty || console_mode || tail;

    bool owned = !shell_mode && !force_raw && !tail && hold_cli_command_is_parser_owned(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 1;
        }
        int flag_rc = hold_cli_collect_owned_args(command, argc - argi, argv + argi,
                                                  cmd_argv, &cmd_argc, &oo);
        if (flag_rc != 0) {
            free(cmd_argv);
            free_run_options(&run);
            return flag_rc;
        }
    } else {
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
        if (shell_mode && cmd_argc == 0) {
            shell_argv[0] = (char *)default_shell(shell_buf, sizeof(shell_buf));
            cmd_argc = 1;
            cmd_argv = shell_argv;
        }
    }

    if (!owned && !shell_mode && !force_raw && !tail && (!strcmp(argv[argi], "--version") || !strcmp(argv[argi], "-V"))) {
        puts(HOLD_VERSION);
        return 0;
    }
    if (!owned && !shell_mode && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        hold_usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: callbox help [topic]\n");
            rc = HOLD_EXIT_USAGE;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = hold_show_help(NULL);
        } else {
            rc = hold_show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && (oo.want_help || (!oo.saw_delimiter && cmd_argc == 1 &&
                                   (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))))) {
        int rc = hold_show_help(command);
        free(cmd_argv);
        free_run_options(&run);
        return rc;
    }
    /* Options for placing a call mean nothing to a verb; accepted and
     * ignored, they would only hide a mistake. */
    if (owned && launch_flag) {
        fprintf(stderr, "callbox: error: %s is for placing a call; it does not apply to 'callbox %s'\n", launch_flag, command);
        free(cmd_argv);
        free_run_options(&run);
        return HOLD_EXIT_USAGE;
    }
    /* callbox attach <target> --detach-keys KEYS: after the target too. */
    if (owned && !strcmp(hold_cli_command_canon(command), "attach")) {
        int kept = 0;
        for (int i = 0; i < cmd_argc; i++) {
            const char *value = NULL;
            if (!strcmp(cmd_argv[i], "--detach-keys") && i + 1 < cmd_argc) value = cmd_argv[++i];
            else if (!strncmp(cmd_argv[i], "--detach-keys=", 14)) value = cmd_argv[i] + 14;
            else {
                cmd_argv[kept++] = cmd_argv[i];
                continue;
            }
            int keys_rc = configure_detach_keys_option(value);
            if (keys_rc != 0) {
                free(cmd_argv);
                free_run_options(&run);
                return keys_rc;
            }
        }
        cmd_argc = kept;
    }
    if (owned) {
        int arity_rc = hold_validate_owned_command_arity(command, cmd_argc);
        if (arity_rc != 0) {
            free(cmd_argv);
            return arity_rc;
        }
    }
    if (run.restart_delay_seen && !restart_policy_arg_enabled(run.restart_policy)) {
        fprintf(stderr, "callbox: error: --restart-delay requires --restart with an active policy\n");
        if (owned) free(cmd_argv);
        free_run_options(&run);
        return HOLD_EXIT_USAGE;
    }

    struct hold_invocation inv;
    if (hold_detect_invocation(&inv, oo.requested_system) != 0) {
        hold_die_errno("callbox: failed to resolve invocation context");
    }
    inv.quiet = oo.quiet;
    /* The bare form launches (or redials) a call and speaks Docker's output
     * dialect: foreground streams and prints nothing of its own; -d prints the
     * bare 64-hex call id. Management verbs keep their own output. */
    inv.docker_run = !owned;
    if (!owned) {
        if (!run.detach && (!console_mode || run.tty)) {
            tail = true;
        }
        if (run.detach) {
            tail = false;
        }
    }
    bool interactive_stdin = run.interactive && !run.tty;

    /* Aliases fold onto their canonical verb (the specs table); dispatch below
     * keys on canon. ps is deliberately NOT folded onto list: they diverge
     * (ps mirrors Docker's machine-wide running view, list is Callbox's scoped
     * ledger). */
    const char *canon = owned ? hold_cli_command_canon(command) : NULL;
    /* Both read views may look at the system store without being root; only
     * acting on it requires root. */
    bool is_read_view = owned && (!strcmp(canon, "list") || !strcmp(canon, "ps"));

    /* The root-managed store is visible to everyone (the read views stay open),
     * but only root may act on it, so a normal user asking for it is refused -
     * except purge, which re-runs itself through sudo below so sudo can ask for
     * the password. */
    if (oo.requested_system && !inv.euid_root && !is_read_view && !(owned && !strcmp(canon, "purge"))) {
        fprintf(stderr, "callbox: error: the root-managed store requires root; re-run as root or with sudo\n");
        if (owned) free(cmd_argv);
        free_run_options(&run);
        return 3;
    }

    struct hold_store user_store;
    struct hold_store system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (hold_init_system_store(&system_store) != 0) {
        hold_die_errno("callbox: failed to resolve system storage");
    }

    if (!inv.euid_root) {
        if (hold_ensure_user_store_for_current_user(&user_store) != 0) {
            hold_die_errno("callbox: failed to init user storage");
        }
    } else {
        /* Root has no store of its own in the normal flow (its calls are
         * system-managed), but list -u / purge -u still need a personal store
         * handle: the invoking user's under sudo, else root's own home. The
         * paths are resolved without creating anything; an absent store simply
         * lists empty. */
        const char *home = inv.have_sudo_user && inv.invoking_home[0] ? inv.invoking_home : getenv("HOME");
        if (home && *home) {
            (void)hold_init_user_store_from_home(home, &user_store);
        }
    }

    if (!owned) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, oo.requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || oo.requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("callbox: failed to init invoking-user storage");
            }
            hold_die_errno("callbox: failed to init start storage");
        }
        /* --tail <call>: follow that call's log, as `callbox tail` does - no
         * PTY, no redial (Ctrl-C stops following; the call keeps running).
         * A word that names no call is still a command to launch. */
        if (run.tail_flag && !force_raw && !shell_mode && !run.name && cmd_argc == 1) {
            int rc = hold_cmd_tail_if_call(&inv, &user_store, &system_store, cmd_argv[0]);
            if (rc >= 0) {
                free_run_options(&run);
                return rc;
            }
        }
        /* Redial: a lone token that resolves to a retained call (id then name)
         * restarts it from its recipe. `--`, --name, and multi-token argv all
         * force a fresh command launch (resolved on PATH and run directly). */
        if (!force_raw && !shell_mode && !run.name && cmd_argc == 1) {
            bool redialed = false;
            int rc = hold_cmd_redial(&inv, &user_store, &system_store, tail, console_mode,
                                     run.auto_remove, run.wait, interactive_stdin, explicit_session_mode,
                                     run.restart_policy[0] ? run.restart_policy : NULL,
                                     run.restart_delay_seen ? run.restart_delay_seconds : HOLD_RESTART_BACKOFF,
                                     run.size_cols, run.size_rows,
                                     cmd_argv[0], &redialed);
            if (redialed) {
                free_run_options(&run);
                return rc;
            }
        }
        /* Resolution fell through to a PATH command: exec argv directly (a
         * missing command fails without leaving a record), matching the doc's
         * "call id -> call name -> PATH command" order. Shell snippets go
         * through an explicit `callbox -- sh -c ...`. */
        /* User mode: a program placed with no session flags runs attached on
         * a PTY, as `callbox shell` does for a shell - with the chat window,
         * so messages for the call go there and are never typed into the
         * program (callbox send still types into it) - and redial brings
         * that program back. It is how a person runs a terminal program.
         * Any switch says otherwise: -it, -d, -dt, -dw, -i. Why -it is not
         * user mode: a program run with -it (an agent, say) must get its
         * messages typed in; the window is only for the switchless launch
         * and callbox shell. Without a terminal there is nowhere to draw
         * the window: the program still runs attached on its PTY. */
        bool restart_asked = run.restart_policy[0] && strcmp(run.restart_policy, "no") != 0;
        bool bare = !run.detach && !run.interactive && !run.tty && !console_mode && !run.tail_flag && !run.wait &&
                    !restart_asked;
        bool user_mode = !shell_mode && bare;
        bool on_terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
        if (user_mode) {
            run.tty = true;
            console_mode = true;
            tail = true;
            interactive_stdin = false;
        }
        struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                interactive_stdin, cmd_argc, cmd_argv);
        /* The chat window opens with the lead key, then the chat key: it needs
         * detach keys that are a lead key plus one, with the chat key distinct
         * (custom --detach-keys such as ctrl-a leave no room - the program
         * then runs attached without the window, as -it always has). */
        unsigned char dk[8];
        size_t ndk = hold_console_get_detach_keys(dk, sizeof(dk));
        unsigned char ck = run.chat_key ? run.chat_key : HOLD_CHAT_KEY_DEFAULT;
        bool keys_fit_chat = ndk == 2 && ck != dk[0] && ck != dk[1];
        opts.chat = shell_mode || (user_mode && on_terminal && keys_fit_chat);
        opts.user_mode = user_mode && keys_fit_chat;
        opts.shell = shell_mode;
        opts.chat_key = run.chat_key;
        int rc = hold_perform_start_options(&inv, &start_store, &opts);
        free_run_options(&run);
        return rc;
    }

    int rc = 0;
    if (!strcmp(canon, "save")) {
        rc = hold_cmd_save_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "rename")) {
        rc = refuse_verb_name(cmd_argv[1]);
        if (rc == 0) rc = hold_cmd_rename_action(&inv, &user_store, &system_store, cmd_argv[0], cmd_argv[1]);
    } else if (!strcmp(canon, "list") || !strcmp(canon, "ps")) {
        const char *name_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (name_filter && !hold_valid_alias(name_filter)) {
            fprintf(stderr, "callbox: error: invalid name '%s'\n", name_filter);
            rc = 5;
        } else if (!strcmp(canon, "ps")) {
            /* ps is Docker's machine-wide running view: both scopes, no USER
             * column, only the -a (include-ended) flag. */
            rc = hold_cmd_ps(&inv, &user_store, &system_store, name_filter, oo.all);
        } else {
            /* list is Callbox's scoped ledger. The scope flags select which stores
             * it draws from; the command resolves DEFAULT against privilege. */
            enum hold_list_scope scope = HOLD_LIST_SCOPE_DEFAULT;
            if (oo.user_scope) {
                scope = HOLD_LIST_SCOPE_USER;
            } else if (oo.system_scope || oo.requested_system) {
                scope = HOLD_LIST_SCOPE_SYSTEM;
            } else if (oo.all) {
                scope = HOLD_LIST_SCOPE_BOTH;
            }
            rc = hold_cmd_list(&inv, &user_store, &system_store, name_filter, scope, oo.live_only);
        }
    } else if (!strcmp(canon, "tail")) {
        rc = hold_cmd_tail_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "inspect")) {
        rc = hold_cmd_inspect_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "ports")) {
        rc = hold_cmd_ports_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "stats")) {
        rc = hold_cmd_stats_action(&inv, &user_store, &system_store, cmd_argv[0], oo.no_stream);
    } else if (!strcmp(canon, "logs")) {
        rc = hold_cmd_view_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "peek")) {
        rc = hold_cmd_peek_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "send")) {
        rc = hold_cmd_send_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "wait")) {
        rc = hold_cmd_wait_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "msg")) {
        rc = hold_cmd_msg_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "inbox")) {
        rc = hold_cmd_inbox_action(&inv, &user_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "monitor")) {
        rc = hold_cmd_monitor_action(&inv, &user_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "resize")) {
        rc = hold_cmd_resize_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "config")) {
        rc = hold_cmd_config_action(&inv, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "attach")) {
        rc = hold_cmd_console_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "purge")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        if (target && target[0] == '-') {
            fprintf(stderr, "callbox: error: unknown flag '%s'\n", target);
            const char *usage = hold_cli_command_usage(command);
            if (usage) fprintf(stderr, "%s\n", usage);
            rc = 1;
        } else {
            /* -u/--user forces the personal sweep; -s/--system (or --system)
             * sweeps the global store. -a keeps its state meaning here
             * (include stale). */
            bool purge_system = (oo.system_scope || oo.requested_system) && !oo.user_scope;
            if (purge_system && !target && !inv.euid_root) {
                /* A non-root global sweep re-execs through sudo so sudo prompts
                 * for the password. This is NOT the deleted elevation
                 * subsystem: no sudoers pinning, no capability tokens — one
                 * auditable execvp, whose root child re-enters here already
                 * privileged and sweeps directly. */
                char self[HOLD_PATH_MAX];
                if (hold_resolve_self_executable_path(argv[0], self, sizeof(self)) == 0) {
                    char *sudo_argv[8];
                    int k = 0;
                    sudo_argv[k++] = "sudo";
                    sudo_argv[k++] = self;
                    sudo_argv[k++] = "purge";
                    sudo_argv[k++] = "--system";
                    if (oo.all) sudo_argv[k++] = "--all";
                    if (oo.force) sudo_argv[k++] = "--force";
                    sudo_argv[k] = NULL;
                    execvp("sudo", sudo_argv);
                }
                fprintf(stderr, "callbox: global purge needs root: sudo callbox purge --system\n");
                rc = 3;
            } else {
                rc = hold_cmd_purge_action(&inv, &user_store, &system_store, target, oo.all, oo.force, purge_system);
            }
        }
    } else if (!strcmp(canon, "end")) {
        rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "stop", cmd_argc, cmd_argv, SIGTERM, true, false, oo.print_cmd);
    } else if (!strcmp(canon, "kill")) {
        rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "kill", cmd_argc, cmd_argv, SIGKILL, false, false, oo.print_cmd);
    } else {
        hold_usage();
        rc = 1;
    }
    if (rc == HOLD_RC_SHOW_HELP) rc = hold_show_help(canon);
    free(cmd_argv);
    return rc;
}
