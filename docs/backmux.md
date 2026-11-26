# backmux: the surface (design, not built yet)

backmux is callbox renamed and reshaped: the same keeper, broker, store and
agent verbs underneath, with a native command surface, one floating window at
a time, and simple tiling. Every line below carries its reason; anything that
cannot state one does not go in.

**The model: nothing has chat and nothing has splits.** Chat is free
floating: `Ctrl-Space c` brings it up over whatever you are attached to, and
it belongs to you, not to any process. Splits exist only because `Ctrl-Space p a`
attached a second process beside the first. No launch verb, `shell` included,
carries either. Why: chat and splits are things a person does, not
properties of a program; tying them to a kind of process (as callbox's chat
shell did) made people pick a mode up front to get them.

**Muxing is optional, and nothing muxes by default.** Every process works
exactly as it does now: one program, the whole terminal, attach and detach as
today, and programs are started from the command line as now. Panes appear
only when someone asks for a second one: `Ctrl-Space p a` brings another
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

 Ctrl-Space q  detach process with durable handle, full reattach control or end
 Ctrl-Space c  recall floating user chat window after it has been dismissed
 Ctrl-Space ?  the rest of the control keys to control backmux

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

Every command is the prefix, `Ctrl-Space`, then one key. Why: one shape to
learn, and a prefix no shell or editor needs held back (Emacs and readline
only tap `Ctrl-Space` to set a mark, and the prefix twice still sends one).
The old prefix, `Ctrl-P`, is a shell's previous-command key, and its
`Ctrl-<letter>` second keys ran out and collided with other keys' bytes
(`Ctrl-M` is Enter, `Ctrl-J` a newline).

| After `Ctrl-Space` | Does |
|---|---|
| `q` | detach the active pane; the last one leaves the terminal |
| `c` | chat: open it with the keyboard; again, while typing in it, put it away |
| `s` | swap: two letters swap two panes; a letter and a number send a pane to that tab |
| `z` | the active pane full screen, or back |
| `f` | footers on or off |
| `n` | rename the active pane's process (asked in its footer) |
| `;` | save the active pane's process, asking for its name first |
| `\` | lock the active pane, or unlock it |
| `w` | spread: every tab used before any splits |
| `a` | alphabetize: the panes in name order, in the places they had |
| `d` | densify: four to a tab from the first |
| `p` `t` `r` `m` | pane, tab, resize, move mode |
| `?` | the keys |
| `Ctrl-Space` | one `Ctrl-Space` to the program |

- **`q` detaches.** Why: Docker's detach is `Ctrl-P Ctrl-Q`, and `d` is
  densify.
- **The prefix waits for the next key, with no time limit.** Why: not
  everyone types fast.
- **The prefix then a key that is not a command cancels; both keys are
  dropped.** Why: a wrong second key is almost always a mistyped command,
  and passing it on would hit the program with a stray prefix and a letter
  it never meant to get.
- **A mouse event is not the second key;** it is handled and the prefix
  goes on waiting. Why: moving the mouse should never answer a key prompt.
- **`?` shows the keys, on request only, and any key puts them away.** In a
  mode, `?` shows that mode's keys. Why: keys shown unasked cover the
  programs; holding `Ctrl-Space` for help (the earlier design) swallowed
  every repeated `Ctrl-Space` a program wanted, and needed timers to guess
  when the key was let go.
- **Modes keep the keyboard until Esc or Enter.** `p` pane (arrows or
  `h j k l` move between panes, `a` attach a running process, `g` jump to a
  pane by name, `z` full screen), `t` tab (`1`-`9` go there, Left/Right the
  previous or next), `r` resize (arrows or `h j k l` grow the active pane
  that way, Shift-arrows or `H J K L` shrink it from that side, `=` even),
  `m` move (arrows swap with the pane that way, `1`-`9` send it to that
  tab). A number in tab or move mode finishes the job and leaves the mode.
  Why: moving around takes several presses, and the prefix before each
  would be tiring; the resize letters are Zellij's.
- **`Alt`-arrows move between panes without the prefix,** when there is a
  pane that way and the active pane is not locked; otherwise they are the
  program's. Why: moving between panes is the most common thing done, and
  a program still gets `Alt`-arrows wherever there is nowhere to move.
  (Stock macOS Terminal sends `Option`-Left/Right as `Esc b` / `Esc f`;
  there they move only with "Use Option as Meta" on.)
- **A locked pane gives its program the keys backmux takes without the
  prefix** (the `Alt`-arrows). The prefix still works there, and twice
  still sends one. Why: one pane may run something that wants those keys
  (tmux, another backmux, an editor) while the others keep them; the lock
  says so in the pane's footer and nowhere else.
- **Rename and save ask in the active pane's footer:** Enter gives the
  name (the one it has is a fine answer), Esc gives up. Rename also saves,
  as `backmux rename` does; save renames first when the name was changed.
  Why: one line where the pane is, and a name is what a profile is known by.
- **Chat is left only with the prefix then `c`.** Esc stays a key for
  writing in it. Why: chat is not a mode to fall out of with half a message
  written.
- **`Ctrl-Space` is `ctrl-space` to `--detach-keys`** (also `ctrl-@`); the
  default is `ctrl-space,q`.

## Floating windows

- **One floating window at a time.** Esc closes the attach list and the
  names list; chat is left with the prefix then `c` (see Keys). Why: one
  rule to remember, and nothing piles up over the work.
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
│   A    │     │ A  │ B  │      │ A  │ B  │      │ A  │ B  │
│        │     │    │    │      ├────┤    │      ├────┼────┤
│        │     │    │    │      │ C  │    │      │ C  │ D  │
└────────┘     └────┴────┘      └────┴────┘      └────┴────┘
```

- **One pane is the default and is today's screen exactly.** Why: muxing
  is opt-in.
- **Placement is automatic: A left, B right, C under A, D under B.** Why:
  reading order, so the first pane stays where it was and each new one
  lands where the eye goes next; and a terminal has three to four times
  more columns than rows, so left/right first keeps the full height
  longest. Resize mode moves the lines between them, nothing else.
- **Four panes to a tab, nine tabs to a terminal,** on the number keys 1-9.
  A tab may be empty. A new pane joins the tab on screen when it has room,
  else the first that does. Why: past four, panes are too small to use;
  nine is what the number keys reach without 0, which reads as "none".
- **Letters run on across tabs:** A-D are tab 1's places, E-H tab 2's, and
  so on to Z. In swap, a letter past a tab's panes means that tab: the pane
  goes there. Why: one alphabet for every place, so going past four panes
  needs no other key; a number sends a pane to any tab.
- **Letters are for the screen, names for everyone else.** `backmux swap`
  takes names or ids only. Why: a shell or an agent cannot see which screen
  a letter would mean.
- **Rearranging:** `w` spreads (12 panes: every tab used, the first three
  split left and right), `d` densifies (12 fill tabs 1-3), `a` puts them in
  name order in the places they had. Focus stays with the pane it was on.
  Why: panes arrive in whatever order they were attached; these put them
  back into an order with one key.
- **Click a pane to move to it.** Why: the mouse is already there.

## Footers

- **Every pane has a footer row: its letter, its name, and its status**
  (`LOCKED`, the mode on, `FULL`, unread chat, the last notice, a name being
  asked for). `f` turns footers off and on, for the terminal. Why: status
  in one steady place per pane, never things popping up in corners; the
  name is what `msg`, `attach` and every agent take.
- **Footers show only while more than one pane is attached.** Why: one pane
  goes back to plain attach, and a row taken and given back would make its
  program redraw twice for nothing.
- **With footers off, a footer shows only while it is needed** (swap's
  letters, a name being asked for, a notice), over the pane's last row.
- **No corner labels:** no name on attach, on arrival or on focus, no mode
  name, no tab count, no lock note. Why: they covered the programs and were
  not wanted; the footer holds all of it, and `backmux list` names
  everything running.

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
