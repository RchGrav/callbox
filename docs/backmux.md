# backmux: the surface (design, not built yet)

backmux is callbox renamed and reshaped: the same keeper, broker, store and
agent verbs underneath, with a native command surface, one floating window at
a time, and simple tiling. Every line below carries its reason; anything that
cannot state one does not go in.

**The model: nothing has chat and nothing has splits.** Chat is free
floating: `Ctrl-P Ctrl-T` brings it up over whatever you are attached to, and
it belongs to you, not to any process. Splits exist only because `Ctrl-P Ctrl-A`
attached a second process beside the first. No launch verb, `shell` included,
carries either. Why: chat and splits are things a person does, not
properties of a program; tying them to a kind of process (as callbox's chat
shell did) made people pick a mode up front to get them.

**Muxing is optional, and nothing muxes by default.** Every process works
exactly as it does now: one program, the whole terminal, attach and detach as
today, and programs are started from the command line as now. Panes appear
only when someone asks for a second one: `Ctrl-P Ctrl-A` brings another
running process in beside the one they are attached to. Closing back to one pane
is the old screen again. Why: the tool earns its name by making splits simple
and powerful for those who want them, without making anyone who does not
want them learn, see, or pay for any of it.

## Commands

```
backmux 1.0.0-dedbeef

 backmux call <program> [<args>...]     attached full screen tui app
 backmux agent <program> [<args>...]    applies chat formatting to messaging
 backmux server <program> [<args>...]   detached server console app
 backmux detach <program> [<args>...]   detached full screen tty app
 backmux shell                          backmux shell mode in a tty
 backmux agent --name <name> <program>  override generated name on first run

 backmux attach <name|id>               attach running process to your terminal
 backmux tail <name|id>                 tail a server console app
 backmux end <name|id>                  end any process gracefully
 backmux kill <name|id>                 big hammer ends any process forcefully
 backmux recall <name|id>               recall ended proc w/ same path and args
 backmux info <name|id>                 details on any process known to backmux

 backmux list                           list only running processes
 backmux list all                       list all running and stopped processes
 backmux purge                          purge stopped processes not saved
 backmux purge all                      end and purge all processes not saved

 backmux save|delete <name|id>          saves (or deletes) a profile by name
 backmux set <name|id> <param>=<value>  edit profiles, args, start paths, name
 backmux unset <name|id> <param>        clear a profile setting, mode, arg, etc

 Ctrl-P Ctrl-Q detach process with durable handle, full reattach control or end
 Ctrl-P Ctrl-T recall floating user chat window after it has been dismissed
 Ctrl-P Ctrl-H the rest of the control keys to control backmux

 backmux help aliases                   container aliases (100% less container)
 backmux help agents                    orchestration and tui debugging details
 backmux help all                       all our switches are belong to you

 backmux manual                         examples, documentation, and explanation
```

- **Verbs, not flag combinations.** Why: `-it`, `-dt` and `-d` made people
  learn a matrix of switches to say one of four things; a verb says it.
- **The launch verbs:** `call` (attached, TTY), `agent` (attached, TTY,
  messages formatted for an agent to read), `server` (detached, plain output
  to a log), `detach` (detached, TTY). Why: these are the four things people
  actually start.
- **`agent` is `call` with the `agent` setting on; that setting is the only
  difference.** With it, a message arrives in chat formatting: a header
  naming the sender and how to reply, then the text. Without it, a message
  arrives raw: exactly the text, then Enter, as if typed. Why: an agent
  needs to know who wrote and how to answer; anything else (a shell, a CLI,
  a REPL) wants just the line, so a message can run a command in it.
- **`backmux set` / `unset` change a process's settings,** running or saved;
  the profile window does the same. Why: what a process runs and how its
  messages arrive belong to the process, not to how it happened to be started,
  and changing one setting is simpler than relaunching by hand.
- **The settings:**

  | Setting | Means | Unset |
  |---|---|---|
  | `agent` | messages arrive in chat formatting | messages arrive raw |
  | `working=<path>` | the folder it starts in | starts where you stand when it is launched |
  | `program=<program>` | the program it runs | refused: a process without a program is nothing |
  | `args=<args>` | the program's arguments | runs with none |
  | `name=<name>` | the process's name (a rename) | refused: every command and agent addresses a process by name |

  Why these: they are what a launch is (what runs, with what, where), what
  the process is called, and the one mode a process has. `program`, not
  `target`: "target" already means the process a command addresses
  (`send <target>`).
- **`name` takes effect at once,** running or not, and the old name stops
  working at the same moment: no alias is kept. Why: it is only the
  address, nothing in the program depends on it, and an old name that still
  answered could later be claimed by another process and reach the wrong one.
- **The id never changes.** It is the process's permanent identifier (a
  generated hex id, unique like a GUID), so `<name|id>` targets keep
  working through any rename. Why: scripts and agents that must not lose a
  call hold its id; names are for people.
- **`agent` is a live toggle: it takes effect from the next message, with no
  relaunch; `working`, `program` and
  `args` from the next launch** (`recall`, or a profile's launch). Why: a
  running program's folder, program and arguments cannot change under it;
  how a message is delivered can.
- **`--name` applies to every launch verb.** Why: the name is what every
  other command and every agent takes; whoever starts a process may want to
  choose it.
- **`end` stops gracefully, `kill` forcefully; both are native.** Why: a
  call you never attached to (a `server`) needs a way to stop that does not
  also delete records, and a program that ignores the polite signal needs
  the big hammer.
- **`shell` is a shell under backmux in a TTY, nothing more.** Why: chat
  and splits are not modes of a process (above).
- **`recall` means only relaunching an ended process.** Why: one word, one
  meaning; the chat key says "bring back".
- **`info <name>` works for any name, running or ended,** and shows the
  working folder, arguments, environment, launch kind, state, times and
  paths. Why: everything needed to understand or reproduce a process, in one
  place.
- **`swap A,B` takes process names** (also `swap A B`). Why: an agent knows
  names, not where panes sit on someone's screen. The comma is safe: names
  allow only letters, digits, `_` and `-`.
- **`list all` and `purge all` are positional; `all` is a reserved name.**
  Why: positional reads like the rest of the native surface, and a process
  named `all` would be ambiguous.
- **`purge all` names the processes it ends.** Why: it is the only command that
  both stops live processes and deletes them; saying which keeps it from going
  unnoticed. Saving a process is how one is kept.
- **`purge` alone touches only stopped processes not saved.** Why: cleaning up
  must never end anything running.
- **Wrong-kind targets (open):** `tail` on a `detach` process reads its log;
  `attach` on a `server` says to use `tail`. To be decided.

## Docker aliases (`help aliases` only)

The Docker words live on their own page and map to native verbs. Why: mixing
them into the main page made two dialects look like one; people who think in
Docker get a translation, everyone else never sees it.

| Docker | backmux |
|---|---|
| `run -it` | `call` |
| `run -d` | `server` |
| `run -dt` | `detach` |
| `start` | `recall` |
| `stop` | `end` |
| `ps`, `ps -a` | `list`, `list all` |
| `logs -f` | `tail` |
| `attach` | `attach` |
| `inspect` | `info` |
| `rm` | `purge <name>` |

- **`exec` maps to nothing.** Why: `docker exec` runs a new command inside a
  running container; mapped to `recall` it would relaunch instead, the wrong
  thing for anyone who knows Docker. It says there is no container to enter
  and points to `call`.

## Keys

Every command is `Ctrl-P` then `Ctrl-<letter>`. Why: one shape to learn.

| Key | Does |
|---|---|
| `Ctrl-P Ctrl-Q` | detach |
| `Ctrl-P Ctrl-T` | chat |
| `Ctrl-P Ctrl-A` | attach a running process beside this one |
| `Ctrl-P Ctrl-S` | swap panes: each pane shows `A: name`; two letters swap two |
| `Ctrl-P Ctrl-G` | go to window: press its number |
| `Ctrl-P Ctrl-M` | move the active pane to another window: press a number; you stay |
| `Ctrl-P Ctrl-F` | toggle the active pane full screen |
| `Ctrl-P Ctrl-J` | jump by name: the panes of this terminal, with their windows |
| `Ctrl-P Ctrl-W` | the next window with panes (shown bottom right when there is more than one) |
| hold `Ctrl-Space` 3 s | the keys and every pane's name, until let go |

- **Help is not a key: hold `Ctrl-Space`.** After 3 seconds the keys and
  the names show; letting go takes them away. Why: they are wanted for a
  glance, not as a window to close. Terminals report no key release, so
  "let go" is the key's auto-repeat stopping (repeats come every 30-92 ms;
  150 ms without one is let go). A tap is the program's: a second
  `Ctrl-Space` within 700 ms (key repeat starts at 250-660 ms) makes it a
  hold and every one is swallowed; otherwise the tap goes through once that
  is clear. `Ctrl-Space` was chosen because nothing holds it: Emacs and
  readline only tap it, to set a mark.
- **Help lists the keys and nothing else.** Why: a mode says what it wants
  when it is entered ("Move to window: press a number"), and what is on
  screen already (the window indicator) need not be repeated.
- **`Ctrl-P` waits for the second key, with no time limit.** Why: not
  everyone types fast. `Ctrl-P Ctrl-P` sends one `Ctrl-P` to the program.
- **`Ctrl-P` then any key that is not a command cancels; both keys are
  dropped.** Why: a wrong second key is almost always a mistyped command,
  and passing it on would hit the program with a stray `Ctrl-P` (previous
  command in a shell, completion in vim) plus a random key.
- **A mouse event is not the second key;** it is handled and the lead key
  goes on waiting. Why: moving the mouse should never answer a key prompt.
- **`Ctrl-M` is Enter's byte,** so `Ctrl-P Enter` also moves a pane.
  Nothing else uses `Ctrl-P Enter`. `Ctrl-I` (Tab) and `Ctrl-[` (Esc) stay
  unused: the terminal sends the same bytes as those keys.

## Floating windows

- **One floating window at a time; Esc closes it.** Why: one rule to
  remember, and nothing piles up over the work.
- **Floating windows are pinned to nothing.** They sit over the whole
  screen and move freely across split lines; no pane or window owns them.
  Why: chat is the person's, not any program's, so it goes where the
  person puts it.
- **There is only one chat, wherever you are.** Opened in another backmux
  terminal, it moves there, with its conversations. Why: one person, one
  chat; two copies would split one conversation.
- **Chat is the chat backmux shell always had** (picker with type to narrow,
  screen snapshots, resizing, delivery receipts), under the person's chat
  name, opening on the pane they are in. Its screen snapshot names the
  pane's process.
- **Only TTY processes get them** (`call`, `agent`, `detach`, `shell`). A
  `server` never does. Why: a server is raw backgrounding (with or without
  a waiter): plain output to a log, read with `tail`, no screen to draw on.
- **Attach lists only processes that work:** alive, with a live console.
  Why: a record and a console file can outlive a process, and hunting
  through dead entries is the annoyance.

## Panes

```
1 pane         2 panes          3 panes          4 panes
┌────────┐     ┌────┬────┐      ┌────┬────┐      ┌────┬────┐
│   A    │     │ B  │ A  │      │ B  │ A  │      │ B  │ A  │
│        │     │    │    │      ├────┤    │      ├────┼────┤
│        │     │    │    │      │ C  │    │      │ C  │ D  │
└────────┘     └────┴────┘      └────┴────┘      └────┴────┘
```

- **One pane is the default and is today's screen exactly.** Why: muxing
  is opt-in.
- **Placement is automatic:** the second pane takes the left, the third the
  bottom of the left column, the fourth the bottom of the right column.
  Why: a terminal has three to four times more columns than rows; rows are
  the scarce resource, and left/right first keeps the full height longest.
- **Four panes to a window, ten windows to a terminal,** on the number keys
  1-9 and 0. A window may be empty; `Ctrl-P Ctrl-W` skips empty ones. A new
  pane joins the window on screen when it has room, else the first that
  does. Why: past four, panes are too small to use; ten is what the number
  keys reach.
- **Letters are for the screen, names for everyone else.** `Ctrl-P Ctrl-S`
  puts `A: name` on each pane on screen; `backmux swap` takes names or ids
  only. Why: a shell or an agent cannot see which screen a letter would
  mean, and a terminal running several windows cannot guess.
- **Click a pane to move to it.** Why: the mouse is already there.

## Names

- **Every launch and attach shows the process's name in the upper corner,
  with `hold Ctrl-Space: help`;** never the command. Why: the name is what
  `msg`, `attach` and every agent take, and the only thing the person cannot
  otherwise find out; the command is what they just typed.
- **Moving the mouse into a pane shows its name for 2 seconds.** Why: with
  four panes, which one is which is the question every time.
- **Holding `Ctrl-Space` shows every pane's name** with the keys.

## Underneath

- **With one pane, nothing new runs:** the attach path is today's. Why:
  people who never split pay nothing for splits, in behavior or in moving
  parts.
- **Once a second pane exists, one process owns the person's terminal** and
  draws the panes and the floating window over them; each pane is a process's screen, fed by its
  broker. Why: splits and an overlay that is not confined to one program
  need one place that composes the whole screen; the brokers keep doing one
  job (keep the program, hold its screen, answer peek/send/wait/msg).
- **Agents address processes by name, as now.** Why: panes are only how a person
  looks at processes; nothing an agent uses depends on the layout.
- **Layout survival (open):** re-attaching should bring back the same pages
  and panes. Whether a saved profile can hold a whole page (four agents in a
  2x2) is to be decided.
