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
    "  --plain, --print, -p\n"
    "                   accepted for compatibility; output is unchanged, including\n"
    "                   any terminal escape sequences\n"
    "\n"
    "The log is a file: log_path in callbox inspect <target> names it. For the live\n"
    "screen of a TTY call, use callbox peek.\n";
static const char help_peek[] =
    "Show a TTY call's screen as it is right now: the visible rows as plain text,\n"
    "one line per row. After a call ends, peek shows its last screen.\n"
    "\n"
    "  --ruler          add zero-based column and row numbers\n"
    "  --json           add the cursor, terminal modes, title, output counter and\n"
    "                   idle time\n"
    "  --image FILE     write a color PNG instead, with a coordinate ruler that marks\n"
    "                   the cursor's column and row in red\n"
    "                   (-o FILE is equivalent; FILE - writes PNG to stdout)\n"
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
    "  TEXT             type literally; separate arguments add no spaces\n"
    "  --text TEXT      type text that starts with a dash\n"
    "  --               type all remaining arguments literally\n"
    "  -k, --key KEY    press a key: Enter, Tab, Escape, Backspace, Space, Up, Down,\n"
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
    "                   COL,ROW are zero-based; BUTTON is 1 left (default), 2 middle,\n"
    "                   or 3 right. Drag with press, then drag, then release.\n"
    "  --wait MS        then wait until the program has answered and gone quiet for\n"
    "                   MS (--timeout MS bounds the wait; 10s if left out)\n"
    "  --seq            print the output counter, for callbox wait --since\n"
    "\n"
    "Keys are encoded for the program's current modes. --wait needs output after\n"
    "the send; an action that draws nothing can time out even when delivered.\n"
    "Durations accept milliseconds, or suffixes ms, s, m (for example 1.5s).\n";
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
    "  --since SEQ      for a running call, require output past SEQ before\n"
    "                   testing --idle, --match, or --regex\n"
    "  --exit           the call ends. Also what a wait with no condition means.\n"
    "                   An exit-only wait prints its recorded exit code: -1 for\n"
    "                   plain -d, which keeps no waiter (use -dw to record it).\n"
    "  --timeout MS     bound the wait (0 = never). Default: 30s for screen\n"
    "                   conditions, no limit for an exit-only wait.\n"
    "\n"
    "If several conditions are given, any one can finish the wait. --idle means\n"
    "quiet output, not that an agent's task is complete. Durations accept\n"
    "milliseconds or suffixes ms, s, m. Screen waits require a console.\n"
    "A text match can also succeed on an ended call's saved final screen.\n"
    "\n"
    "To wait for a redraw after input:\n"
    "\n"
    "  seq=$(callbox send app -k Down --seq)\n"
    "  callbox wait app --since \"$seq\" --idle 300\n"
    "\n"
    "On an --rm call, wait returns once the call is removed, so its name is free;\n"
    "for a minute after that, wait still reports how it exited.\n"
    "\n"
    "Exit status: 0 the condition held, 7 timed out, 8 the call ended first.\n"
    "A successful exit wait returns 0 even if the program failed; its printed\n"
    "exit code is separate. To pass a job's result to CI, capture and use it:\n"
    "\n"
    "  code=$(callbox wait job)\n"
    "  exit \"$code\"\n";
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
    "named: an executable $SHELL, else your login shell, else /bin/sh.\n"
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
    "An agent can use peek and send to inspect and drive the shell while you\n"
    "stay attached; chat messages and terminal input remain separate.\n"
    "\n"
    "Flags: --name, --size, -e, --env-file, --rm, --detach-keys (two keys: the lead\n"
    "key, then the key that detaches) and --chat-key. See callbox help launch.\n";
static const char help_msg[] =
    "Message a call with a console, typically an agent started with -it or -dt.\n"
    "The text is typed into its prompt at once and submitted with Enter; the\n"
    "receiving program decides how to handle input while busy. A one-line header\n"
    "says who sent it and how to answer:\n"
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
    "  --received [ID]  list incoming message IDs and short text previews, or with\n"
    "                   ID (printed by callbox msg when it sends) show that\n"
    "                   message's sender and full text. Run it inside the receiving\n"
    "                   call, or name the call: callbox msg CALL --received ID.\n"
    "\n"
    "msg prints a message ID after delivery, not a completion report. Plain -d\n"
    "and -dw calls have no console to receive messages. From an outside harness,\n"
    "see callbox help comms for a reply destination you can inspect and wait on.\n"
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
           "Root and sudo calls normally use the root-managed store:\n\n"
           "  Linux: /var/lib/callbox\n"
           "  macOS: /var/db/callbox\n\n"
           "A launch whose executable or path argument resolves inside the invoking\n"
           "user's home uses that user's store instead, including under sudo. Root\n"
           "without a sudo user uses its own home for this check. This routing also\n"
           "applies when --system is supplied.\n\n"
           "Private root records and logs stay root-only. Normal users see only the\n"
           "redacted public index, where a call's USER and COMMAND read 'hidden'.\n"
           "Acting on a root-managed target requires root; user-local targets win\n"
           "over root-public collisions.\n\n"
           "  sudo callbox <cmd...>           start with root privileges\n"
           "  sudo callbox list               list the global calls (authoritative)\n"
           "  sudo callbox list -a            global calls plus every user's calls\n"
           "  callbox list -s                 the redacted global view, as a normal user\n"
           "  callbox purge -s                sweep the global store (re-execs via sudo)\n");
    return 0;
}

static int help_launch(void) {
    fputs(
        "callbox help launch - Start or reconnect to a call\n"
        "\n"
        "  callbox [options] [--] PROGRAM [ARGS...]\n"
        "  callbox [options] CALL\n"
        "\n"
        "Put options before PROGRAM; everything after it is passed to that program.\n"
        "-- forces a fresh program launch, even if its name is a Callbox command or\n"
        "an existing call. Shell expressions need an explicit shell: sh -c '...'.\n"
        "A lone CALL reconnects if running, or restarts its recorded command and mode\n"
        "if ended. A running call without a console is followed through its log.\n"
        "\n"
        "CHOOSE A MODE\n"
        "  No session flags: run attached on a terminal (PTY). On a real terminal,\n"
        "  messages go to a chat window; Ctrl-P Ctrl-T opens it. In a script, output\n"
        "  streams with terminal line endings, stderr merged, and messages typed in.\n"
        "  Use -i for pipe input. A PTY is a virtual terminal for the program.\n"
        "\n"
        "  -d, --detach     return immediately with the call ID on stdout\n"
        "  -w, --wait       keep an exit-code waiter; with -d, launch still returns now\n"
        "  -t, --tty        give the program a terminal screen\n"
        "  -i, --interactive\n"
        "                   keep input open; without -t, relay it through a pipe\n"
        "\n"
        "  -it              attached terminal; messages are typed into the program\n"
        "  -dt              detached terminal; inspect and control it with peek/send\n"
        "  -d               detached process, no terminal or recorded exit code\n"
        "  -dw              detached process with a recorded exit code\n"
        "  -di              detached pipe input; attach can feed it later\n"
        "  --console        earlier spelling for a detached terminal; prefer -dt\n"
        "\n"
        "  Terminal and pipe-input calls record exit codes. --rm and active restart\n"
        "  policies also retain a waiter. Plain -d alone leaves no Callbox helper.\n"
        "\n"
        "SET UP THE PROGRAM\n"
        "  --name NAME      assign a name (one is generated otherwise)\n"
        "  --size COLS ROWS set terminal dimensions, each 1-1000; COLSxROWS also works\n"
        "                   Default: --size, then saved detach-size for detached\n"
        "                   terminals, then invoking terminal size, then 80 24.\n"
        "  -e, --env KEY=VALUE\n"
        "                   set an environment variable; repeat for more variables\n"
        "  --env-file FILE  read KEY=VALUE lines from a file\n"
        "  --rm             remove the record, log, and screens when the call ends\n"
        "  --restart POLICY no (default), always, unless-stopped, on-failure[:N]\n"
        "                   Active policies run without a PTY; they cannot use -t.\n"
        "  --restart-delay SECONDS\n"
        "                   fixed delay between restarts, 0 for none; requires an\n"
        "                   active policy. Default: 1 s, doubling to 30 s, reset to\n"
        "                   1 s after a run of at least 10 s.\n"
        "\n"
        "CONTROL AN ATTACHED CALL\n"
        "  Ctrl-C           send the interrupt to the program\n"
        "  Ctrl-P Ctrl-Q    detach; the call keeps running\n"
        "  Ctrl-D           end pipe input with -i\n"
        "  --detach-keys KEYS\n"
        "                   change detach keys (default ctrl-p,ctrl-q); one key such\n"
        "                   as ctrl-] detaches immediately. Chat needs two\n"
        "                   keys, with its chat key different from both.\n"
        "  --chat-key KEY   for callbox shell only: choose the key after the lead key\n"
        "                   that opens chat (default ctrl-t)\n"
        "  -f, --tail CALL  follow a call's new log output without attaching or redial;\n"
        "                   Ctrl-C stops following, leaving the call running\n"
        "\n"
        "COMMON OPTIONS\n"
        "  --quiet          suppress status notes on stderr, keeping data on stdout\n"
        "  --system         select root-managed state; actions require root\n"
        "  -h, --help       show help (also callbox COMMAND --help)\n"
        "  -V, --version    print the version\n"
        "\n"
        "Value options accept --flag=value. PTY calls use TERM=xterm-256color unless\n"
        "overridden with -e TERM=... . The detach-size setting applies only to\n"
        "new detached terminal calls; see callbox config --help.\n"
        "\n"
        "More: callbox help targets, callbox help system, callbox help scripting\n",
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

/* Build the full reference from the same entries used by per-command help. */
static int help_page_all(void) {
    fputs("callbox help all - Complete command reference\n\n"
          "Read a command with callbox COMMAND --help, or a help topic listed below.\n"
          "CALL/target means a name, ID, or unique prefix. [x] is optional;\n"
          "x|y means either spelling; ... means repeatable arguments.\n\n"
          "Workflow guides: tui, agents, comms, users, service.\n"
          "Additional pages: examples, messaging, features.\n"
          "Reference pages: launch (also run), targets, system, scripting.\n\n",
          stdout);
    help_launch();
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        const struct hold_cli_command_spec *spec = &command_specs[i];
        if (spec->canon) continue; /* aliases share their canonical page */
        printf("\n%s\n\n%s", spec->help_usage ? spec->help_usage : spec->usage,
               spec->help ? spec->help : "Show the overview, or a named help page.\n");
        bool first = true;
        for (size_t j = 0; j < sizeof(command_specs) / sizeof(command_specs[0]); j++) {
            if (command_specs[j].canon && !strcmp(command_specs[j].canon, spec->name)) {
                printf("%s%s", first ? "\nAliases: " : ", ", command_specs[j].name);
                first = false;
            }
        }
        if (!first) putchar('\n');
    }
    putchar('\n');
    help_targets();
    putchar('\n');
    help_system();
    putchar('\n');
    return help_scripting();
}

/* For a person running agents and programs, with examples. */
static int help_page_users(void) {
    fputs(
        "callbox help users - Debug a TUI with your coding agent\n"
        "\n"
        "Use this when you want to reproduce an issue yourself, ask an agent to fix\n"
        "it, and try the result while keeping the application visible.\n"
        "\n"
        "START IN YOUR TERMINAL\n"
        "  From your project directory, start an installed, configured coding agent,\n"
        "  then your TUI (replace ./my-tui with your program):\n"
        "\n"
        "    callbox -dt --name coder -- codex\n"
        "    callbox peek coder\n"
        "    callbox --name app -- ./my-tui\n"
        "\n"
        "  Check that coder is ready for input. The last command keeps the TUI on\n"
        "  your terminal. With no session flags, it also has a chat window.\n"
        "\n"
        "ASK FOR HELP\n"
        "  Press Ctrl-P Ctrl-T to open chat, then Tab to select coder. For example:\n"
        "\n"
        "    The selection disappears when I press Down. Inspect call app,\n"
        "    reproduce it, fix the code, and tell me how to test the change.\n"
        "\n"
        "  Ctrl-S includes a clean screen snapshot; Enter sends the message.\n"
        "  Esc hides chat so you can keep using the application.\n"
        "\n"
        "LET THE AGENT INSPECT AND TEST\n"
        "  The agent runs these in its command tool, while you stay attached:\n"
        "\n"
        "    callbox peek app --image issue.png --cursor-ring\n"
        "    callbox peek app --json\n"
        "    callbox send app -k Down --wait 300\n"
        "    callbox peek app --image result.png --cursor-ring\n"
        "    callbox msg app \"Please try Down again and tell me what you see.\"\n"
        "\n"
        "  The Down key is this example's reproduction step; use the keys that\n"
        "  trigger your issue. peek shows the application's screen and cursor;\n"
        "  send changes that same live screen. Replies appear in your chat window.\n"
        "  The agent can inspect and send input without taking over your attach.\n"
        "\n"
        "TRY THE FIX\n"
        "  After the agent builds a change, quit the TUI normally and run:\n"
        "\n"
        "    callbox app\n"
        "\n"
        "  This restarts the recorded command. Repeat the failing interaction and\n"
        "  report the result in chat. The program's own build/reload process still\n"
        "  applies. When the coding agent is no longer needed, stop it:\n"
        "\n"
        "    callbox end coder\n"
        "\n"
        "  Ctrl-P Ctrl-Q detaches without stopping an app; callbox attach app\n"
        "  reconnects. callbox shell gives your shell the same chat interface.\n"
        "  A call started with -dt stays in that mode when attached: it has no chat.\n"
        "\n"
        "More: callbox help shell, callbox help attach, callbox help tui\n",
        stdout);
    return 0;
}

/* For an agent orchestrating other agents and driving terminal apps. */
static int help_page_agents(void) {
    fputs(
        "callbox help agents - Start another agent and give it work\n"
        "\n"
        "Use this to delegate work to an agent in another terminal or harness, then\n"
        "inspect its progress and receive a report. The agent CLIs below must\n"
        "already be installed and configured in your project.\n"
        "\n"
        "START THE LEAD AGENT\n"
        "  Run this in your terminal:\n"
        "\n"
        "    callbox -it --name lead -- claude\n"
        "\n"
        "  -it keeps the lead attached and delivers messages into its prompt.\n"
        "  A lead already running as a -it or -dt call can use the steps below.\n"
        "\n"
        "DELEGATE FROM THE LEAD\n"
        "  The lead runs these through its command tool:\n"
        "\n"
        "    callbox -dt --name reviewer -- codex\n"
        "    callbox peek reviewer\n"
        "\n"
        "  Check the screen for startup questions and a ready prompt before sending\n"
        "  work. Then ask for an explicit completion report:\n"
        "\n"
        "    callbox msg reviewer \\\n"
        "      \"Review src/main.c. Send your findings to lead with callbox msg.\"\n"
        "\n"
        "  -dt gives the reviewer its own detached terminal. msg delivers the task\n"
        "  immediately, with a sender and reply command. A successful send confirms\n"
        "  delivery; the receiving harness decides how to handle input while busy.\n"
        "\n"
        "CHECK PROGRESS AND RECEIVE THE REPORT\n"
        "    callbox peek reviewer --image reviewer.png\n"
        "    callbox msg lead --received\n"
        "\n"
        "  peek shows the current screen. --received lists incoming message\n"
        "  previews; use callbox msg lead --received ID for the full text of one.\n"
        "  The reviewer replies by running, for example:\n"
        "\n"
        "    callbox msg lead \"Review complete: the redraw path needs a test.\"\n"
        "\n"
        "  Use the report and the work itself to judge completion. wait --idle only\n"
        "  measures screen quietness; wait reviewer waits for the harness to exit,\n"
        "  which interactive agents normally do only when they are closed.\n"
        "\n"
        "STOP WHEN THE WORK IS FINISHED\n"
        "    callbox end reviewer\n"
        "    callbox logs reviewer -n 40\n"
        "    callbox purge reviewer\n"
        "\n"
        "  end stops the process; purge removes its retained record, log, and\n"
        "  screens. Keep the record if you want to inspect it or restart by name.\n"
        "\n"
        "Outside Callbox? See callbox help comms. Message options: callbox msg --help.\n",
        stdout);
    return 0;
}

/* How callbox msg works and where a message goes. */
static int help_page_messaging(void) {
    fputs(
        "callbox help messaging - Choose a messaging workflow\n"
        "\n"
        "Send work with callbox msg CALL \"text\". A call with a console can receive\n"
        "messages; an ordinary -d or -dw service cannot.\n"
        "\n"
        "  callbox help agents   Coordinate agents running inside Callbox\n"
        "  callbox help comms    Send and collect replies from an outside harness\n"
        "  callbox help users    Chat with an agent while using a terminal app\n"
        "  callbox msg --help    Read message syntax, delivery, and history options\n"
        "\n"
        "Messages to -it and -dt calls are typed into their programs immediately.\n"
        "Messages to callbox shell and attached user-mode calls go to their chat\n"
        "window. The sender's call identity supplies the reply address; a sender\n"
        "outside Callbox must provide a destination explicitly.\n"
        "\n"
        "Delivery is not a completion report. Ask the receiving agent to send its\n"
        "findings back, and inspect its screen or work when you need progress.\n",
        stdout);
    return 0;
}

/* Services and background processes, kept without systemd. */
static int help_page_service(void) {
    fputs(
        "callbox help service - Run background jobs and services in CI\n"
        "\n"
        "Use this to run a build, test job, or service with a name and captured logs.\n"
        "-d runs without a terminal screen; -dw also records the eventual exit code.\n"
        "Both return immediately. Use -dt when you need to drive a terminal UI.\n"
        "\n"
        "RUN A JOB AND RETURN ITS RESULT\n"
        "  From a project with a Makefile test target, run this as a CI shell step:\n"
        "\n"
        "    set -e\n"
        "    callbox -dw --name checks -- make test\n"
        "    callbox list checks\n"
        "    code=$(callbox wait checks)\n"
        "    callbox logs checks -n 100\n"
        "    callbox purge checks\n"
        "    exit \"$code\"\n"
        "\n"
        "  list shows status. wait blocks until the job ends and prints its exit\n"
        "  code; its own shell status is 0 when that wait succeeds. exit \"$code\"\n"
        "  passes the job's result to CI. A plain -d call has no recorded exit code\n"
        "  (wait prints -1), so use -dw here. Read or export logs before purging.\n"
        "\n"
        "KEEP A SERVICE RUNNING DURING TESTS\n"
        "  This separate shell example serves the current directory using Python 3.\n"
        "  It requires curl and an available local port 8080.\n"
        "\n"
        "    set -e\n"
        "    callbox -d --name web -- \\\n"
        "      python3 -u -m http.server 8080 --bind 127.0.0.1\n"
        "    trap 'callbox end web; callbox purge web' EXIT\n"
        "    callbox list web\n"
        "    curl --retry 10 --retry-connrefused --retry-delay 1 \\\n"
        "      --fail --silent --show-error http://127.0.0.1:8080/ >/dev/null\n"
        "    callbox ports web\n"
        "    callbox logs web -n 20\n"
        "\n"
        "  The HTTP request is this example's readiness check and smoke test; follow\n"
        "  it with your project's tests. The EXIT trap stops and removes the service\n"
        "  when the shell step ends. Use logs -f to follow output interactively;\n"
        "  Ctrl-C stops following and leaves the service running.\n"
        "\n"
        "  A -d service has no screen for wait --match or peek. Use its own readiness\n"
        "  check. A service launched with -dt has a terminal, so wait --match can\n"
        "  watch a startup message, but that also changes its terminal behavior.\n"
        "\n"
        "RESTART OR RETAIN A SERVICE\n"
        "    callbox -d --restart on-failure --name worker -- ./worker\n"
        "    callbox end worker\n"
        "    callbox worker\n"
        "\n"
        "  Replace ./worker with your program. --restart retries failures; end stops\n"
        "  the call. Its name starts the recorded command again. --rm removes logs\n"
        "  as soon as a call ends, so leave it off when you need post-exit diagnosis.\n"
        "  save protects a retained call from purge; rename also saves it.\n"
        "\n"
        "More: callbox help launch, callbox help scripting, callbox logs --help\n",
        stdout);
    return 0;
}

/* Whole workflows, start to finish. */
static int help_page_examples(void) {
    fputs(
        "callbox help examples - Find a workflow\n"
        "\n"
        "Start with the guide that matches your task:\n"
        "\n"
        "  callbox help tui       Reproduce a terminal UI issue and verify the result\n"
        "  callbox help agents    Delegate work and inspect another agent's progress\n"
        "  callbox help comms     Exchange messages from outside Callbox\n"
        "  callbox help users     Keep a TUI visible while an agent helps debug it\n"
        "  callbox help service   Run a CI job or service, read logs, and clean up\n"
        "\n"
        "These additional recipes use programs and files you supply:\n"
        "\n"
        "COLLECT A LONG JOB'S EXIT CODE\n"
        "    callbox -dw --name backup -- ./backup.sh\n"
        "    code=$(callbox wait backup)\n"
        "    callbox logs backup -n 50\n"
        "    printf 'backup exited %s\\n' \"$code\"\n"
        "    callbox purge backup\n"
        "\n"
        "CHECK A BACKGROUND VM\n"
        "    callbox -d --name vm -- qemu-system-x86_64 -m 4G disk.qcow2\n"
        "    callbox stats vm --no-stream\n"
        "    callbox logs vm -n 50\n"
        "    callbox end vm\n"
        "    callbox purge vm\n"
        "\n"
        "START A SERVICE WITH YOUR SHELL\n"
        "  To start a service on opening a shell, add this to ~/.bashrc or\n"
        "  ~/.zshrc, replacing /absolute/path/to/server with your program:\n"
        "\n"
        "    callbox -d --name api -- /absolute/path/to/server >/dev/null 2>&1 ||\n"
        "      callbox -d api >/dev/null 2>&1\n"
        "\n"
        "  The first shell creates the call; later shells leave it running or\n"
        "  restart its recorded command if it ended. callbox save api protects\n"
        "  its record from ordinary purge. Remove the startup line and run\n"
        "  callbox end api when you no longer need it.\n"
        "\n"
        "DRIVE SEVERAL VM SERIAL CONSOLES\n"
        "  Supply a bootable node1.qcow2 through node4.qcow2. Guest serial consoles\n"
        "  must be configured; login and network setup depend on the guest images.\n"
        "\n"
        "    for i in 1 2 3 4; do\n"
        "      callbox -dt --name node$i -- qemu-system-x86_64 -nographic -m 512M \\\n"
        "        -drive file=node$i.qcow2 -nic user,model=virtio\n"
        "    done\n"
        "    callbox wait node3 --match 'login:' --timeout 120000\n"
        "    callbox peek node3\n"
        "    callbox attach node3\n"
        "\n"
        "  Log in yourself, or use send for the guest's login and command sequence.\n"
        "  Ctrl-P Ctrl-Q detaches. To finish the example:\n"
        "\n"
        "    callbox end node1 node2 node3 node4\n"
        "    for i in 1 2 3 4; do callbox purge node$i; done\n"
        "\n"
        "Command options: callbox help all, or callbox COMMAND --help.\n",
        stdout);
    return 0;
}

/* What callbox does under the hood, so people can see what it is worth. */
static int help_page_features(void) {
    fputs(
        "callbox help features - Understand what a call provides\n"
        "\n"
        "A call records a command, its identity, and captured output. A terminal\n"
        "call also maintains a screen, input modes, and cursor state. These are the\n"
        "same capabilities used in the workflow guides; callbox help all lists\n"
        "every command and flag.\n"
        "\n"
        "RUN AND RECONNECT\n"
        "  Callbox runs ordinary host programs without a central daemon. Detached\n"
        "  processes keep running after the launching command returns. A plain -d\n"
        "  call leaves no Callbox helper; a terminal call keeps a broker, and -dw,\n"
        "  --rm, or restart policies keep the helpers needed for those features.\n"
        "  attach reconnects to one existing console; Ctrl-P Ctrl-Q detaches.\n"
        "\n"
        "INSPECT AND CONTROL A TERMINAL\n"
        "  peek reads the current screen as text, JSON, or a color PNG. It includes\n"
        "  cursor and coordinate information. send encodes keys, pastes, and mouse\n"
        "  input for the program's terminal modes. wait observes output or exit;\n"
        "  resize changes the terminal dimensions. Full-screen apps can leave a\n"
        "  final alternate-screen snapshot for peek --last-alt.\n"
        "\n"
        "COORDINATE AGENTS\n"
        "  msg sends text with a sender and reply address to a call with a console.\n"
        "  Agents launched with -it or -dt receive it as input; user-mode calls\n"
        "  receive it in chat. An outside harness can use a separate reply call:\n"
        "  see callbox help comms. A message ID confirms delivery, not completion.\n"
        "\n"
        "KEEP OR REMOVE A CALL\n"
        "  The record retains the launch recipe so its name can start it again.\n"
        "  Explicit environment assignments and launch mode are retained. save and\n"
        "  rename protect retained calls from ordinary purge; --rm removes them\n"
        "  automatically when they end. --restart can retry a failed program.\n"
        "\n"
        "IDENTIFY AND SCOPE CALLS\n"
        "  Names and unique prefixes make calls easier to address. Before signaling,\n"
        "  Callbox checks process identity to avoid acting on a reused PID. Calls\n"
        "  normally belong to your user; root-managed records have a redacted public\n"
        "  view. See callbox help targets and callbox help system for lookup and sudo.\n"
        "\n"
        "USE DIFFERENT ENVIRONMENTS\n"
        "  Linux builds can be static; macOS builds are also available. Where Unix\n"
        "  sockets are denied, the console can use FIFO transport. Callbox uses a\n"
        "  supervisor to verify a process viewed from another PID namespace.\n"
        "  It does not provide containers, port forwarding, or filesystem isolation.\n"
        "\n"
        "Implementation details: https://github.com/RchGrav/callbox/tree/main/docs\n",
        stdout);
    return 0;
}

/* Troubleshooting a terminal UI with callbox. */
static int help_page_tui(void) {
    fputs(
        "callbox help tui - Inspect, drive, and test a terminal UI\n"
        "\n"
        "Use this when an agent or script needs to reproduce a navigation, cursor,\n"
        "color, or layout problem in a terminal UI (TUI). A detached terminal keeps\n"
        "its screen available even when nobody is attached.\n"
        "\n"
        "LAUNCH AND INSPECT\n"
        "  This example uses Vim; install it first. -u NONE and -i NONE keep the\n"
        "  example independent of your Vim settings and history.\n"
        "\n"
        "    callbox -dt --name editor --size 80 24 -- \\\n"
        "      vim -u NONE -N -i NONE -c 'set mouse=a ttymouse=sgr'\n"
        "    callbox wait editor --idle 300\n"
        "    callbox peek editor --image before.png --cursor-ring\n"
        "    callbox peek editor --json\n"
        "\n"
        "  The PNG shows colors, a coordinate ruler, and the cursor. JSON includes\n"
        "  text, cursor position, dimensions, input modes, and an output counter.\n"
        "  For an existing Callbox terminal, use its name instead of launching again.\n"
        "\n"
        "INTERACT AND VERIFY\n"
        "    callbox send editor i 'Callbox test' -k Escape --wait 300\n"
        "    callbox wait editor --match 'Callbox test'\n"
        "    callbox send editor --mouse click,4,0\n"
        "    callbox resize editor 100 30\n"
        "    callbox wait editor --idle 300\n"
        "    callbox peek editor --json\n"
        "    callbox peek editor --image after.png --cursor-ring\n"
        "\n"
        "  send applies text and keys in order. --wait waits for new output, then\n"
        "  300 ms of quiet. Check the resulting text, cursor, and 100-by-30 layout.\n"
        "  A quiet screen is ready to inspect; it is not proof the result is correct.\n"
        "\n"
        "  Mouse coordinates are zero-based column,row. This example enables mouse\n"
        "  reporting in Vim; other applications must support and enable it too.\n"
        "  send --mouse returns 9 without sending if reporting is off.\n"
        "\n"
        "FINISH OR LET A PERSON TRY IT\n"
        "  A person can run callbox attach editor in another terminal while the\n"
        "  agent uses peek and send. Ctrl-P Ctrl-Q detaches without stopping Vim.\n"
        "  attach reconnects to a Callbox call; it cannot adopt an arbitrary process.\n"
        "\n"
        "    callbox send editor ':q!' --enter\n"
        "    callbox wait editor\n"
        "    callbox peek editor --last-alt --image final.png\n"
        "    callbox purge editor\n"
        "\n"
        "  :q! discards this example's scratch buffer. wait prints Vim's exit code;\n"
        "  --last-alt captures its last full-screen frame before cleanup.\n"
        "\n"
        "For your own app, replace Vim and its keys with the steps that reproduce\n"
        "its issue. After an action, --seq and wait --since can require fresh output;\n"
        "see callbox send --help and callbox wait --help. Raw output is available\n"
        "with callbox logs editor while the record is retained.\n",
        stdout);
    return 0;
}

/* Agents in different harnesses messaging each other. */
static int help_page_comms(void) {
    fputs(
        "callbox help comms - Exchange messages from outside Callbox\n"
        "\n"
        "Use this when your current harness can run shell commands but was not\n"
        "started inside Callbox. It can send to a Callbox agent, but has no call of\n"
        "its own for replies. A temporary terminal call can receive them instead.\n"
        "The reviewer CLI below must already be installed and configured.\n"
        "\n"
        "CREATE A REPLY DESTINATION AND A WORKER\n"
        "    callbox -dt --name replies -- cat\n"
        "    callbox -dt --name reviewer -- codex\n"
        "    callbox peek reviewer\n"
        "\n"
        "  Check that reviewer is ready for input. replies runs cat: it displays\n"
        "  incoming text without executing it. Callbox also records its messages.\n"
        "\n"
        "GIVE WORK AND WAIT FOR A REPLY\n"
        "    callbox msg reviewer \\\n"
        "      \"Review src/main.c. Reply with your findings using\" \\\n"
        "      \"callbox msg replies 'your findings here; REVIEW_DONE'.\"\n"
        "    callbox wait replies --match \"REVIEW_DONE\" --timeout 120000\n"
        "    callbox msg replies --received\n"
        "\n"
        "  The request is marked as coming from outside Callbox, so it names the\n"
        "  reply destination explicitly. Ask the reviewer to replace the example\n"
        "  findings with its actual report, keeping the marker at the end so it\n"
        "  stays visible. Only replies is watched, so the original request in\n"
        "  reviewer's prompt cannot satisfy this wait.\n"
        "\n"
        "  --received lists message IDs and short previews. To read the full report,\n"
        "  replace ID with the ID shown in that list:\n"
        "\n"
        "    callbox msg replies --received ID\n"
        "\n"
        "  Use a fresh reply call or a new marker for each task; --match also accepts\n"
        "  text already on the screen. A timeout (exit 7) means the marker did not\n"
        "  appear in time. Inspect the worker and continue waiting if appropriate:\n"
        "\n"
        "    callbox peek reviewer --image progress.png\n"
        "\n"
        "CLEAN UP WHEN FINISHED\n"
        "    callbox end reviewer replies\n"
        "    callbox purge reviewer\n"
        "    callbox purge replies\n"
        "\n"
        "There is no monitor command in this version. peek reads a screen, logs -f\n"
        "follows output, and wait waits for a screen condition or process exit;\n"
        "none registers an outside harness or pushes replies into its prompt.\n"
        "For direct replies to your agent's prompt, start it with callbox -it.\n"
        "\n"
        "More: callbox help agents, callbox msg --help, callbox wait --help\n",
        stdout);
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
