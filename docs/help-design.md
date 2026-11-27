# Help rewrite: design for review

Status, 2026-11-27. Built and passing CI, but not committed:
- **Help for its reader (section 6).** An agent's own front page has a
  header naming where it runs, `Your Agent ID: Sleek Carbon
  (sleek_carbon)`, the Started by and Last message lines, the pitch, and a
  task table with each task's commands. There are 11 task pages. Every
  guide and command page points an agent to its tasks. `help all` is an
  index, `--full` prints a page whole, and the full text is in `manual`.
- **The person's side (sections 3 to 5).**
  - The glossary has the settled terms.
  - The front page is restyled, with an AUTOMATE AND MESSAGE group and
    `<target>`.
  - `help keys` and the Ctrl-Space ? panel share one source,
    `src/cli/help/keys/`, and the panel fits 80x24.
  - Every command page has a purpose line that matches `help all`.
  - Usage lines use the notation.
  - `help panes` is the model, with the keys in `help keys`.
  - `share` is rewritten around Assist.
- **Friendly names, `BACKMUX_PARENT` holding the ID, service readiness,
  and the `--wait-reply` wording.**

`help commands` and `help topics` became part of `help all`, the index.
Tested by four subagent rounds, each driving real Claude agents inside
backmux.

Sources: your front page (the plain one in a716055, which 45c0b96 turned
into headed sections), your help-surface handoff (2026-11-26), your
critique of the Ctrl-Space ? panel (2026-11-26), every page under
`src/cli/help/`, and the key handling in `src/console/mux_keys.c` and
`mux_help.c`.

## 1. What is wrong now

Measured on 1.0.0-6ad5ac2.

- **No index of commands.** The front page names 18 of the 34 commands.
  `peek`, `send`, `wait` and `msg` are missing. Those four are the headless
  terminal and the messaging, which is half of what backmux is.
- **`help all` prints 43 KB**, every page joined end to end. It was never
  meant to; it is the first thing an agent runs, so it floods the agent's
  session. `manual` is the same 43 KB plus examples (45 KB).
- **No page covers the most common agent case.** Chat's Assist message
  tells the agent "run `backmux help tui`", and `help tui` starts by
  launching vi from scratch. An agent joining a live app needs the message
  header, how to reply, `peek` and `send`, and that is split across `tui`,
  `share`, `messaging` and `agents`. This is the example in your handoff.
- **One thing, several names.**
  - Modes. The bar says PANE, WINDOW, RESIZE, MOVE. The keys panel says
    Pane mode, Window mode, Resize mode, Move mode. `help panes` says Pane
    Navigation, Window Navigation, Pane Resizing, Pane Placement.
  - Zoom. The bar and the panel say Zoom; `help panes` says "full-screen
    view".
  - "Console". It names a process's input/output connection (`attach`,
    error messages), and also a launcher list (processes with no terminal).
- **Argument notation differs on every surface.**
  - The front page writes PROGRAM and TARGET.
  - Usage lines write `<program>`, `<name|id>` and `<target>`.
  - `inbox` writes `[<name>]` in its usage line and `[NAME]` on its page.
  - `<name|id>` is out of date: since 6ad5ac2 a place (2C) works as a
    target for every command.
- **`help panes` does three jobs in 152 lines:** the model (places,
  shapes, views), the full key reference, and a tour of every bar item.
  A person looking up one key has to read all of it.
- **The keys panel is better than when you critiqued it, but not fixed.**
  It now has sections, two columns and a frame. But:
  - Its labels are still fragments ("Zoom the pane, or back", "These keys",
    "Next pane; past the edge, slide").
  - Its groups are mixed: Lock is under Display; Rename and Save are under
    Chat.
  - Nothing says what a mode is or how to leave one.

## 2. Readers

| Reader | Needs | Finds it in |
| --- | --- | --- |
| New to backmux | What it is; the dozen commands to start with | front page |
| Returning | The command or key they half remember | `help commands`, `help keys`, Ctrl-Space ? |
| An agent sent to do one thing | One small page with everything for that activity | an activity topic (`tui`, `agents`, ...) |
| Writing a script | Exact syntax, options, exit codes | `help <command>`, `help all`, `help scripting` |

Why this split: the handoff asks for two things at once, a cheap way in
and a complete reference. git has the same problem and solves it in
three layers:
- `git help`: the common commands, grouped by activity, each group
  pointing to a guide;
- `git help -a`: every command, one line each;
- `git help -g`: the guides.

Your front page already reads like git's front page, so git's layering is
the existing convention, not a new invention.

## 3. Terms to settle first

Each term is set to match what the screen shows, or what you already
decided.

| Term | Meaning | Change | Why |
| --- | --- | --- | --- |
| process | A program backmux started, running or ended, with a name and an ID | None. `call` is only a command name, never a noun. | Your decision, 11-26 |
| record | What backmux keeps on disk about a process; it outlives the process | None | |
| profile | A saved record; purge leaves it alone | None | Your front page |
| background process | A process with no terminal attached to it | `server` stops being described as "a background process with a log"; it becomes "a program with no terminal" | Detach is the act and background is what the process becomes (your decision). What makes a server different is that it never has a terminal, so that is what its description should say. |
| pane, window, place, base, view, layout, grid | As built on 11-27 | None | Your decisions, already built |
| modes | Pane mode, Window mode, Resize mode, Move mode | The four long names in `help panes` go | The bar shows PANE, WINDOW, RESIZE, MOVE. Help must use the names on screen. |
| zoom | One pane fills the screen | "full-screen view" goes | The bar button says Zoom, and zoom is tmux's word |
| lock | Alt-arrows go to the program instead of moving between panes | Labels state that effect | Your critique asked "lock what?" |
| console | The launcher's list of processes with no terminal (servers, pipe input) | Everywhere else, user-facing text says "input" instead ("has no input") | One word, one meaning. The launcher list is the only place a person sees the word. |
| `<target>` | A process name, an ID or ID prefix, or a place (2C) | Replaces `<name\|id>` and TARGET | Places are targets for every command since 6ad5ac2 |

## 4. Structure

### Layers

```
backmux, backmux help   front page: common commands by activity, three keys,
                        where to go next
  help commands         every command, one line each, grouped        (new)
  help topics           every guide and reference topic, one line each
  help keys             every key: after the prefix, in each mode, launcher,
                        Layouts panel, chat, mouse                   (new)
  help <command>        one command, in a fixed page shape (below)
  help <topic>          one activity, self-contained
  help all              the index: agent tasks; for a person, commands too
  manual                everything, with worked examples
Ctrl-Space ?            the prefix keys, grouped; in a mode, that mode's keys
```

Why each layer exists:

- **help commands** is the handoff's "obvious way to discover every
  command and its purpose". Nothing does that today. It is git's `help -a`.
- **help topics** does the same for topics. It is git's `help -g`.
  `help help` keeps showing this list, because the help command's own
  page is this list.
- **help keys** takes the key reference out of `help panes`. Then `panes`
  can explain the model and `keys` can be scanned. Ctrl-Space ? and
  `help keys` read the same source file, so the panel and the page cannot
  disagree.
  - Feasibility: `help_text.h` already lets any layer read the compiled
    pages, so the compositor can draw from them.
- **help all** is an index, never a dump. For an agent it is the agent
  task index; for a person, every command plus the guides plus the agent
  tasks (section 6). The 43 KB of every page joined together goes only
  into `manual`. Why: you never meant `help all` to dump everything, and
  it is the first thing an agent runs.
- **manual** stays as the long read, so nothing that exists today is
  lost.

### Topics

| Topic | For | Covers | Made from today's |
| --- | --- | --- | --- |
| panes | A person at the keyboard | Windows, places, views, layouts, launcher, bar, chat, detach: the model. Points to `help keys` for keys. | `panes` without its key lists, plus the person's side of `share` |
| keys | Anyone | Every key | The key lists in `panes`, plus `mux_help.c` |
| tui | An agent or a script driving a terminal app, including one a person is using. Assist sends agents here. | The message header and how to reply; `peek`, `send`, `wait`, `resize`, `msg`; testing your own app | `tui`, the agent's side of `share`, and the header part of `messaging` |
| agents | An agent running other agents | Start one, wait for its prompt, give it work with `msg`, watch with `peek`, get replies, `end`/`purge` | `agents` |
| messaging | A harness outside backmux | `inbox`, `monitor`, `--wait-reply`, expiry | `messaging` |
| service | Jobs and services | `-d` and `-dw`, the exit code from `wait`, restart policies | `service` |
| launch, targets, system, scripting, aliases | Reference | Same scope as today, rewritten to the style below | Same pages |
| features, examples | A first look, or a long read | Kept, and included in `manual` | Same pages |

**`share` is folded into `tui` and `panes`.** `tui` takes the agent's
side and `panes` takes the person's side; `help share` and `help users`
redirect to `tui`.
- Why: your handoff names this exact split as the problem. Today someone
  joining a live app needs `tui`, `share`, `messaging` and `agents` all at
  once. Keeping `share` as its own page keeps the split.

**Size limits**, so an agent knows what a page costs:
- front page: 50 lines or fewer;
- `help commands`: 50 lines or fewer;
- each activity topic: 100 lines or fewer (about 1,500 tokens);
- `help all`: 25 lines or fewer for an agent, 60 for a person (from
  43 KB today);
- each agent task page: 25 lines or fewer (about 350 tokens).

### Command page shape

Every command page has the same shape:

```
usage: backmux peek <target> [--json] [--ruler] [--last-alt]
                    [--image <file> [<image options>]]

Read a terminal process's screen as text, JSON, or a PNG.

OPTIONS
  --json            print the screen with cursor, modes, size, counter
  ...

DETAILS
  (behavior, scope, consequences, the exit codes that matter here)

EXAMPLES
  (only when the usage line alone doesn't show how)

See also: backmux help tui, wait, send.
```

- The purpose line is word for word the line `help commands` shows for
  that command.
- Why one shape:
  - A reader always knows where on the page to look.
  - `help all` can take the usage and OPTIONS sections mechanically.
  - `help commands` can take the purpose line, and a test fails if the
    index and a page ever disagree.
  - Today pages vary: some have an OPTIONS section, others describe their
    options in paragraphs.

### Writing rules (these go into docs/help.md)

- **Labels and one-line descriptions:** imperative, sentence case, verb
  first ("Toggle zoom", "Swap two places", "End it now: SIGKILL"). Why:
  your critique asked for this, and git and tmux both write this way
  (tmux's list: "Split window vertically", "Rename current session").
- **State the consequence** when it decides what to press: "Detach; it
  keeps running", "Remove ended, unsaved records".
- **Notation:**
  - Arguments in angle brackets, as your front page wrote them:
    `<target>`, `<program>`, `<args>`.
  - `[...]` means optional, and `...` means it can repeat.
  - `Ctrl-Space q` means press Ctrl-Space, release it, then press q.
    `Ctrl-x` means hold both together.
  - This notation is explained once on the front page and once in
    `help keys`.
- **No jokes.** Your original page had three. Since then you have asked
  for clear, professional, technically accurate help.
- **No lists of warnings for agents.** Give the meaning and the syntax,
  one line per fact. Why: your handoff says agents are intelligent
  readers.
- **80 columns**, and every page ends with where to go next.

## 5. Mockups

The wording is a first pass. The structure is what needs your decision.

### Front page (`backmux`, `backmux help`)

```
backmux 1.0.0 - keeps programs running and shares their terminals

LAUNCH PROGRAMS
  backmux shell [<shell>]                 Start your shell, with panes and chat
  backmux call <program> [<args>]         Start a terminal program, attached
  backmux detach <program> [<args>]       Start a terminal program, detached
  backmux server <program> [<args>]       Start a program with no terminal
  Put --name <name> before <program> to name it; otherwise one is made.

MANAGE PROCESSES
  backmux list [all]                      List running processes (all: ended)
  backmux attach <target>                 Attach this terminal to it
  backmux tail <target>                   Follow its new output
  backmux info <target>                   Show its state, command, settings
  backmux end <target>...                 End it: SIGTERM, SIGKILL after 5 s
  backmux kill <target>...                End it now: SIGKILL
  backmux recall <target>                 Start an ended one again, as before
  backmux purge                           Remove ended, unsaved records
  backmux purge all                       End and remove all unsaved ones

AUTOMATE AND MESSAGE
  backmux peek <target>                   Read its screen: text, JSON, or PNG
  backmux send <target> <text>...         Type into it; -k <key> sends keys
  backmux wait <target> [--match <text>]  Wait for text, quiet, or exit
  backmux msg <target> <text>             Message it; it can reply to you

MANAGE PROFILES
  backmux save <target>                   Keep its record through purge
  backmux set <target> <setting>=<value>  Change program, args, dir, or name
  backmux unset <target> <setting>        Clear a setting
  backmux delete <target>                 Remove an ended record, even saved

<target> is a process name, its ID, or the place it is on screen (2C).

TERMINAL CONTROLS  (press Ctrl-Space, release it, then the key)
  Ctrl-Space q                            Detach; the process keeps running
  Ctrl-Space c                            Toggle the chat window
  Ctrl-Space ?                            Show all keys

HELP AND REFERENCE
  backmux help <command>                  One command: usage, options, details
  backmux help commands                   Every command, one line each
  backmux help topics                     Guides: panes, keys, tui, agents...
  backmux help all                        Every command's usage and options
  backmux manual                          All of it, with worked examples
```

What changed and why:
- **Kept:** your groups, your order, and your two `purge` lines. The
  headings from 45c0b96 stay too. They are already on the page, and your
  critique asked for headings.
- **Added:**
  - AUTOMATE AND MESSAGE, because those commands are half the product and
    were invisible.
  - The `<target>` line, because every command uses it.
  - `help commands` and `help topics`, so every command and every topic is
    one step away.
- **Removed:** `help panes` from the keys group, because `help topics`
  lists it next to `help keys`.

### help commands

```
backmux help commands - every command

LAUNCH                                    WATCH
  shell    Start your shell, panes, chat    list     List processes
  call     Start attached, on a terminal    ps       Docker-style process list
  detach   Start detached, on a terminal    info     Show a process's details
  server   Start with no terminal           inspect  Print its record as JSON
                                            logs     Print its captured output
DRIVE                                       tail     Follow its new output
  attach   Attach this terminal to it       stats    Show its CPU and memory
  peek     Read its screen                  ports    Show its listening ports
  send     Type text, keys, mouse events
  wait     Wait for text, quiet, or exit  STOP AND REMOVE
  resize   Change its terminal size         end      End: SIGTERM, then SIGKILL
                                            kill     End now: SIGKILL
MESSAGE                                     purge    Remove ended records
  msg      Message a process or person      delete   Remove one, even saved
  inbox    Read replies sent to you
  monitor  Follow replies as they come    PROFILES AND SETTINGS
                                            recall   Start an ended one again
PANES                                       save     Keep its record (profile)
  swap     Exchange two panes' places       rename   Rename it and save it
  layout   Save and load pane layouts       set      Change a profile setting
                                            unset    Clear a profile setting
HELP                                        config   Change your defaults
  help     Show help on a command, topic
  manual   Print the whole manual

Aliases: start = recall, stop = end, rm/prune/drop = purge,
console = attach. Docker forms (run -it, run -d, ps -a): help aliases.
```

All 34 commands appear. Two columns keep the page under 40 lines.

### Ctrl-Space ? (the keys panel)

```
╭─ Keys ───────────────────────────────────────────────────────────────────╮
│ Press Ctrl-Space, release it, then a key. Alt-arrows need no prefix.     │
│ Places are a window number and a letter (2C), as the name bars show.     │
│                                                                          │
│ OPEN                                 ARRANGE                             │
│  Enter       New terminal (shell)     s       Swap two places            │
│  |           Split right: terminal    m       Move mode                  │
│  -           Split down: terminal     r       Resize mode                │
│  l           Open the launcher        z       Toggle zoom                │
│                                       L       Open the Layouts panel     │
│ MOVE AROUND                                                              │
│  Alt-arrows  Focus pane; at edge,     VIEW                               │
│              slide to next window     a       Toggle sort by name        │
│  o  O        Focus next, previous     w       Toggle spread over windows │
│  t           Window mode              d       Toggle pack, four a window │
│  p           Pane mode                f       Toggle the bar             │
│                                       i       Toggle name bars           │
│ THIS PANE                                                                │
│  n           Rename its process       CHAT AND HELP                      │
│  ;           Save its profile         c       Toggle chat                │
│  \           Lock: Alt-arrows to it   ?       Show these keys            │
│  q           Detach; it keeps running Ctrl-Space  Send one Ctrl-Space    │
│                                                                          │
╰─ In a mode (p t r m), ? lists its keys and Esc leaves · any key closes ──╯
```

How this answers your critique, point by point:
- Headings group by function.
- Every label is a verb and an object.
- Toggles say "Toggle".
- Each mode has its own entry, and the footer says what a mode is and
  how to leave one.
- The header explains the prefix and places.
- Detach and lock state their consequences.
- Saving the profile (`;`) and saving a layout (`L`) are in different
  groups, so they can't be mistaken for each other.
- Colors stay as they are now: amber keys, blue headings, a framed box.

Inside a mode, `?` shows that mode's panel:

```
╭─ Pane mode ───────────────────────────────╮
│ Keys act on panes until you leave.        │
│                                           │
│  arrows, h j k l  Focus the pane that way │
│  a                Attach a process here   │
│  g                Go to a pane by name    │
│  z                Toggle zoom             │
│  Esc, Enter       Leave pane mode         │
╰─ ? or Esc closes this ────────────────────╯
```

### help tui (the page Assist sends agents to)

```
backmux help tui - inspect and drive a terminal app

backmux keeps every terminal process's screen in memory, whether or not
anyone is attached. Read it and type into it from any shell, while a
person watches or works in the same app.

WHO ASKED
  A message starts with a header:
    [msg from NAME; reply: backmux msg NAME "..."]
  Reply with that command. The message names the app; backmux list shows
  every running process, and a place on screen (2C) also works as a name.

LOOK
  backmux peek app                       the screen as text
  backmux peek app --ruler               with column and row numbers (from 0)
  backmux peek app --json                text, cursor, modes, size, counter
  backmux peek app --image s.png --cursor-ring    a color PNG to look at
  backmux peek app --last-alt            the full-screen frame it last showed

ACT
  backmux send app 'text'                type text; nothing else is added
  backmux send app -k Enter              keys: Enter Escape Tab Up F5 C-c M-x
  backmux send app ':wq' -k Enter --wait 300   then output, then 300 ms quiet
  backmux send app --mouse click,4,0     column 4, row 0; exit 9 if mouse is off
  backmux resize app 100 30              new size; the app gets SIGWINCH

WAIT
  backmux wait app --match 'Ready'       until the text is on screen (30 s)
  backmux wait app --idle 300            until 300 ms without output
  backmux wait app                       until it exits; prints its exit code

TALK
  backmux msg NAME "..."                 reply; a person sees it in chat
  ...

TEST YOUR OWN APP
  backmux detach --name app --size 80 24 ./my-tui
  ... (start, drive, check, end, purge)

Details: backmux help peek, send, wait, resize, msg.
```

About 80 lines in full. The agent needs nothing else to start.

## 6. Help that knows its reader

`backmux help` looks at who is running it. It addresses that reader
directly, and leaves out what doesn't apply to them.
- Why: an agent inside backmux misread help written for senders outside
  it, and reported features as missing that already existed (section 7).
  Every token an agent reads costs it, so a page for someone else is cost
  with no benefit.

### What help can tell, without changing anything

Every signal below exists today. Help only reads them; it never creates an
inbox or a record.

| Signal | Source | Tells |
| --- | --- | --- |
| `BACKMUX_ID` or `BACKMUX_NAME` set | Set by the launch for every process (`runtime/start.c`) | Inside or outside backmux. `msg` uses the same rule to choose its sender (`screen_cmds.c:1184`), so help describes exactly what `msg` will do. |
| `BACKMUX_NAME`, `BACKMUX_ID` | Same | Its own name and address |
| `BACKMUX_PARENT` | Set when a process was launched from inside another one | Who started it, so it knows where to report |
| A known harness among its ancestors | The list chat already uses (`console/agents.c` plus config `agents`), checked from help's parent up to the backmux broker | Agent or person. The check looks at the process name, `argv[0]`, and the directories of the executable's path, because Claude Code's installed binary is named after its version (`~/.local/share/claude/versions/2.1.332`, as seen on this machine). |
| Newest entry in its received messages | What `msg --received` reads | Who last wrote to it, and the exact reply command |
| An inbox already keyed to its harness | `hold_operator_name` with create off | An outside agent's inbox name, if it has one |
| The person's chat name | The "person uid" inbox key, read only | How agents reach the person |

### Who sees what

| Reader | Front page | Left out |
| --- | --- | --- |
| Agent inside backmux | Addressed to it: its name, that messages arrive as its input, the last sender's reply command, who started it. Then the commands for driving apps, messaging and running programs. 20 lines or fewer (about 300 tokens). | Keys, `attach`, `shell`, `call`, profiles, and the outside flow (`inbox`, `monitor`, `--wait-reply`) |
| Agent outside backmux | Addressed to it: its inbox name if it has one (otherwise that its first `msg` makes one), and how replies arrive. Then the same commands. 20 lines or fewer. | Keys, `attach`, `shell`, `call`, profiles, `--received` |
| Person outside backmux | Your front page, unchanged | Nothing |
| Person inside backmux | Your front page, plus one line: "Agents reach you as `<chat name>`." | Nothing |

Topics and command pages work the same way.
- **Only the sections for this reader are printed.** For example, `help
  msg` for an agent inside drops the `--wait-reply` section, and `help
  messaging` for an agent inside says in its first line that replies
  arrive as input and points to `agents`.
- **One line at the top names the view and how to see everything:**
  `help for you: an agent inside backmux · all: backmux help --full`.
  - Why: a page that has been filtered must say so. Otherwise a missing
    section reads as a missing feature, which is sleek_carbon's mistake
    in reverse.
- **`help all` and `manual` are never tailored.** They are the complete
  reference.
- **`--full` prints any page complete, with no personal lines.** Why:
  - the test suite must not depend on who runs it (a suite started from
    Claude Code would otherwise get the agent pages);
  - someone writing docs needs the whole page;
  - an agent writing instructions for a reader in another situation
    needs that reader's view.

### Mockups

The tool doesn't speak as a persona. It names the process politely as a
labeled field: `Your Agent ID: Sleek Carbon`, with underscores turned into
spaces and each word capitalized. The literal `sleek_carbon` appears only
in commands that need it, because that form exists for the Docker
resemblance.

For an agent, `backmux help`, `backmux help all` and `backmux help agents`
all print the same page: who it is, then its task index. Every task is a
small page addressed to it, ending with the next step. Nothing it doesn't
need is shown.

Agent inside backmux (Sleek Carbon's case):

```
backmux 1.0.0 · agent help, inside backmux · every page: help --full

Your Agent ID: Sleek Carbon
Started by:    Lead           (backmux msg lead "..." reports back)
Last message:  Rich Chat, 08:41; reply with backmux msg rich_chat "..."
Messages to this process arrive as input, after a header with the reply
command. Nothing needs to poll for them.

TASKS  backmux help <task>
  assist     Help the person with the app on their screen
  look       Read a terminal app's screen: text, JSON, or an image
  drive      Type text, keys and mouse into it; wait for the result
  reply      Message a person or a process
  test       Start an app in a terminal of its own and test it
  serve      Run a server and wait until it is ready
  job        Run a command in the background; get its exit code
  delegate   Start another agent and give it work
  check      See what is running: details, output, ports
  cleanup    Stop processes and remove their records

Next: backmux help assist
```

The Next line is the likely first task. It suggests `assist` when the last
message came from a person's chat, and `look` otherwise.

A task page (`backmux help look`):

```
backmux help look · Sleek Carbon · every page: help --full

A terminal process's screen is kept in memory whether or not anyone is
attached, so a person's app can be read while they use it.

  backmux peek app                       the screen as text
  backmux peek app --ruler               with column and row numbers, from 0
  backmux peek app --json                text, cursor, modes, size, counter
  backmux peek app --image s.png --cursor-ring    a color PNG to read
  backmux peek app --last-alt            the full-screen frame shown last

Pictures and colored blocks show only in --image. app is a name, an ID,
or a place on screen (2C); backmux list shows the names.

Next: backmux help drive (type into it), reply (report what it shows)
```

Agent outside backmux (an agent like me, in Claude Code here). Its ID is
its inbox:

```
backmux 1.0.0 · agent help, outside backmux · every page: help --full

Your Agent ID: Grand Soldier  (replies to backmux msg wait in its inbox)
Read replies:  backmux inbox --wait     (backmux help replies)

TASKS  backmux help <task>
  look, drive, reply, replies, test, serve, job, delegate, check, cleanup
  (one line each, as above)

Next: backmux help look
```

When it has no inbox yet, the ID line reads: `Your Agent ID: none yet;
your first backmux msg makes one`.

A person's `backmux help all` is the index of everything: every command,
one line each; the guides; and the agent tasks, so they can see what an
agent can do and point one at a page.

```
backmux help all - everything backmux does

COMMANDS  backmux help <command>
  (the grouped two-column index from section 5)

GUIDES  panes, keys, service, launch, targets, system, scripting, aliases

AGENT TASKS  what an agent sees with backmux help; point one at a page
  assist  look  drive  reply  replies  test  serve  job  delegate  check
  cleanup

All of it in one text: backmux manual
```

Lines that would be empty are dropped. With no message received yet there
is no "Last message" line, and a process that no other process started
has no "Started by" line.

### How it is built

- **Tags in the page source.** A section heading carries the readers it
  is for in brackets, for example `SEND FROM OUTSIDE BACKMUX  [outside]`.
  The tags are `agent`, `person`, `inside` and `outside`, and they
  combine. A section with no tag is for everyone.
  - Personal lines use placeholders (`{name}`, `{parent}`, `{last_from}`).
    A line is dropped when its placeholder has no value.
  - The pages stay plain text you can read on GitHub, and
    `scripts/gen_help.sh` doesn't change.
- **The runtime layer works out the reader once.** A new
  `runtime/help_context.c` reads the environment, the ancestry and the
  store, and returns a plain struct.
- **`core/help.c` prints with that struct.** It filters by tag and fills
  the placeholders, with no IO of its own, so the layer rule holds.
- **Tests.**
  - Page comparisons use `--full`.
  - New tests run `backmux help` under a fake harness (sh copied to a temp
    directory as `claude`), with and without `BACKMUX_ID`. They check each
    view's content, and that each agent front page is 20 lines or fewer.

### What can go wrong

- **A wrong guess.**
  - A person who runs `backmux help` from inside an agent's shell (Claude
    Code's `!` prefix) gets the agent view. The top line says so, and
    `--full` is one word away.
  - A harness backmux doesn't know gets the person view, which is
    complete. `--agent` or config `agents` teaches it, which is the same
    list chat uses.
- **The same command prints different text for different readers.** Bug
  reports and screenshots will differ. The top line names the view, so a
  reader can always tell which one they are looking at.

## 7. Gaps from an agent's session

An agent, sleek_carbon, ran as Claude Code inside backmux on 2026-11-27
after an Assist request. It inspected nyancat, ran a web server, started a
subagent and exchanged messages. Its notes are in
`/home/rich/test/backmux-help-gaps.md`. Section 6 is the answer to its
main finding. The items below are the ones I
checked against the code or a run.

### Its main finding: the agent can't tell which case it is in

It read `help messaging`, which is written for senders outside backmux,
while it was inside. It concluded that send-and-wait was missing, when in
fact replies were arriving as its input. It read `help service`, which
says "a background service has no screen for wait --match". It concluded
that readiness waits were missing, when in fact they work for terminal
processes.

Per-task pages only help if the agent lands on the right page.

### Help text (folded into this design; no behavior change)

- **Every topic's first line says who it is for and where the other case
  is.** For example, on `messaging`: "For a sender outside backmux. Inside
  a backmux process, replies arrive as your input: see help agents." Why:
  inside versus outside is the split between `messaging` and `agents`, and
  the reader can't see it from the page. With section 6, help picks the
  right case on its own, and this line remains for `--full` readers.
- **`service` gets a section, "Start a service and wait until it is
  ready".**
  - Use `-dt` with `wait --match '<ready line>'`. It exits 0 when ready,
    and 8 if the program ends first, after which `wait` prints the exit
    code. I verified this with `python3 -m http.server`, including on a
    busy port.
  - Use `-d --restart` with a curl check when the service must restart,
    because terminal processes can't take `--restart`.
  - Why: today the page says the opposite, and sends the reader to a curl
    loop.
- **`agents` gets three lines:**
  - Replies arrive as input in the lead's own prompt, so nothing needs to
    poll, and `msg --received` lists them.
  - A reply needs no acknowledgement. Answering one starts a loop.
  - An agent started from another agent's shell inherits its environment.
    The subagent inherited `CLAUDE_CODE_CHILD_SESSION` and turned off
    transcript saving. `env -u NAME` avoids it.
- **`tui` gets one line under LOOK:** pictures made of colored blank cells
  show only in `--image`. The text view of nyancat was empty.
- **`service` says what the PORTS column in `list` holds,** and that
  `ports <target>` shows the listening sockets. The agent took the empty
  column for leftover Docker layout.
- **The `--wait-reply` error inside a process is reworded**
  (`src/runtime/screen_cmds.c:1190`). It says the message was not sent,
  and that replies arrive as program input so no wait is needed. The
  refusal and the exit code stay. Why: today it reads as a missing feature
  and doesn't say the message was dropped. This is user-facing text, so it
  belongs to this rewrite.

### Behavior: yours to decide

| Change | For it | Against it / cost |
| --- | --- | --- |
| `purge` takes several targets, as `end` and `kill` do | `rm` is a purge alias, and Docker's `rm` takes several. The agent ran `end a b` and then `purge a b`, and got the usage line. | Parser limit 1 becomes -1, with each target handled on its own, as `end` already does. The keyword `all` stays reserved. |
| The `peek` text view notes on stderr when the screen has colored blank cells ("N colored cells not shown; use --image") | The agent saw a blank screen and couldn't tell why | A new stderr line; the `tui` line above may be enough |
| `msg` to an inbox (a person's chat name) prints something, as `msg` to a process prints its delivery ID | The two cases are inconsistent; the sender gets no sign of success | Unix tools are silent on success. `peek --image` should stay silent for that reason, since scripts use its exit code. |
| **Decided by you:** a friendly name resolves as a target: "Sleek Carbon" (any case, spaces for underscores) finds `sleek_carbon`, in every command and in chat's recipient search | Help shows names in that form, so whoever types one back must get the process | An exact name still wins. A friendly form that fits two names is refused (exit 6), like any other ambiguous target. |
| `wait --port N`, or `--restart` for terminal processes | Readiness for services that print no ready line, or restart together with readiness | New features: later, if at all |

## 8. The work, once you approve

1. Update the glossary with the settled terms.
2. Write the front page, `help commands` and `help topics`, with the
   tailored views from section 6 (`runtime/help_context.c`, tags and
   placeholders in `core/help.c`, `--full`). The agent views come first,
   because they are what Assist and worker agents read.
3. Write `help keys` as the one source, and have `mux_help.c` draw the
   panel and the four mode panels from it.
4. Rewrite the 34 command pages to the shape, and change the usage lines
   in `specs.c` to the notation.
5. Write the topics: `panes`, `tui` (with `share` folded in), `agents`,
   `messaging`, `service`. Restyle the reference pages.
6. Build `help all` from the command pages. `manual` becomes `help all`
   plus the topics plus examples.
7. Tests. The existing help test (pages compiled in, aliases covered,
   80 columns) stays. New checks:
   - each index line matches its page's purpose line;
   - every command appears in `help commands`;
   - every key in the panel source is handled in `mux_command`, and every
     key handled there is in the source.
8. Rewrite docs/help.md to describe this design, and update README's
   mentions of help.

One commit per step or group of steps, CI before each commit, and an
install after each so you can test.
