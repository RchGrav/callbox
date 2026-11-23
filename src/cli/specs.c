/* cli/specs.c — command_specs[] and the flag engine that runs off it.
 *
 * One row per spelled verb: aliases are rows whose canon names the dispatch
 * verb they fold onto. The table owns arity, shared flags, and error usage.
 * Commands with value options parse their own arguments. Help prose lives
 * in src/cli/help; this table selects each command's page and assembles the
 * complete reference so aliases and less-common commands stay discoverable. */
#include "callbox/config.h"
#include <stddef.h>
#include "callbox/types.h"
#include "callbox/cli.h"
#include "callbox/runtime.h"
#include "callbox/core.h"
#include "callbox/help_text.h"

/* Per-command flag mask: which owned flags a verb consumes. */
enum {
    SPEC_ALL       = 1 << 0, /* --all; -a is accepted wherever --all is */
    SPEC_LIVE      = 1 << 1, /* -l/--live, list's running-only filter */
    SPEC_SCOPE     = 1 << 2, /* -s/--system and -u/--user store scope */
    SPEC_FORCE     = 1 << 3, /* --force (purge family) */
    SPEC_PRINT     = 1 << 4, /* --print (end/stop/kill) */
    SPEC_NOSTREAM  = 1 << 5, /* --no-stream (stats) */
    SPEC_STRICT    = 1 << 6, /* reject any unconsumed flag: a target or a name never starts with - */
    SPEC_PS_DOCKER = 1 << 7, /* ps speaks only Docker: --system not consumed */
    SPEC_OWN_HELP  = 1 << 8, /* parses its own arguments, -h among them (send --text -h is text) */
    SPEC_KEYS      = 1 << 9  /* takes --detach-keys KEYS after its target too (attach) */
};

struct hold_cli_command_spec {
    const char *name;
    const char *canon;      /* dispatch verb aliases fold onto; NULL = itself */
    int min_args;
    int max_args;           /* -1 means unbounded. */
    unsigned flags;
    const char *usage;      /* for errors; a command's help page is commands/<canon>.txt */
};

static const char usage_end[] = "usage: callbox end [--print] <target>...";
static const char usage_attach[] = "usage: callbox attach <target> [--detach-keys KEYS]";

#define SPEC_PURGE_FLAGS (SPEC_ALL | SPEC_SCOPE | SPEC_FORCE | SPEC_STRICT)

static const struct hold_cli_command_spec command_specs[] = {
    {"list", NULL, 0, 1, SPEC_ALL | SPEC_LIVE | SPEC_SCOPE | SPEC_STRICT,
     "usage: callbox list [-a|--all] [-s|--system] [-u|--user] [-l|--live] [name]"},
    {"ps", NULL, 0, 1, SPEC_ALL | SPEC_STRICT | SPEC_PS_DOCKER,
     "usage: callbox ps [-a|--all] [name]"},
    {"end", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT, usage_end},
    {"stop", "end", 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: callbox stop [--print] <target>..."},
    {"kill", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: callbox kill [--print] <target>..."},
    {"tail", NULL, 1, 1, SPEC_STRICT, "usage: callbox tail <target>"},
    {"logs", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox logs <target> [--follow|-f] [--tail|-n N]"},
    {"inspect", NULL, 1, 1, SPEC_STRICT, "usage: callbox inspect <target>"},
    {"ports", NULL, 1, 1, SPEC_STRICT, "usage: callbox ports <target>"},
    {"stats", NULL, 1, 1, SPEC_NOSTREAM | SPEC_STRICT, "usage: callbox stats <target> [--no-stream]"},
    {"peek", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox peek <target> [--json] [--ruler] [--last-alt]\n"
     "                 [--image FILE [--no-ruler] [--cursor-ring] [--scale N]\n"
     "                               [--no-footer] [--no-mouse-marker] [--text]]"},
    {"send", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox send <target> [TEXT]... [--text TEXT] [-k KEY]...\n"
     "                 [--keys \"K1 K2\"] [--enter] [--paste TEXT|-] [--stdin] [--seq]\n"
     "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--wait MS [--timeout MS]]"},
    {"wait", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox wait <target> [--idle [MS]] [--match TEXT | --regex RE]\n"
     "                 [--since SEQ] [--exit] [--timeout MS]"},
    {"msg", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox msg <target> TEXT... [--stdin] [--no-enter] [-- TEXT...]\n"
     "                   [--wait-reply [--timeout MS]]\n"
     "       callbox msg [<target>] --received [ID]"},
    {"inbox", NULL, 0, -1, SPEC_OWN_HELP, "usage: callbox inbox [<name>] [--wait | --monitor] [--timeout <ms>]"},
    {"monitor", NULL, 0, -1, SPEC_OWN_HELP, "usage: callbox monitor [NAME] [--timeout MS]"},
    {"resize", NULL, 1, 3, SPEC_OWN_HELP, "usage: callbox resize <target> <cols> <rows>   (or <cols>x<rows>)"},
    {"config", NULL, 0, 4, SPEC_OWN_HELP,
     "usage: callbox config get <setting>\n"
     "       callbox config set <setting> <value>\n"
     "       callbox config unset <setting>\n"
     "settings: detach-size COLS ROWS"},
    /* shell is a launch, parsed in cli/main.c; this row is its help. */
    {"shell", NULL, 0, -1, 0,
     "usage: callbox shell [--name NAME] [--size COLS ROWS] [--chat-key KEY]\n"
     "                  [-e KEY=VALUE]... [--rm] [shell [args...]]"},
    {"attach", NULL, 1, 1, SPEC_STRICT | SPEC_KEYS, usage_attach},
    {"console", "attach", 1, 1, SPEC_STRICT | SPEC_KEYS, "usage: callbox console <target> [--detach-keys KEYS]"},
    {"purge", NULL, 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]"},
    {"prune", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]"},
    {"rm", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [--force]"},
    {"drop", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]"},
    {"save", NULL, 1, 1, SPEC_STRICT, "usage: callbox save <target>"},
    {"rename", NULL, 2, 2, SPEC_STRICT, "usage: callbox rename <target> <name>"},
    {"help", NULL, 0, 1, 0, "usage: callbox help [topic]"},
};

static const struct hold_cli_command_spec *find_public_command_spec(const char *s) {
    if (!s) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        if (!strcmp(command_specs[i].name, s)) {
            return &command_specs[i];
        }
    }
    return NULL;
}

/* The spelled flags the owned sweep can consume; each spec's mask says which
 * apply. -a rides SPEC_ALL: it is accepted wherever --all is. */
static const struct owned_flag {
    const char *shortf; /* NULL when the flag has no short spelling */
    const char *longf;
    unsigned bit;
    size_t out; /* offsetof the bool it sets in hold_cli_owned_opts */
} owned_flags[] = {
    {"-a", "--all", SPEC_ALL, offsetof(struct hold_cli_owned_opts, all)},
    {"-l", "--live", SPEC_LIVE, offsetof(struct hold_cli_owned_opts, live_only)},
    {"-s", "--system", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, system_scope)},
    {"-u", "--user", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, user_scope)},
    {NULL, "--force", SPEC_FORCE, offsetof(struct hold_cli_owned_opts, force)},
    {NULL, "--print", SPEC_PRINT, offsetof(struct hold_cli_owned_opts, print_cmd)},
    {NULL, "--no-stream", SPEC_NOSTREAM, offsetof(struct hold_cli_owned_opts, no_stream)},
};

/* Sweep an owned command's argv: consume the flags its spec allows (plus the
 * global --system/--quiet), collect everything else into cmd_argv. A literal
 * `--` passes the rest through untouched. Strict verbs (list/ps) reject any
 * flag they did not consume rather than misreading it as a name filter.
 * Returns 0, or HOLD_EXIT_USAGE after printing the unknown-flag error and usage. */
int hold_cli_collect_owned_args(const char *command, int argc, char **argv,
                                char **cmd_argv, int *cmd_argc,
                                struct hold_cli_owned_opts *opts) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    unsigned allowed = spec ? spec->flags : 0;
    bool literal = false;
    /* send and msg read a `--` themselves: what follows is text to type,
     * and the verb has to know that to keep it from its own flags. */
    bool keep_delimiter = command && (!strcmp(command, "send") || !strcmp(command, "msg"));
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        if (!literal) {
            if (!strcmp(arg, "--")) {
                literal = true;
                opts->saw_delimiter = true;
                if (keep_delimiter) cmd_argv[(*cmd_argc)++] = arg;
                continue;
            }
            /* --system selects the root-managed store for every owned verb
             * except ps, which speaks only Docker and takes no scope flag. */
            if (!(allowed & SPEC_PS_DOCKER) && !strcmp(arg, "--system")) {
                opts->requested_system = true;
                continue;
            }
            if (!strcmp(arg, "--quiet")) {
                opts->quiet = true;
                continue;
            }
            /* -h anywhere asks for the verb's help - except for the verbs
             * that read their own arguments, where it may be text to send. */
            if (!(allowed & SPEC_OWN_HELP) && (!strcmp(arg, "-h") || !strcmp(arg, "--help"))) {
                opts->want_help = true;
                continue;
            }
            const struct owned_flag *f = NULL;
            for (size_t j = 0; j < sizeof(owned_flags) / sizeof(owned_flags[0]); j++) {
                if ((owned_flags[j].shortf && !strcmp(arg, owned_flags[j].shortf)) ||
                    !strcmp(arg, owned_flags[j].longf)) {
                    f = &owned_flags[j];
                    break;
                }
            }
            if (f && (allowed & f->bit)) {
                *(bool *)((char *)opts + f->out) = true;
                continue;
            }
            if ((allowed & SPEC_KEYS) && !strncmp(arg, "--detach-keys", 13) && (!arg[13] || arg[13] == '=')) {
                cmd_argv[(*cmd_argc)++] = arg; /* its caller reads it, and the value after it */
                if (!arg[13] && i + 1 < argc) cmd_argv[(*cmd_argc)++] = argv[++i];
                continue;
            }
            if ((allowed & SPEC_STRICT) && arg[0] == '-') {
                fprintf(stderr, "callbox: error: unknown flag '%s'\n", arg);
                if (spec) {
                    fprintf(stderr, "%s\n", spec->usage);
                }
                return HOLD_EXIT_USAGE;
            }
        }
        cmd_argv[(*cmd_argc)++] = arg;
    }
    return 0;
}

/* Topic names from earlier releases, kept working: a link or a habit still
 * lands on the page that replaced it. */
static const char *const help_renamed[][2] = {
    {"users", "share"},
    {"comms", "messaging"},
    {"services", "service"},
    {"run", "launch"},
};

/* The complete reference reuses the same pages as per-command help. Only
 * the introduction is authored separately; command coverage and aliases
 * come from the parser's table rather than a second hand-maintained list. */
static int show_all_help(void) {
    static const char *const reference[] = {
        "reference/all", "reference/launch", "reference/targets",
        "reference/system", "reference/scripting"
    };
    for (size_t i = 0; i < sizeof(reference) / sizeof(reference[0]); i++) {
        if (i) putchar('\n');
        if (hold_help_print(reference[i]) != 0) return HOLD_EXIT_USAGE;
    }
    puts("\nCOMMAND REFERENCE\n");
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        const struct hold_cli_command_spec *spec = &command_specs[i];
        if (spec->canon) continue;
        char page[96];
        if (hold_checked_snprintf(page, sizeof(page), "commands/%s", spec->name) != 0 ||
            hold_help_print(page) != 0)
            return HOLD_EXIT_USAGE;
        putchar('\n');
    }
    puts("COMMAND ALIASES");
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++)
        if (command_specs[i].canon)
            printf("  %-12s %s\n", command_specs[i].name, command_specs[i].canon);
    puts("\nHELP TOPIC ALIASES");
    for (size_t i = 0; i < sizeof(help_renamed) / sizeof(help_renamed[0]); i++)
        printf("  %-12s %s\n", help_renamed[i][0], help_renamed[i][1]);
    return 0;
}

/* The help pages are compiled in from src/cli/help (help_text.h): guides,
 * reference, the unlisted extras, and one page per command, which an alias
 * shares with the command it stands for. */
int hold_show_help(const char *topic) {
    if (!topic || !*topic) {
        hold_usage();
        return 0;
    }
    for (size_t i = 0; i < sizeof(help_renamed) / sizeof(help_renamed[0]); i++)
        if (!strcmp(topic, help_renamed[i][0])) topic = help_renamed[i][1];
    if (!strcmp(topic, "all")) return show_all_help();
    static const char *const sections[] = {"guides/", "reference/", "extra/"};
    char name[96];
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
        if (hold_checked_snprintf(name, sizeof(name), "%s%s", sections[i], topic) == 0 && hold_help_print(name) == 0)
            return 0;
    const struct hold_cli_command_spec *spec = find_public_command_spec(topic);
    if (spec && hold_checked_snprintf(name, sizeof(name), "commands/%s", spec->canon ? spec->canon : spec->name) == 0 &&
        hold_help_print(name) == 0)
        return 0;
    fprintf(stderr, "callbox: unknown help topic '%s' (callbox help lists the commands)\n", topic);
    return HOLD_EXIT_USAGE;
}

size_t hold_cli_verb_names(const char **out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]) && n < max; i++)
        out[n++] = command_specs[i].name;
    return n;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL;
}

const char *hold_cli_command_usage(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec ? spec->usage : NULL;
}

const char *hold_cli_command_canon(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    if (!spec) return s;
    return spec->canon ? spec->canon : spec->name;
}

int hold_validate_owned_command_arity(const char *command, int argc) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    if (!spec) {
        return 0;
    }
    if (argc < spec->min_args || (spec->max_args >= 0 && argc > spec->max_args)) {
        fprintf(stderr, "%s\n", spec->usage);
        return HOLD_EXIT_USAGE;
    }
    return 0;
}
