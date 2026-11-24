# backmux: the surface (design, not built yet)

backmux is callbox renamed and reshaped: the same keeper, broker, store and
agent verbs underneath, with a native command surface, one floating window at
a time, and simple tiling. Every line below carries its reason; anything that
cannot state one does not go in.

**The model: nothing has chat and nothing has splits.** Chat is free
floating: `Ctrl-P Ctrl-T` brings it up over whatever you are attached to, and
it belongs to you, not to any call. Splits exist only because `Ctrl-P Ctrl-A`
attached a second call beside the first. No launch verb, `shell` included,
carries either. Why: chat and splits are things a person does, not
properties of a program; tying them to a kind of call (as callbox's chat
shell did) made people pick a mode up front to get them.

**Muxing is optional, and nothing muxes by default.** Every call works
exactly as it does now: one program, the whole terminal, attach and detach as
today, and programs are started from the command line as now. Panes appear
only when someone asks for a second one: `Ctrl-P Ctrl-A` brings another
running call in beside the one they are attached to. Closing back to one pane
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

 backmux attach <name|id>               attach a running call to your terminal
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
- **`backmux set` / `unset` change a call's settings,** running or saved;
  the profile window does the same. Why: what a call runs and how its
  messages arrive belong to the call, not to how it happened to be started,
  and changing one setting is simpler than relaunching by hand.
- **The settings:**

  | Setting | Means | Unset |
  |---|---|---|
  | `agent` | messages arrive in chat formatting | messages arrive raw |
  | `working=<path>` | the folder it starts in | starts where you stand when it is launched |
  | `program=<program>` | the program it runs | refused: a call without a program is nothing |
  | `args=<args>` | the program's arguments | runs with none |
  | `name=<name>` | the call's name (a rename) | refused: every command and agent addresses a call by name |

  Why these: they are what a launch is (what runs, with what, where), what
  the call is called, and the one mode a call has. `program`, not
  `target`: "target" already means the call a command addresses
  (`send <target>`).
- **`name` takes effect at once,** running or not, and the old name stops
  working at the same moment: no alias is kept. Why: it is only the
  address, nothing in the program depends on it, and an old name that still
  answered could later be claimed by another call and reach the wrong one.
- **The id never changes.** It is the call's permanent identifier (a
  generated hex id, unique like a GUID), so `<name|id>` targets keep
  working through any rename. Why: scripts and agents that must not lose a
  call hold its id; names are for people.
- **`agent` is a live toggle: it takes effect from the next message, with no
  relaunch; `working`, `program` and
  `args` from the next launch** (`recall`, or a profile's launch). Why: a
  running program's folder, program and arguments cannot change under it;
  how a message is delivered can.
- **`--name` applies to every launch verb.** Why: the name is what every
  other command and every agent takes; whoever starts a call may want to
  choose it.
- **`end` stops gracefully, `kill` forcefully; both are native.** Why: a
  call you never attached to (a `server`) needs a way to stop that does not
  also delete records, and a program that ignores the polite signal needs
  the big hammer.
- **`shell` is a shell under backmux in a TTY, nothing more.** Why: chat
  and splits are not modes of a call (above).
- **`recall` means only relaunching an ended call.** Why: one word, one
  meaning; the chat key says "bring back".
- **`info <name>` works for any name, running or ended,** and shows the
  working folder, arguments, environment, launch kind, state, times and
  paths. Why: everything needed to understand or reproduce a call, in one
  place.
- **`swap A,B` takes call names** (also `swap A B`). Why: an agent knows
  names, not where panes sit on someone's screen. The comma is safe: names
  allow only letters, digits, `_` and `-`.
- **`list all` and `purge all` are positional; `all` is a reserved name.**
  Why: positional reads like the rest of the native surface, and a call
  named `all` would be ambiguous.
- **`purge all` lists what it will end and asks first.** Why: it is the only
  command that both stops live calls and deletes them, and people run real
  agents.
- **`purge` alone touches only stopped calls not saved.** Why: cleaning up
  must never end anything running.
- **Wrong-kind targets (open):** `tail` on a `detach` call reads its log;
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
| `Ctrl-P Ctrl-H` | help |
| `Ctrl-P Ctrl-A` | attach another running call: this is how muxing starts |
| `Ctrl-P Ctrl-E` | edit profiles |
| `Ctrl-P Ctrl-N` | identify: every pane's name |
| `Ctrl-P Ctrl-P` | a literal `Ctrl-P` to the program |

- **`Ctrl-P` waits for the second key, with no time limit.** Why: not
  everyone types fast.
- **`Ctrl-P` then any key that is not a command cancels; both keys are
  dropped.** Why: with six commands (plus `Ctrl-P` itself), a wrong second key is almost
  always a mistyped command, and passing it on would hit the program with a stray
  `Ctrl-P` (previous command in a shell, completion in vim) plus a random
  key. This replaces the Docker-style rule of sending both through.
- **A mouse event is not the second key;** it is handled and the lead key
  goes on waiting. Why: moving the mouse should never answer a key prompt.
- **Letters that cannot be used:** `Ctrl-M` (the same byte as Enter),
  `Ctrl-I` (Tab), `Ctrl-[` (Esc). Why: the terminal sends identical bytes,
  so backmux cannot tell them apart. `Ctrl-H` is safe on nearly every
  terminal (Backspace sends DEL); where Backspace sends `Ctrl-H`, the worst
  case is opening help.

## Floating windows

- **One floating window at a time; Esc always closes it.** Why: one rule to
  remember, and nothing piles up over the work.
- **Floating windows are pinned to nothing.** They sit over the whole
  screen and move freely across split lines; no pane, call or window owns
  them. Why: chat and help are the person's, not any program's, so they go
  where the person puts them.
- **There is only one, wherever you are.** Call it up from another backmux
  terminal and it comes to you there, with its state. Why: one person, one
  chat; two copies would split one conversation.
- **Only backmux TTYs get them** (a `call`, `agent`, `detach` or `shell`
  you are attached to). A `server` console never does. Why: a server is raw
  backgrounding (with or without a waiter, as it was launched): plain output
  to a log, read with `tail`. It is not a TTY, so there is no screen to draw
  on.
- **Chat (`Ctrl-P Ctrl-T`) comes up wherever you are and follows you:**
  move to another terminal and it is drawn there; the draft, the open
  conversation and the window's position travel with you. "Where you are" is
  the terminal you last pressed a key in. Why: chat belongs to the person,
  not to one shell.
- **Your chat name** is one you pick, or a generated one. Why: either works;
  what matters is that it is shown (below).
- **Help (`Ctrl-P Ctrl-H`)** is fixed and centered. Why: it is read, not
  worked in.
- **No launcher window.** Programs start from the command line, as now
  (from a shell pane, `backmux call ...` opens it beside). Why: the floating
  window brings running calls into view; starting things already has a
  surface, and a second one would be two ways to learn.
- **Attach (`Ctrl-P Ctrl-A`):** the top half lists the running TTY calls by
  name, one highlighted; the bottom half shows the highlighted call's info:
  program, arguments, working folder (what `backmux info` shows). Arrows
  move, Enter brings it in as a new pane, placed like any other. Why: being
  attached to one call and wanting to see a second is the moment someone
  wants a mux, and attaching is the word they already know; a name alone
  does not say which call is which, its program and folder do. Pressed while
  attached to a single call, this is what turns the screen into panes.
- **Swap (open where it lives):** proposed as `s` in the Attach window: big
  letters A–D appear in the panes' corners and two letters swap two panes.
  Why: two key presses, no layout language, and no extra key to learn.
- **Profiles (`Ctrl-P Ctrl-E`):** edit or launch any saved profile.
- **Open:** opening a second floating window while one is up: does Esc on
  the second bring the first back where it was? Proposed yes, so a glance at
  help never loses a conversation.

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
  is opt-in (above).
- **Placement is automatic:** the second pane takes the left, the third the
  bottom of the left column, the fourth the bottom of the right column.
  Focus goes to the new pane. Why: screens are widescreen and a cell is about
  twice as tall as wide, so a terminal has three to four times more columns
  than rows; rows are the scarce resource, and splitting left/right first
  keeps the full height for as long as possible. No layouts to choose.
- **Only TTY calls can be panes.** `Ctrl-P Ctrl-A` lists only them; a
  `server` cannot be tiled. Why: a pane shows a screen, and a server has
  none: its output is a log.
- **At most four panes on a page; a fifth starts a new page.** Why: past
  four, panes get too small to use, and four is easy to address (A–D).
- **Swaps are the only rearranging:** `backmux swap A,B`, or the two-letter
  swap from the keyboard.
  Why: no layout system, nothing to learn beyond "these two trade places".
- **Click a pane to move to it.** Why: the mouse is already there.

## Names

- **Every launch and attach, of any kind, shows the call's name in the upper corner;
  never the command.** Why: the name is what `msg`, `attach` and every agent
  take, and the only thing the person cannot otherwise find out; the command
  is what they just typed.
- **Moving the mouse into a pane shows its name for 2 seconds.** Why: with
  four panes, which one is which is the question every time.
- **`Ctrl-P Ctrl-N` shows every pane's name on demand.** Why: the keyboard
  way to the same answer.

## Underneath

- **With one pane, nothing new runs:** the attach path is today's. Why:
  people who never split pay nothing for splits, in behavior or in moving
  parts.
- **Once a second pane exists, one process owns the person's terminal** and
  draws the panes and the floating window over them; each pane is a call's screen, fed by its
  broker. Why: splits and an overlay that is not confined to one program
  need one place that composes the whole screen; the brokers keep doing one
  job (keep the program, hold its screen, answer peek/send/wait/msg).
- **Agents address calls by name, as now.** Why: panes are only how a person
  looks at calls; nothing an agent uses depends on the layout.
- **Layout survival (open):** re-attaching should bring back the same pages
  and panes. Whether a saved profile can hold a whole page (four agents in a
  2x2) is to be decided.
