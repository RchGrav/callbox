/* cli/specs.c — command_specs[] and the flag engine that runs off it.
 *
 * One row per spelled verb: aliases are rows whose canon names the dispatch
 * verb they fold onto. Arity, the per-command flag mask, the usage line, and
 * the help prose all live in the row, so parsing, arity guards, usage text,
 * and help are driven from this one table; no verb parses its own flags by
 * hand. */
#include "callbox/config.h"
#include <stddef.h>
#include "callbox/types.h"
#include "callbox/cli.h"
#include "callbox/runtime.h"

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
    const char *usage;
    const char *help_usage; /* help's usage line when it differs; NULL = usage */
    const char *help;       /* prose body; NULL = not a help topic */
};

/* Help prose, shared between a verb and its aliases. Prose is content, not
 * code: only its duplicated usage lines compress (help prints the row's
 * usage line above the body). */
static const char help_list[] =
    "list is callbox's scoped ledger, with a USER column (CALL ID, USER, COMMAND,\n"
    "CREATED, STATUS, PORTS, NAMES). By default you see your own calls, live and\n"
    "past. Scope flags:\n"
    "\n"
    "  -u/--user     your personal calls only (even under sudo: the invoking user's)\n"
    "  -s/--system   the global (root-managed) calls only\n"
    "  -a/--all      both scopes together\n"
    "  -l/--live     narrow any of the above to running calls (composes)\n"
    "\n"
    "Global calls a normal user is not entitled to read appear redacted: USER and\n"
    "COMMAND read 'hidden'. Run as root, list defaults to the global calls; -a also\n"
    "walks every user's store. An optional name narrows the listing. (For Docker's\n"
    "running view, see ps.)\n";
static const char help_ps[] =
    "ps is Docker's machine-wide view: running calls (add -a for ended too) across\n"
    "both your own calls and the global ones, as Docker's table without an IMAGE or\n"
    "USER column (CALL ID, COMMAND, CREATED, STATUS, PORTS, NAMES). Global calls you\n"
    "cannot read show a 'hidden' COMMAND. ps speaks only Docker: it takes no scope\n"
    "flags. For callbox's scoped, past-inclusive ledger with a USER column, use list.\n";
static const char help_end[] =
    "End calls politely: TERM, then KILL if they have not gone within 5 seconds.\n"
    "Several targets may be named. --print ends nothing: it prints the kill command\n"
    "that would, for a script to run or inspect. (stop is an alias.)\n";
static const char help_kill[] =
    "Force calls down with KILL, at once. Several targets may be named. --print\n"
    "prints the kill command instead of running it.\n";
static const char help_tail[] =
    "Watch a running call's output from now on: tail prints only what the call\n"
    "writes after you start, until it ends or you press Ctrl-C. (callbox logs\n"
    "<target> -f prints what is already captured first, then follows.) For a call\n"
    "that has ended, tail prints its whole log.\n";
static const char help_logs[] =
    "Print a call's captured output, byte for byte as the program wrote it: plain\n"
    "text for a plain call; for a TTY call, its escape sequences too (colors,\n"
    "cursor moves), as a terminal would receive them.\n"
    "\n"
    "  -f, --follow     keep printing as a running call writes, until it ends\n"
    "  -n N, --tail N   only the newest N lines\n"
    "\n"
    "The log is a file: log_path in callbox inspect <target> names it. For the live\n"
    "screen of a TTY call, use callbox peek.\n";
static const char help_peek[] =
    "Show a TTY call's screen as it is right now: the visible rows as plain text,\n"
    "one line per row. After a call ends, peek shows its last screen.\n"
    "\n"
    "  --ruler          add column and row numbers\n"
    "  --json           add the cursor, terminal modes, title, output counter and\n"
    "                   idle time\n"
    "  --image FILE     write a color PNG instead, with a coordinate ruler that marks\n"
    "                   the cursor's column and row in red\n"
    "    --no-ruler     ...without the ruler\n"
    "    --cursor-ring  ...with a ring around the cursor\n"
    "    --scale N      ...N times the size (1-4; default 2 up to 100 columns,\n"
    "                   else 1)\n"
    "    --no-footer    ...without the cursor/size line under the screen\n"
    "    --no-mouse-marker ...without the mark at the last mouse event\n"
    "    --text         ...and print the screen as text too\n"
    "  --last-alt       the last full-screen (alternate-screen) frame a program such\n"
    "                   as vim or less showed before it switched back\n"
    "\n"
    "Run callbox wait first (or callbox send --wait) so you do not read a half-drawn\n"
    "frame.\n";
static const char help_send[] =
    "Type into a TTY call without attaching. Arguments are applied in order.\n"
    "\n"
    "  TEXT             typed literally (--text TEXT for text that starts with -)\n"
    "  -k KEY           press a key: Enter, Tab, Escape, Backspace, Space, Up, Down,\n"
    "                   Left, Right, Home, End, PageUp, PageDown, Insert, Delete,\n"
    "                   F1-F24, with C- M- S- modifiers (C-c, M-x)\n"
    "  --keys \"K1 K2\"   press several keys\n"
    "  --enter          press Enter\n"
    "  --paste TEXT     paste it (bracketed when the program asked for that;\n"
    "                   - reads stdin)\n"
    "  --stdin          type what stdin holds\n"
    "  --mouse ACTION,COL,ROW[,BUTTON]\n"
    "                   a mouse event: click, press, release, move, drag, scroll-up,\n"
    "                   scroll-down (exit 9 if the program has mouse reporting off)\n"
    "  --wait MS        then wait until the program has answered and gone quiet for\n"
    "                   MS (--timeout MS bounds the wait; 10s if left out)\n"
    "  --seq            print the output counter, for callbox wait --since\n"
    "\n"
    "Keys are encoded for the program's current modes.\n";
static const char help_wait[] =
    "Block until a condition holds, so a script or agent never has to guess with\n"
    "sleep. Wait before you peek: right after a send, the screen may be only half\n"
    "drawn (callbox send --wait MS sends and waits in one step).\n"
    "\n"
    "  --idle [MS]      the program has drawn something and the screen then stays\n"
    "                   quiet for MS (500 if MS is left out), counted from when the\n"
    "                   wait began. Silence before the first output never counts:\n"
    "                   on a program that prints nothing (cat, a quiet server)\n"
    "                   --idle runs to the timeout, with or without --since 0. Use\n"
    "                   --match, --exit or peek there.\n"
    "  --match TEXT     the text is on the screen (already there counts)\n"
    "  --regex RE       ...as a regular expression\n"
    "  --since SEQ      only output past SEQ counts: --idle needs some, and --match\n"
    "                   and --regex wait for some before they look\n"
    "  --exit           the call ends. Also what a wait with no condition means.\n"
    "                   Prints its exit code: -1 for a plain -d call, which keeps\n"
    "                   no waiter to record one (launch it with -dw).\n"
    "  --timeout MS     a screen condition gives up after 30s unless this says\n"
    "                   otherwise (0 = never)\n"
    "\n"
    "On an --rm call, wait returns once the call is removed, so its name is free;\n"
    "for a minute after that, wait still reports how it exited.\n"
    "\n"
    "Exit status: 0 the condition held, 7 timed out, 8 the call ended first.\n";
static const char help_resize[] =
    "Resize a running TTY call's terminal to <cols> <rows> (or <cols>x<rows>), each\n"
    "1-1000. The program receives SIGWINCH, and the screen callbox peek shows reflows\n"
    "with it. To start a call at a size, place it with --size <cols> <rows>;\n"
    "callbox config set detach-size saves a default for detached TTY calls.\n";
static const char help_config[] =
    "Your saved defaults, kept in ~/.config/callbox/config (one \"name value\" line\n"
    "each).\n"
    "\n"
    "  detach-size COLS ROWS   the terminal size of a detached TTY call (callbox -dt,\n"
    "                          -dwt) placed without --size. Order: --size, then\n"
    "                          this, then the invoking terminal's size, then 80 24.\n"
    "                          Foreground launches never use it, and changing it\n"
    "                          never resizes a running call (callbox resize does).\n"
    "\n"
    "get prints the saved value (exit 1 when unset); set checks the value before\n"
    "saving, so a refused one leaves what was saved; unset clears it. Under sudo a\n"
    "launch reads your settings, but changing them is done as yourself, without\n"
    "sudo.\n";
static const char help_shell[] =
    "Open your shell (or the one you name) on a PTY, with a chat window to the\n"
    "agents you run under callbox. Callbox's flags come before the shell's name;\n"
    "every word after it is the shell's own (callbox shell zsh -l). With no shell\n"
    "named: $SHELL, else your login shell.\n"
    "\n"
    "  Ctrl-P           the lead key: it waits for the next key, and shows what it\n"
    "                   offers while it waits\n"
    "  Ctrl-P Ctrl-T    open chat, or move the keyboard between chat and the shell\n"
    "                   (--chat-key picks the second key, e.g. --chat-key ctrl-g)\n"
    "  Ctrl-P Ctrl-Q    detach (callbox attach <name> picks the shell back up, chat\n"
    "                   intact)\n"
    "  Ctrl-P Ctrl-P    send one Ctrl-P to the shell. Ctrl-P and any other key:\n"
    "                   both go through.\n"
    "  Esc              hide chat; the conversation stays, and so does the shell\n"
    "  Tab              choose who to talk to: a running TTY call, by name or id\n"
    "  Enter            send; Alt-Enter starts a new line\n"
    "  Ctrl-S           include a snapshot of the shell's screen in the next message\n"
    "  mouse            drag the title bar to move, the lower-right corner to\n"
    "                   resize, click the x to close; the wheel scrolls the\n"
    "                   conversation\n"
    "\n"
    "A message you send arrives in the agent's prompt from this shell's call, so\n"
    "the agent answers with an ordinary callbox msg <this shell> \"...\". Replies land\n"
    "in the chat window, never at your prompt; while chat is hidden, a corner badge\n"
    "counts what is unread. A message that could not be delivered says so. Chat is\n"
    "drawn over the shell, not into it: callbox peek, the log, and the shell itself\n"
    "never see it, and resizing or moving the window never resizes the shell.\n"
    "Messaging gives the agent no control of this shell.\n"
    "\n"
    "Flags: --name, --size, -e, --env-file, --rm, --detach-keys (two keys: the lead\n"
    "key, then the key that detaches) and --chat-key. See callbox help launch.\n";
static const char help_msg[] =
    "Message another call, typically an agent harness running under callbox. The text\n"
    "is typed into its prompt at once and submitted with Enter (an agent harness\n"
    "that is busy queues it), behind a header (all on one line) that says who\n"
    "sent it and how to answer:\n"
    "\n"
    "  [msg from <you>; reply: callbox msg <you> \"...\"] TEXT\n"
    "\n"
    "<you> is your own call, which callbox msg finds in $CALLBOX_ID (set in every\n"
    "call); the receiver shows that call's name as it is now. From outside any\n"
    "call the header is [msg from outside callbox].\n"
    "Multi-line text goes in as one bracketed paste when the program supports it.\n"
    "A callbox shell takes its messages in its chat window, never at its prompt.\n"
    "\n"

    "  --no-enter       type without submitting\n"
    "  --stdin          read the text from stdin\n"
    "  --               the rest is text, even words that start with a dash\n"
    "  --received [ID]  the call's message history: every message delivered to it,\n"
    "                   or with ID (printed by callbox msg when it sends) that\n"
    "                   message's sender and full text. Run it inside the receiving\n"
    "                   call, or name the call: callbox msg CALL --received ID.\n"
    "\n"
    "The header is addressing, not proof: whatever can reach a call's console runs\n"
    "as its user and could type the same text with callbox send. Control\n"
    "characters are removed from the text, and delivery is capped at 20 messages a\n"
    "minute per receiver.\n";
static const char help_attach[] =
    "Pick a running call back up in this terminal: a PTY call (callbox <cmd>, -it,\n"
    "-dt) or one placed with -i. Detach again with Ctrl-P Ctrl-Q. (console is an\n"
    "alias of attach.)\n"
    "\n"
    "Without a terminal (piped), attach sends its input and then keeps showing\n"
    "the call's output until the call ends; it does not end the call's input, so\n"
    "a -di call can be fed again by the next attach. To drive a call from a\n"
    "script, callbox send, peek, and wait are usually what you want.\n"
    "\n"
    "Ctrl-P is a lead key: it waits for the next key, with no time limit. Ctrl-Q\n"
    "after it detaches; Ctrl-P again sends one Ctrl-P to the program; any other key\n"
    "sends both through, so nothing typed is lost. --detach-keys KEYS changes the\n"
    "keys for this attach (ctrl-a,ctrl-d; one key, such as ctrl-], detaches at\n"
    "once).\n"
    "\n"
    "One terminal has a call at a time: a second attach is told who has it.\n"
    "Attached from inside another call (a callbox shell, say), the detach keys let go\n"
    "of the innermost attach first - the one on screen - and leave you in the call\n"
    "around it; press them again to detach that one. A console is free again as\n"
    "soon as its client is gone, however it went (a closed window, a killed\n"
    "process).\n";
static const char help_purge[] =
    "The one removal verb. With no target it sweeps ended calls; -a includes stale\n"
    "ones (a state widener, not a scope one). Scope: -u/--user sweeps your personal\n"
    "calls (the default); -s/--system sweeps the global store and, if you are not\n"
    "root, re-execs through sudo so sudo can prompt for the password. A target\n"
    "removes one call; --force removes it whatever its state (live or saved). rm,\n"
    "prune, and drop are accepted aliases.\n";
static const char help_inspect[] =
    "Print a call's record as JSON (an array, as docker inspect's is): its id and\n"
    "name, state, pids, start time, argv, mode, and log_path - the file its output\n"
    "is captured in. For a running call, Stdio shows where fds 0/1/2 point right\n"
    "now. For the log's text, use callbox logs <target>.\n";
static const char help_ports[] =
    "List the listening sockets in use by the call's process group, one per line\n"
    "(\"127.0.0.1:8080/tcp\"). Your own calls need no root.\n";
static const char help_stats[] =
    "Live resource usage for the call's process group: CPU %, resident memory, and\n"
    "process count, refreshed in place every second until Ctrl-C. --no-stream (or\n"
    "a stdout that is not a terminal) prints one frame.\n";
static const char help_save[] =
    "Protect a call from purge. There is no unsave; callbox purge <target> --force\n"
    "removes a saved call.\n";
static const char help_rename[] =
    "Rename a call, live or ended. Naming a call says you want to keep it, so\n"
    "rename also saves it (protects it from purge); callbox purge <name> --force\n"
    "still removes it.\n";

/* The purge family's help documents the scope flags its arity usage omits. */
static const char usage_purge_help[] =
    "usage: callbox purge [<target>] [-a|--all] [-s|--system] [-u|--user] [--force]";
static const char usage_end[] = "usage: callbox end [--print] <target>...";
static const char usage_attach[] = "usage: callbox attach <target> [--detach-keys KEYS]";

#define SPEC_PURGE_FLAGS (SPEC_ALL | SPEC_SCOPE | SPEC_FORCE | SPEC_STRICT)

static const struct hold_cli_command_spec command_specs[] = {
    {"list", NULL, 0, 1, SPEC_ALL | SPEC_LIVE | SPEC_SCOPE | SPEC_STRICT,
     "usage: callbox list [-a|--all] [-s|--system] [-u|--user] [-l|--live] [name]", NULL, help_list},
    {"ps", NULL, 0, 1, SPEC_ALL | SPEC_STRICT | SPEC_PS_DOCKER,
     "usage: callbox ps [-a|--all] [name]", NULL, help_ps},
    {"end", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT, usage_end, NULL, help_end},
    {"stop", "end", 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: callbox stop [--print] <target>...", usage_end, help_end},
    {"kill", NULL, 1, -1, SPEC_PRINT | SPEC_STRICT,
     "usage: callbox kill [--print] <target>...", NULL, help_kill},
    {"tail", NULL, 1, 1, SPEC_STRICT, "usage: callbox tail <target>", NULL, help_tail},
    {"logs", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox logs <target> [--follow|-f] [--tail|-n N]", NULL, help_logs},
    {"inspect", NULL, 1, 1, SPEC_STRICT, "usage: callbox inspect <target>", NULL, help_inspect},
    {"ports", NULL, 1, 1, SPEC_STRICT, "usage: callbox ports <target>", NULL, help_ports},
    {"stats", NULL, 1, 1, SPEC_NOSTREAM | SPEC_STRICT, "usage: callbox stats <target> [--no-stream]", NULL, help_stats},
    {"peek", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox peek <target> [--json] [--ruler] [--last-alt]\n"
     "                 [--image FILE [--no-ruler] [--cursor-ring] [--scale N]\n"
     "                               [--no-footer] [--no-mouse-marker] [--text]]", NULL, help_peek},
    {"send", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox send <target> [TEXT]... [--text TEXT] [-k KEY]...\n"
     "                 [--keys \"K1 K2\"] [--enter] [--paste TEXT|-] [--stdin] [--seq]\n"
     "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--wait MS [--timeout MS]]", NULL, help_send},
    {"wait", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox wait <target> [--idle [MS]] [--match TEXT | --regex RE]\n"
     "                 [--since SEQ] [--exit] [--timeout MS]", NULL, help_wait},
    {"msg", NULL, 1, -1, SPEC_OWN_HELP,
     "usage: callbox msg <target> TEXT... [--stdin] [--no-enter] [-- TEXT...]\n"
     "       callbox msg [<target>] --received [ID]", NULL, help_msg},
    {"resize", NULL, 1, 3, SPEC_OWN_HELP, "usage: callbox resize <target> <cols> <rows>   (or <cols>x<rows>)", NULL, help_resize},
    {"config", NULL, 0, 4, SPEC_OWN_HELP,
     "usage: callbox config get <setting>\n"
     "       callbox config set <setting> <value>\n"
     "       callbox config unset <setting>\n"
     "settings: detach-size COLS ROWS", NULL, help_config},
    /* shell is a launch, parsed in cli/main.c; this row is its help. */
    {"shell", NULL, 0, -1, 0,
     "usage: callbox shell [--name NAME] [--size COLS ROWS] [--chat-key KEY]\n"
     "                  [-e KEY=VALUE]... [--rm] [shell [args...]]",
     NULL, help_shell},
    {"attach", NULL, 1, 1, SPEC_STRICT | SPEC_KEYS, usage_attach, NULL, help_attach},
    {"console", "attach", 1, 1, SPEC_STRICT | SPEC_KEYS, "usage: callbox console <target> [--detach-keys KEYS]", usage_attach, help_attach},
    {"purge", NULL, 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"prune", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"rm", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [--force]", usage_purge_help, help_purge},
    {"drop", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: callbox purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"save", NULL, 1, 1, SPEC_STRICT, "usage: callbox save <target>", NULL, help_save},
    {"rename", NULL, 2, 2, SPEC_STRICT, "usage: callbox rename <target> <name>", NULL, help_rename},
    {"help", NULL, 0, 1, 0, "usage: callbox help [topic]", NULL, NULL},
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

static int help_targets(void) {
    printf("callbox help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = call id, leading id prefix, call name, or the start of a name\n\n"
           "A call id addresses one call directly, always. A call name resolves to the\n"
           "single call recorded under that name, narrowed by the verb: end/kill/logs\n"
           "look at running or logged calls, inspect looks at retained records, and purge\n"
           "looks at removable past call data. Failing a whole name or an id prefix, the\n"
           "start of a name works when one call the verb can act on has it (callbox logs\n"
           "hel for helper). When more than one call matches, callbox lists them and\n"
           "exits 6 - it never picks one.\n\n"
           "Verbs shorten the same way: callbox li is callbox list, when nothing else\n"
           "has that meaning. The top-level word is tried in this order: a verb; a call\n"
           "by whole name or id prefix (redial); a program on PATH; then the start of\n"
           "exactly one verb or (alone) one call's name. Several fits are listed, exit 6.\n");
    return 0;
}

static int help_system(void) {
    printf("callbox help system\n\n"
           "Root and sudo calls use the root-managed store:\n\n"
           "  Linux: /var/lib/callbox\n"
           "  macOS: /var/db/callbox\n\n"
           "Private root records and logs stay root-only. Normal users see only the\n"
           "redacted public index, where a call's USER and COMMAND read 'hidden'.\n"
           "Acting on a root-managed target requires root; user-local targets win\n"
           "over root-public collisions.\n\n"
           "  sudo callbox <cmd...>           start in root-managed state\n"
           "  sudo callbox list               list the global calls (authoritative)\n"
           "  sudo callbox list -a            global calls plus every user's calls\n"
           "  callbox list -s                 the redacted global view, as a normal user\n"
           "  callbox purge -s                sweep the global store (re-execs via sudo)\n");
    return 0;
}

static int help_launch(void) {
    fputs(
        "callbox help launch\n"
        "\n"
        "  callbox [flags] <command> [args...]      place a call\n"
        "  callbox [flags] -- <command> [args...]   ...when its name could be read as a\n"
        "                                        flag or as one of callbox's commands\n"
        "  callbox <id|name>                        redial: place a recorded call again\n"
        "\n"
        "Flags come before the command; every word after it is the command's own.\n"
        "\n"
        "HOW IT RUNS\n"
        "  (no flag)        attached here on a PTY, and callbox exits as it does. At a\n"
        "                   terminal, messages for the call (callbox msg) go to a chat\n"
        "                   window drawn over it, never into the program (callbox\n"
        "                   send still types into it); redial brings it back the same\n"
        "                   way. From a script (no terminal): its output streams (PTY:\n"
        "                   \\r\\n line ends, stderr merged), the end of your stdin\n"
        "                   reaches it as Ctrl-D, and messages are typed into it, as\n"
        "                   with -dt. To pipe data in, use -i.\n"
        "  -it              the same, without the chat window: messages are typed into\n"
        "                   the program, as with -dt (an agent run this way gets them).\n"
        "  -d, --detach     in the background: prints the call id and returns. No callbox\n"
        "                   process stays with it, so no exit code is recorded (-dt\n"
        "                   and -di record one: their broker stays).\n"
        "  -w, --wait       with -d: keep a waiter that records the exit code (-dw)\n"
        "  -t, --tty        on a PTY, with a screen for callbox peek, send and wait\n"
        "  -i, --interactive\n"
        "                   attached, with your input relayed on a pipe (no PTY): it\n"
        "                   can be piped into, and never holds your terminal; stderr is\n"
        "                   merged into its output. -di keeps the input open for\n"
        "                   callbox attach later.\n"
        "  --size COLS ROWS the PTY's size (with -t). Without it: your detach-size\n"
        "                   setting for a detached call, else this terminal's size,\n"
        "                   else 80 24.\n"
        "\n"
        "  The usual pairs:  -dt  a PTY call in the background, for scripts and agents\n"
        "                         (their messages are typed into the program)\n"
        "                    -dw  in the background, exit code recorded\n"
        "\n"
        "KEYS, WHEREVER YOU ARE ON A CALL\n"
        "  Ctrl-C           goes to the program (as in Docker)\n"
        "  Ctrl-P Ctrl-Q    detach: it keeps running; callbox attach picks it back up\n"
        "  Ctrl-D           with -i: end of input\n"
        "  Following a log (callbox tail, --tail, logs -f), Ctrl-C stops following.\n"
        "\n"
        "WHAT IT IS GIVEN\n"
        "  --name NAME      a name, good wherever a target is asked for\n"
        "  -e KEY=VALUE     an environment variable (--env; may be repeated)\n"
        "  --env-file FILE  ...a file of KEY=VALUE lines\n"
        "  --rm             remove the call's record and log when it ends\n"
        "  -f, --tail CALL  follow a call's log here instead (no PTY, no redial;\n"
        "                   Ctrl-C stops following, the call keeps running)\n"
        "  --restart POLICY start it again when it exits: no, always, unless-stopped,\n"
        "                   on-failure[:N] (not with -t: it runs without a PTY)\n"
        "  --restart-delay SECONDS\n"
        "                   wait exactly this long between restarts (0: none).\n"
        "                   Without it, callbox backs off: 1 s, doubling to 30 s,\n"
        "                   back to 1 s after a run of 10 s or more - so a program\n"
        "                   that fails at once does not restart in a tight loop\n"
        "  --detach-keys KEYS\n"
        "                   the keys that let go of an attached PTY call (the default\n"
        "                   is ctrl-p,ctrl-q)\n"
        "  --quiet          no status notes on stderr\n"
        "\n"
        "Value flags also take --flag=value. Run as root (or under sudo), callbox places\n"
        "the call in the root-managed store: see callbox help system. For your own shell\n"
        "with a chat window to your agents: callbox help shell.\n",
        stdout);
    return 0;
}

static int help_scripting(void) {
    printf("callbox help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(callbox -d <cmd...>)       capture the bare 64-hex call id\n"
           "  id=$(callbox -dw <cmd...>)      ...and record its exit code for callbox wait\n"
           "  callbox end --print <id>        print kill -TERM -- -<pgid>\n"
           "  callbox kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known name with nothing to do)\n"
           "  1  usage error (unknown option, bad value, wrong number of arguments), or a\n"
           "     failure with no code of its own\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  no such call: nothing matches the target, or it is not a valid id or name\n"
           "  6  more than one call (or verb) matches: they are listed; be more exact\n"
           "  7  callbox wait timed out (send --wait: the input was sent; the program\n"
           "     did not go quiet in time)\n"
           "  8  the call ended: before the wait condition held (send --wait: after the\n"
           "     input was sent), or (attach) before or while you were on it; attach\n"
           "     exits 0 when you detach, 3 if refused\n"
           "  9  send --mouse: the program has mouse reporting off (not delivered)\n"
           "  10 the call has no terminal screen or console (placed without -t / -i)\n"
           "  11 the call name asked for is taken\n"
           "  126 the command was found but cannot be run; 127 command not found\n"
           "\n"
           "With several targets (end a b c), each is acted on alone: the ones that\n"
           "resolve are ended, the rest reported, and the exit code is the worst.\n"
           "callbox msg prints the delivered message's id on stdout.\n");
    return 0;
}

/* The complete reference: every command and flag, in usage notation. */
static int help_page_all(void) {
    printf(
           "callbox help all\n"
           "\n"
           "Usage: callbox [<launch-flags>] [--] <program> [<args>...]\n"
           "       callbox <call>                      start it again, or join it if it runs\n"
           "       callbox <command> [<options>] [<call>...]\n"
           "\n"
           "A call is a program callbox keeps running, under a name. <call> is that\n"
           "name, the call's id, or the start of either. [x] is optional, a|b is one\n"
           "or the other, and ... may repeat.\n"
           "\n"
           "START A CALL\n"
           "  (no flag)                   attached on a PTY, with a chat window\n"
           "  -it                         attached on a PTY; messages are typed in\n"
           "  -i                          attached; your input on a pipe, no PTY\n"
           "  -d                          in the background; prints the call id\n"
           "  -dt                         ...on a headless terminal you can drive\n"
           "  -dw                         ...and records the exit code\n"
           "  --name <name>               its name (one is generated otherwise)\n"
           "  --size <cols> <rows>        its terminal size (with -t)\n"
           "  -e <key>=<value>            an environment variable; may repeat\n"
           "  --env-file <file>           environment variables from a file\n"
           "  --restart <policy>          always, on-failure[:<n>], or unless-stopped\n"
           "  --rm                        remove the call when it ends\n"
           "  --                          the next word is a program, never a command\n"
           "  shell [<shell> [<args>...]] your shell, with a chat window\n"
           "                              Every flag: callbox help launch\n"
           "\n"
           "SEE A CALL\n"
           "  ps                          running calls\n"
           "  list                        your calls, running and ended\n"
           "  logs <call> [-f] [-n <n>]   its output (-f follows, -n the last <n> lines)\n"
           "  tail <call>                 its output from now on\n"
           "  peek <call> [--json]        its screen as text\n"
           "  peek <call> --image <file>  its screen as a PNG\n"
           "  inspect <call>              everything callbox knows about it, as JSON\n"
           "  stats <call>                CPU, memory, and processes\n"
           "  ports <call>                the sockets it listens on\n"
           "\n"
           "DRIVE A CALL\n"
           "  attach <call>               join it; Ctrl-P Ctrl-Q leaves it running\n"
           "  send <call> [<text>] [-k <key>]... [--paste <text>] [--mouse <spec>]\n"
           "                              type and press keys in it\n"
           "  wait <call> [--match <text>|--regex <re>] [--idle <ms>] [--timeout <ms>]\n"
           "                              until its screen shows it, or it settles\n"
           "  wait <call>                 until it ends; prints its exit code\n"
           "  resize <call> <cols> <rows> change its terminal size\n"
           "  msg <call> <text>           message it (typed in, or to its chat window)\n"
           "  msg [<call>] --received [<id>]   messages it has been sent\n"
           "\n"
           "END AND CLEAN UP\n"
           "  end <call>...               TERM, then KILL if it has not ended in 5 s\n"
           "  kill <call>...              KILL now\n"
           "  rename <call> <name>        rename it (this also saves it)\n"
           "  save <call>                 keep it when ended calls are purged\n"
           "  purge [<call>] [-a] [--force]   remove ended calls you have not saved\n"
           "  config set detach-size <cols> <rows>   the size -dt calls start at\n"
           "\n"
           "KEYS, ON A CALL YOU ARE ATTACHED TO\n"
           "  Ctrl-C                      goes to the program\n"
           "  Ctrl-P Ctrl-Q               leave it running\n"
           "  Ctrl-P Ctrl-T               the chat window (switchless calls and shell)\n"
           "\n"
           "HELP BY TASK, WITH EXAMPLES\n"
           "  callbox help users          run agents and programs yourself\n"
           "  callbox help agents         orchestrate agents, drive terminal apps\n"
           "  callbox help messaging      how msg works, and where messages go\n"
           "  callbox help service        services and background processes\n"
           "  callbox help examples       workflows, start to finish\n"
           "  callbox help features       what callbox does under the hood\n"
           "  callbox help tui            troubleshoot a terminal UI\n"
           "  callbox help comms          agents in different harnesses, talking\n"
           "\n"
           "REFERENCE\n"
           "  callbox help launch         every flag for starting a call\n"
           "  callbox help scripting      exit codes, stdout and stderr, --quiet\n"
           "  callbox help system         root, sudo, and other users' calls\n"
           "  callbox <command> -h        one command in full\n"
           "  callbox --version\n"
           "\n"
           "Docs and issues: https://github.com/RchGrav/callbox\n");
    return 0;
}

/* For a person running agents and programs, with examples. */
static int help_page_users(void) {
    printf(
           "callbox help users\n"
           "\n"
           "Run agents and programs yourself.\n"
           "\n"
           "Start an agent the way you always would, with -it in front. It runs\n"
           "attached, just as it would on its own, but now it is a call: its name\n"
           "shows in the top-right corner for a few seconds, it keeps running if you\n"
           "step away, and other agents can message it.\n"
           "\n"
           "    callbox -it claude\n"
           "    callbox -it --name lead codex\n"
           "\n"
           "Start a full-screen program without a switch. It gets a chat window:\n"
           "Ctrl-P Ctrl-T opens it, messages for the program arrive there, and you can\n"
           "message your agents from it. callbox shell does the same for your shell.\n"
           "\n"
           "    callbox htop\n"
           "    callbox shell\n"
           "\n"
           "Step away and come back. Ctrl-P Ctrl-Q leaves the call running.\n"
           "\n"
           "    callbox ps                   what is running, by name\n"
           "    callbox attach lead          join it again\n"
           "\n"
           "When a call has ended, start it again by its name:\n"
           "\n"
           "    callbox lead\n"
           "\n"
           "If a program's name is also a callbox command, put -- before it:\n"
           "\n"
           "    callbox -- list\n");
    return 0;
}

/* For an agent orchestrating other agents and driving terminal apps. */
static int help_page_agents(void) {
    printf(
           "callbox help agents\n"
           "\n"
           "For an agent running inside callbox (started with callbox -it or -dt).\n"
           "\n"
           "Start a subagent on its own terminal and give it work:\n"
           "\n"
           "    callbox -dt --name reviewer claude\n"
           "    callbox msg reviewer \"review src/main.c and list the problems\"\n"
           "\n"
           "Its answer is typed into your prompt (see callbox help messaging). While it\n"
           "works you can read its screen, wait for it, or press keys in it:\n"
           "\n"
           "    callbox peek reviewer                its screen, as text\n"
           "    callbox wait reviewer --idle 5000    until it has been quiet for 5 s\n"
           "    callbox send reviewer -k Escape      interrupt it\n"
           "    callbox end reviewer                 stop it when it is done\n"
           "\n"
           "Drive any terminal app the same way, to test it or automate it:\n"
           "\n"
           "    callbox -dt --size 120 40 --name app ./my-tui\n"
           "    callbox wait app --match \"Main menu\"\n"
           "    callbox send app 2 -k Enter\n"
           "    callbox wait app --idle 500\n"
           "    callbox peek app --image screen.png  a picture of its screen\n"
           "    callbox send app --mouse click,10,5  click column 10, row 5\n"
           "    callbox end app\n"
           "\n"
           "A -d or -dt start prints the call's id on stdout and its name on stderr.\n"
           "Exit codes for scripts: callbox help scripting.\n");
    return 0;
}

/* How callbox msg works and where a message goes. */
static int help_page_messaging(void) {
    printf(
           "callbox help messaging\n"
           "\n"
           "callbox msg sends text to another call. Its broker types the text into the\n"
           "call's prompt at once, after a one-line header that says who sent it and\n"
           "how to answer:\n"
           "\n"
           "    callbox msg reviewer \"the build is green; please review src/main.c\"\n"
           "\n"
           "    [msg from lead; reply: callbox msg lead \"...\"] the build is green; ...\n"
           "\n"
           "The answer comes back the same way, typed into the sender's prompt. That is\n"
           "the whole protocol: agents talk to each other by running callbox msg.\n"
           "\n"
           "Where a message goes depends on how the call was started:\n"
           "\n"
           "    callbox -it, -dt          typed into the program\n"
           "    callbox <program>, shell  the chat window (a notice shows in the corner;\n"
           "                              Ctrl-P Ctrl-T opens it)\n"
           "\n"
           "Your own name is $CALLBOX_NAME. Messages are cleaned of control characters\n"
           "before they are typed, so they cannot press Escape or submit early. A call\n"
           "takes at most 20 messages a minute, which stops two agents from replying\n"
           "to each other forever.\n"
           "\n"
           "    callbox msg reviewer --received        every message reviewer was sent\n"
           "    callbox msg reviewer --received 3fa2c1 one of them, in full\n");
    return 0;
}

/* Services and background processes, kept without systemd. */
static int help_page_service(void) {
    printf(
           "callbox help service\n"
           "\n"
           "Background processes that last, with logs and names: more than nohup,\n"
           "less than systemd.\n"
           "\n"
           "Start a server, a VM, or a worker with -d. It keeps running after you log\n"
           "out, in its own session; callbox keeps its output in a log and gives it a\n"
           "name you manage it by. No unit file, no root, no daemon:\n"
           "\n"
           "    callbox -d --name api ./server --port 8080\n"
           "    callbox -d --name vm qemu-system-x86_64 -m 4G disk.qcow2\n"
           "\n"
           "Then manage it by name:\n"
           "\n"
           "    callbox ps                   is it running?\n"
           "    callbox logs api -f          follow its output\n"
           "    callbox stats api            CPU and memory\n"
           "    callbox end api              stop it: TERM, then KILL after 5 s\n"
           "    callbox api                  start it again, exactly as before\n"
           "\n"
           "That makes testing a service easy: start it, run your tests against it,\n"
           "read its log, end it, start it again. With --restart, callbox starts it\n"
           "again when it fails; with -w, callbox wait reports how it exited:\n"
           "\n"
           "    callbox -d --restart on-failure --name worker ./worker\n"
           "    callbox -dw --name migrate ./migrate.sh && callbox wait migrate\n"
           "\n"
           "In CI, start the service under test, wait until it is ready, run the\n"
           "tests, and keep its log if they fail. With -dt, wait can watch its output:\n"
           "\n"
           "    callbox -dt --name api ./server --port 8080\n"
           "    trap 'callbox end api; callbox purge api' EXIT\n"
           "    callbox wait api --match \"listening\" --timeout 30000\n"
           "    ./run-tests.sh || { callbox logs api -n 200; exit 1; }\n"
           "\n"
           "To have it running whenever you are, add one line to your ~/.bashrc (or\n"
           "~/.zshrc). Every new shell runs it: the first starts the service, later\n"
           "ones leave it running, and after a reboot the next shell starts it again.\n"
           "\n"
           "    callbox -d --name api ./server >/dev/null 2>&1 ||\n"
           "        callbox -d api >/dev/null 2>&1\n"
           "\n"
           "Then save it, so purging ended calls never removes it:\n"
           "\n"
           "    callbox save api\n"
           "\n"
           "That is the advantage over systemd: no unit file to write, nothing to\n"
           "install, no root, and nothing on your system to change. Set it up to test,\n"
           "and when you are done, end it and delete the line.\n");
    return 0;
}

/* Whole workflows, start to finish. */
static int help_page_examples(void) {
    printf(
           "callbox help examples\n"
           "\n"
           "A lead agent hands a review to a subagent:\n"
           "\n"
           "    callbox -it --name lead claude              you, in your terminal\n"
           "    callbox -dt --name reviewer codex           lead starts a subagent\n"
           "    callbox msg reviewer \"review src/main.c\"    lead gives it work\n"
           "    callbox wait reviewer --idle 5000           lead waits for it to finish\n"
           "    callbox peek reviewer                       ...or reads its screen\n"
           "    callbox end reviewer                        lead ends it when done\n"
           "\n"
           "Test a terminal app from a script:\n"
           "\n"
           "    callbox -dt --size 100 30 --name app ./my-tui\n"
           "    callbox wait app --match \"Main menu\" --timeout 5000\n"
           "    callbox send app 2 -k Enter\n"
           "    callbox wait app --match \"Saved\" --timeout 5000 ||\n"
           "        callbox peek app --image fail.png       a picture of what went wrong\n"
           "    callbox end app\n"
           "\n"
           "Run a development server while you work:\n"
           "\n"
           "    callbox -d --name api npm run dev\n"
           "    callbox logs api -f\n"
           "    callbox end api && callbox api               restart it\n"
           "\n"
           "Run a VM in the background and check on it:\n"
           "\n"
           "    callbox -d --name vm qemu-system-x86_64 -m 4G disk.qcow2\n"
           "    callbox stats vm\n"
           "    callbox logs vm -n 50\n"
           "    callbox end vm\n"
           "\n"
           "Run a long job and collect its exit code:\n"
           "\n"
           "    callbox -dw --name backup ./backup.sh\n"
           "    code=$(callbox wait backup); echo \"backup exited $code\"\n"
           "\n"
           "Test a network service in CI, and keep its log when the tests fail:\n"
           "\n"
           "    callbox -dt --name api ./server --port 8080\n"
           "    trap 'callbox end api; callbox purge api' EXIT\n"
           "    callbox wait api --match \"listening\" --timeout 30000\n"
           "    ./run-tests.sh || { callbox logs api -n 200; exit 1; }\n"
           "\n"
           "Bring up a mesh of QEMU nodes and drive each one's serial console:\n"
           "\n"
           "    for i in 1 2 3 4; do\n"
           "        callbox -dt --name node$i qemu-system-x86_64 -nographic -m 512M \\\n"
           "            -drive file=node$i.qcow2 -nic user,model=virtio\n"
           "    done\n"
           "    callbox wait node3 --match \"login:\" --timeout 120000\n"
           "    callbox send node3 root -k Enter\n"
           "    callbox send node3 \"ip -br addr\" -k Enter\n"
           "    callbox peek node3                          what it answered\n"
           "    callbox attach node3                        or take its console yourself\n");
    return 0;
}

/* What callbox does under the hood, so people can see what it is worth. */
static int help_page_features(void) {
    printf(
           "callbox help features\n"
           "\n"
           "What callbox does for you, and how.\n"
           "\n"
           "ONE BINARY, NO DAEMON\n"
           "  One binary: static on Linux (x86-64, arm64, armhf, riscv64, mipsel), and\n"
           "  for macOS on Intel and Apple silicon. Nothing runs between commands: each\n"
           "  call's state is a record on disk, written atomically (temp file, fsync,\n"
           "  rename), so a crash or a full disk never leaves half a record. A plain -d\n"
           "  call leaves nothing behind at all: no helper process, just the program\n"
           "  and its log.\n"
           "\n"
           "SIGNALS THAT CANNOT HIT THE WRONG PROCESS\n"
           "  Before end or kill sends a signal, callbox checks that the process is still\n"
           "  the one it started: the boot, the process start time, the executable, and\n"
           "  the process group and session. A pid that was reused by something else is\n"
           "  never signaled. end sends TERM to the whole process group, waits, then KILL.\n"
           "\n"
           "A REAL TERMINAL, WITH NOBODY WATCHING\n"
           "  A -t call runs on a PTY owned by a small broker, which feeds every byte into\n"
           "  a headless terminal (libvterm). It always knows the screen: every cell, its\n"
           "  colors and attributes, the cursor, and the program's input modes. That is\n"
           "  what peek, wait --match, and peek --image read, with no one attached.\n"
           "  It answers the program's terminal queries itself, so programs that ask\n"
           "  for the cursor position or the terminal type work under -dt.\n"
           "  Keys go in as exact xterm bytes, one write each; mouse events use the\n"
           "  encoding the program asked for; pastes are bracketed when it wants that.\n"
           "  When a full-screen program exits, its last frame is kept: peek --last-alt.\n"
           "  peek --image draws the screen to a PNG with real fonts (Unifont, emoji),\n"
           "  box drawing, the cursor, and a ruler, so an agent can look at it.\n"
           "\n"
           "CALLS THAT COME BACK\n"
           "  Each call keeps its recipe: the command line (paths made absolute, so it\n"
           "  works from anywhere), the -e variables you gave it, its terminal size, and\n"
           "  how it was started. callbox <name> starts it again from that. --restart\n"
           "  restarts a failed program with backoff (1 s, doubling to 30 s), so a\n"
           "  program that fails at once never spins. Attach and detach as often as you\n"
           "  like; a new terminal is repainted from the screen, not replayed.\n"
           "\n"
           "NAMES YOU CAN TYPE\n"
           "  Every call gets a name (adjective_noun, from its id) unless you give one.\n"
           "  Any unique start of a name or id works, and so does the start of a\n"
           "  command: callbox pe is peek.\n"
           "\n"
           "MESSAGES BETWEEN AGENTS\n"
           "  msg types text into another call's prompt with a header naming the sender\n"
           "  and how to reply. Control characters and escape sequences are removed\n"
           "  first, so a message can never press Escape, submit early, or drive the\n"
           "  terminal; a call takes at most 20 messages a minute.\n"
           "\n"
           "WORKS WHERE OTHERS BREAK\n"
           "  Where Unix sockets are blocked (some sandboxes), consoles fall back to\n"
           "  FIFOs and keep every feature, peer checks included. In another PID\n"
           "  namespace, callbox asks the call's supervisor rather than trusting pids.\n"
           "  Under sudo, a call started in your home lands in your store, and every\n"
           "  file stays yours. Other users see root's calls through a redacted list.\n");
    return 0;
}

/* Troubleshooting a terminal UI with callbox. */
static int help_page_tui(void) {
    printf(
           "callbox help tui\n"
           "\n"
           "Troubleshoot a terminal UI: reproduce what a user saw, read exactly what the\n"
           "program drew, and drive it with exact keys and mouse events.\n"
           "\n"
           "Run it on a headless terminal at a fixed size, so every run is the same:\n"
           "\n"
           "    callbox -dt --size 80 24 --name app ./my-tui\n"
           "\n"
           "Read what is on its screen, with coordinates, or as a picture:\n"
           "\n"
           "    callbox peek app --ruler            text, with column and row numbers\n"
           "    callbox peek app --image s.png      colors, attributes, and the cursor\n"
           "    callbox peek app --json             plus the cursor, the terminal modes\n"
           "                                        (mouse, bracketed paste, keypad),\n"
           "                                        the title, and the output counter\n"
           "\n"
           "Send exact keys, then wait only for output that comes after them, so you\n"
           "never read the screen from before the program answered:\n"
           "\n"
           "    seq=$(callbox send app -k Down -k Enter --seq)\n"
           "    callbox wait app --since \"$seq\" --idle 300\n"
           "    callbox peek app\n"
           "\n"
           "Try other sizes, and the mouse:\n"
           "\n"
           "    callbox resize app 40 12\n"
           "    callbox send app --mouse click,10,5   exits 9 if mouse reporting is off\n"
           "\n"
           "When it drew something wrong, read the bytes it actually wrote. The log\n"
           "keeps them, escape sequences included:\n"
           "\n"
           "    callbox logs app | cat -v | tail -40\n"
           "\n"
           "A full-screen program takes its screen with it when it exits; callbox keeps\n"
           "the last frame:\n"
           "\n"
           "    callbox peek app --last-alt\n"
           "\n"
           "Use it yourself while a script drives it:\n"
           "\n"
           "    callbox attach app                  Ctrl-P Ctrl-Q leaves it running\n"
           "\n"
           "A call started with no TERM gets TERM=xterm-256color. To test another\n"
           "terminal type, set it: callbox -dt -e TERM=vt100 --name app ./my-tui\n");
    return 0;
}

/* Agents in different harnesses messaging each other. */
static int help_page_comms(void) {
    printf(
           "callbox help comms\n"
           "\n"
           "Agents in different harnesses talk to each other through callbox. Claude\n"
           "Code, Codex, opencode, agy, pi: anything that can run a shell command can\n"
           "send a message, and anything started in callbox can receive one.\n"
           "\n"
           "Start each harness as a call, named for its role:\n"
           "\n"
           "    callbox -it --name lead claude          the one you work with\n"
           "    callbox -dt --name reviewer codex       a second harness, in the background\n"
           "    callbox -dt --name tester opencode      and a third\n"
           "\n"
           "Any of them messages any other by name. The message is typed into the\n"
           "receiver's prompt after a header that says who sent it and how to answer,\n"
           "so the receiver needs no setup:\n"
           "\n"
           "    callbox msg reviewer \"review the diff in src/net.c\"\n"
           "\n"
           "    [msg from lead; reply: callbox msg lead \"...\"] review the diff in ...\n"
           "\n"
           "Tell each harness about callbox once, in its instructions file (CLAUDE.md,\n"
           "AGENTS.md, ...). For example:\n"
           "\n"
           "    You run inside callbox as $CALLBOX_NAME. To ask another agent, run\n"
           "    callbox msg <name> \"<text>\"; its answer is typed into your prompt.\n"
           "    callbox ps lists the agents that are running.\n"
           "\n"
           "Watch the conversation, or step in:\n"
           "\n"
           "    callbox peek reviewer                   what it is doing now\n"
           "    callbox msg reviewer --received         what it has been told\n"
           "    callbox attach reviewer                 take its terminal yourself\n"
           "    callbox msg tester \"stop; lead takes over\"\n"
           "\n"
           "A harness started outside callbox can send messages, but has no call to\n"
           "receive the answer in. Start it with callbox -it, and it can.\n");
    return 0;
}

int hold_show_help(const char *topic) {
    if (!topic || !*topic) {
        hold_usage();
        return 0;
    }
    if (!strcmp(topic, "all")) return help_page_all();
    if (!strcmp(topic, "users")) return help_page_users();
    if (!strcmp(topic, "agents")) return help_page_agents();
    if (!strcmp(topic, "messaging")) return help_page_messaging();
    if (!strcmp(topic, "service")) return help_page_service();
    if (!strcmp(topic, "examples")) return help_page_examples();
    if (!strcmp(topic, "features")) return help_page_features();
    if (!strcmp(topic, "tui")) return help_page_tui();
    if (!strcmp(topic, "comms")) return help_page_comms();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "launch") || !strcmp(topic, "run")) return help_launch();
    const struct hold_cli_command_spec *spec = find_public_command_spec(topic);
    if (spec && spec->help) {
        printf("%s\n\n%s", spec->help_usage ? spec->help_usage : spec->usage, spec->help);
        return 0;
    }
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
