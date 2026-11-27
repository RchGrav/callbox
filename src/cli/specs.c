/* cli/specs.c - command metadata, shared option parsing, and help lookup.
 *
 * Each accepted command has a row; aliases name their canonical dispatch
 * command. The table defines arity, shared options, and error usage.
 * Commands with value options parse their own arguments. Help prose lives
 * in src/cli/help; this table selects each command's page and assembles the
 * complete reference so aliases and less-common commands stay discoverable. */
#include "backmux/config.h"
#include <stddef.h>
#include "backmux/types.h"
#include "backmux/cli.h"
#include "backmux/runtime.h"
#include "backmux/core.h"
#include "backmux/help_text.h"

/* Shared options accepted by each command. */
enum {
    SPEC_ALL       = 1 << 0, /* --all; -a is accepted wherever --all is */
    SPEC_LIVE      = 1 << 1, /* -l/--live: running and Unknown list records */
    SPEC_SCOPE     = 1 << 2, /* -s/--system and -u/--user store scope */
    SPEC_FORCE     = 1 << 3, /* --force (purge family) */
    SPEC_PRINT     = 1 << 4, /* --print (end/stop/kill) */
    SPEC_NOSTREAM  = 1 << 5, /* --no-stream (stats) */
    SPEC_STRICT    = 1 << 6, /* reject any unconsumed flag: a target or a name never starts with - */
    SPEC_PS_DOCKER = 1 << 7, /* ps does not accept --system */
    SPEC_OWN_HELP  = 1 << 8, /* parses its own arguments, -h among them (send --text -h is text) */
    SPEC_KEYS      = 1 << 9  /* takes --detach-keys KEYS after its target too (attach) */
};

struct hold_cli_command_spec {
    const char *name;
    const char *canon;      /* canonical dispatch command; NULL = itself */
    int min_args;
    int max_args;           /* -1 means unbounded. */
    unsigned flags;
    const char *usage;      /* for errors; a command's help page is commands/<canon>.txt */
};

static const char usage_end[] = "usage: backmux end [--print] <target>...";
static const char usage_attach[] = "usage: backmux attach <target> [--detach-keys KEYS]";

#define SPEC_PURGE_FLAGS (SPEC_ALL | SPEC_SCOPE | SPEC_FORCE | SPEC_STRICT)

static const struct hold_cli_command_spec command_specs[] = {
    /* cli/main.c parses launch commands and forwards arguments after PROGRAM.
     * These rows provide help and reserve command names from process naming. */
    {"call", NULL, 1, -1, 0, "usage: backmux call [--name <name>] [options] <program> [<args>...]"},
    {"server", NULL, 1, -1, 0, "usage: backmux server [--name <name>] [-w] [options] <program> [<args>...]"},
    {"detach", NULL, 1, -1, 0, "usage: backmux detach [--name <name>] [options] <program> [<args>...]"},
    {"list", NULL, 0, 2, SPEC_ALL | SPEC_LIVE | SPEC_SCOPE | SPEC_STRICT,
     "usage: backmux list [all] [-s|--system] [-u|--user] [-a|--all] [name]"},
    {"info", NULL, 1, 1, SPEC_STRICT, "usage: backmux info <target>"},
    {"recall", NULL, 1, 1, SPEC_STRICT, "usage: backmux recall <target>"},
    {"start", "recall", 1, 1, SPEC_STRICT, "usage: backmux start <target>   (alias for recall)"},
    {"delete", NULL, 1, 1, SPEC_STRICT, "usage: backmux delete <target>"},
    {"set", NULL, 2, -1, SPEC_OWN_HELP, "usage: backmux set <target> <setting>[=<value>]"},
    {"unset", NULL, 2, 2, SPEC_OWN_HELP, "usage: backmux unset <target> <setting>"},
    {"swap", NULL, 1, 2, SPEC_STRICT, "usage: backmux swap <target>,<target>"},
    {"layout", NULL, 1, 2, SPEC_STRICT, "usage: backmux layout list | save NAME | load NAME | rm NAME | default [NAME]"},
    {"manual", NULL, 0, 0, SPEC_STRICT, "usage: backmux manual"},
    {"ps", NULL, 0, 1, SPEC_ALL | SPEC_STRICT | SPEC_PS_DOCKER,
     "usage: backmux ps [-a|--all] [name]"},
    {"end", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT, usage_end},
    {"stop", "end", 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: backmux stop [--print] <target>..."},
    {"kill", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: backmux kill [--print] <target>..."},
    {"tail", NULL, 1, 1, SPEC_STRICT, "usage: backmux tail <target>"},
    {"logs", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: backmux logs <target> [--follow|-f] [--tail|-n N]"},
    {"inspect", NULL, 1, 1, SPEC_STRICT, "usage: backmux inspect <target>"},
    {"ports", NULL, 1, 1, SPEC_STRICT, "usage: backmux ports <target>"},
    {"stats", NULL, 1, 1, SPEC_NOSTREAM | SPEC_STRICT, "usage: backmux stats <target> [--no-stream]"},
    {"peek", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: backmux peek <target> [--json] [--ruler] [--last-alt]\n"
     "                 [--image FILE [--no-ruler] [--cursor-ring] [--scale N]\n"
     "                               [--no-footer] [--no-mouse-marker] [--text]]"},
    {"send", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: backmux send <target> [TEXT]... [--text TEXT] [-k KEY]...\n"
     "                 [--keys \"K1 K2\"] [--enter] [--paste TEXT|-] [--stdin] [--seq]\n"
     "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--wait MS [--timeout MS]]"},
    {"wait", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: backmux wait <target> [--idle [MS]] [--match TEXT | --regex RE]\n"
     "                 [--since SEQ] [--exit] [--timeout MS]"},
    {"msg", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: backmux msg <target> TEXT... [--stdin] [--no-enter] [-- TEXT...]\n"
     "                   [--wait-reply [--timeout MS]]\n"
     "       backmux msg [<target>] --received [ID]"},
    {"inbox", NULL, 0, -1, SPEC_OWN_HELP, "usage: backmux inbox [<name>] [--wait | --monitor] [--timeout <ms>]"},
    {"monitor", NULL, 0, -1, SPEC_OWN_HELP, "usage: backmux monitor [NAME] [--timeout MS]"},
    {"resize", NULL, 1, 3, SPEC_OWN_HELP, "usage: backmux resize <target> <cols> <rows>   (or <cols>x<rows>)"},
    {"config", NULL, 0, 4, SPEC_OWN_HELP,
     "usage: backmux config get <setting>\n"
     "       backmux config set <setting> <value>\n"
     "       backmux config unset <setting>\n"
     "settings: detach-size COLS ROWS, chat-name NAME"},
    /* cli/main.c parses shell launches; this row provides help. */
    {"shell", NULL, 0, -1, 0,
     "usage: backmux shell [--name <name>] [--size <cols> <rows>] [-e <key>=<value>]... [--rm]\n"
     "                     [<shell> [<args>...]]"},
    {"attach", NULL, 1, 1, SPEC_STRICT | SPEC_KEYS, usage_attach},
    {"console", "attach", 1, 1, SPEC_STRICT | SPEC_KEYS, "usage: backmux console <target> [--detach-keys KEYS]"},
    {"purge", NULL, 0, 1, SPEC_PURGE_FLAGS,
     "usage: backmux purge [<target>] [-a|--all] [--force]"},
    {"prune", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: backmux purge [<target>] [-a|--all] [--force]"},
    {"rm", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: backmux purge [<target>] [--force]"},
    {"drop", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: backmux purge [<target>] [-a|--all] [--force]"},
    {"save", NULL, 1, 1, SPEC_STRICT, "usage: backmux save <target>"},
    {"rename", NULL, 2, 2, SPEC_STRICT, "usage: backmux rename <target> <name>"},
    {"help", NULL, 0, 2, 0, "usage: backmux help [--full] [topic]"},
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

/* Shared option spellings. Each command's mask selects the applicable rows;
 * -a and --all are equivalent wherever SPEC_ALL is enabled. */
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
 * `--` passes the rest through untouched. SPEC_STRICT commands reject any
 * flag they did not consume rather than misreading it as a name filter.
 * Returns 0, or HOLD_EXIT_USAGE after printing the unknown-flag error and usage. */
int hold_cli_collect_owned_args(const char *command, int argc, char **argv,
                                char **cmd_argv, int *cmd_argc,
                                struct hold_cli_owned_opts *opts) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    unsigned allowed = spec ? spec->flags : 0;
    bool literal = false;
    /* send and msg need the delimiter to distinguish text from options. */
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
            /* --system selects the root-managed store except for ps, which
             * does not accept store-selection options. */
            if (!(allowed & SPEC_PS_DOCKER) && !strcmp(arg, "--system")) {
                opts->requested_system = true;
                continue;
            }
            if (!strcmp(arg, "--quiet")) {
                opts->quiet = true;
                continue;
            }
            /* Command-specific parsers decide whether -h is help or data. */
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
                fprintf(stderr, "backmux: error: unknown flag '%s'\n", arg);
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

/* Retained topic aliases resolve to the current canonical help page. */
static const char *const help_renamed[][2] = {
    {"users", "share"},
    {"comms", "messaging"},
    {"services", "service"},
    {"run", "launch"},
};

/* The complete reference reuses the same pages as per-command help. Only
 * the introduction is authored separately; command coverage and aliases
 * come from the parser's table rather than a second hand-maintained list.
 * It is the manual's, not help all's: help all is an index (below). */
static int show_reference(void) {
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
        if (command_specs[i].canon) printf("  %-12s %s\n", command_specs[i].name, command_specs[i].canon);
    puts("\nHELP TOPIC ALIASES");
    for (size_t i = 0; i < sizeof(help_renamed) / sizeof(help_renamed[0]); i++)
        printf("  %-12s %s\n", help_renamed[i][0], help_renamed[i][1]);
    return 0;
}

/* Help pages are compiled from src/cli/help. Command aliases share their
 * canonical command's page; guides, reference, extra topics and agent tasks
 * use names. Every page is for its reader (runtime/help_reader.c) unless
 * full: an agent gets its task index for help, help all and help agents,
 * and a task page only as far as it applies. Why: help all once printed
 * every page (43 KB), the first thing an agent runs; an agent needs its
 * tasks, small, each pointing to the next. */
int hold_show_help_for(const char *topic, bool full) {
    struct hold_help_view view;
    hold_help_reader_view(&view, full);
    if (!topic || !*topic) {
        if (!view.agent) {
            hold_usage();
            return 0;
        }
        topic = "agents";
    }
    for (size_t i = 0; i < sizeof(help_renamed) / sizeof(help_renamed[0]); i++)
        if (!strcmp(topic, help_renamed[i][0])) topic = help_renamed[i][1];
    if (!strcmp(topic, "all")) topic = view.agent ? "agents" : "index";
    if (!strcmp(topic, "agents")) topic = "tasks/index";
    if (!strcmp(topic, "index") || !strcmp(topic, "tasks/index"))
        return hold_help_print_view(topic, &view) == 0 ? 0 : HOLD_EXIT_USAGE;
    if (!strcmp(topic, "keys")) { /* the keys pages, one source with the Ctrl-Space ? panel */
        static const char *const keys[] = {"keys/about", "keys/prefix", "keys/modes", "keys/pane",
                                           "keys/window", "keys/resize", "keys/move", "keys/more"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && (i == 0 || view.full || !view.agent); i++)
            if (hold_help_print_view(keys[i], &view) != 0) return HOLD_EXIT_USAGE;
        return 0;
    }
    static const char *const sections[] = {"guides/", "reference/", "extra/", "tasks/"};
    char name[96];
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
        if (hold_checked_snprintf(name, sizeof(name), "%s%s", sections[i], topic) == 0 &&
            hold_help_print_view(name, &view) == 0)
            return 0;
    const struct hold_cli_command_spec *spec = find_public_command_spec(topic);
    if (spec && hold_checked_snprintf(name, sizeof(name), "commands/%s", spec->canon ? spec->canon : spec->name) == 0 &&
        hold_help_print_view(name, &view) == 0)
        return 0;
    fprintf(stderr, "backmux: unknown help topic '%s'; backmux help --full all lists them\n", topic);
    return HOLD_EXIT_USAGE;
}

int hold_show_help(const char *topic) {
    return hold_show_help_for(topic, false);
}

/* backmux help [--full] [topic]. --full: the whole page, not the reader's
 * part of it (for writing docs, for tests, for a reader in another
 * situation). -h and --help after help are help's overview, as before. */
int hold_help_command(int argc, char **argv) {
    bool full = false;
    const char *topic = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) full = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) continue;
        else if (!topic) topic = argv[i];
        else {
            fprintf(stderr, "usage: backmux help [--full] [topic]\n");
            return HOLD_EXIT_USAGE;
        }
    }
    return hold_show_help_for(topic, full);
}

/* manual: everything in one text - the complete reference and the worked
 * examples - for whoever wants to read it all. */
int hold_show_manual(void) {
    int rc = show_reference();
    if (rc != 0) return rc;
    putchar('\n');
    return hold_help_print("extra/examples") == 0 ? 0 : HOLD_EXIT_USAGE;
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
