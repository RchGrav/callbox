# Backmux — identity

Status: normative. This document sits above
[docker-familiarity-contract.md](docker-familiarity-contract.md), which governs flag
behavior and table output only. `backmux help <verb>` is the reference for
every flag; this page orients.

## The name

A process box is where you place a process and walk away while it goes on, where
you reach someone else on the line, and a terminal you walk up to and
operate. Those are the tool's three jobs:

- keeping a program running after the shell that started it is gone, with a
  durable record, identity checks before any signal, and an exit code for CI
  (from a foreground, `-dw`, `--rm`, `--restart`, or TTY process; a plain `-d`
  call leaves no process behind to record one);
- messaging between agents, with the sender's name and how to reply;
- a headless screen for every TTY process that scripts and agents read and
  type into (`peek`, `send`, `wait`, `resize`, `msg`).

The telephone vocabulary follows from it: processes, redial, attach to pick the
line back up, caller ID.

More than nohup, less than systemd.

The unit backmux manages is a **process**: one held process group. You place a
process, list your processes, look at its screen, type into it, pick it back up,
end it, save the ones worth keeping. "Run" and "container" do not appear in
the vocabulary.

## The shape: one binary

One binary, small enough to code-review in an afternoon, statically linked
on Linux and dynamically on macOS. It holds the lean core plus the screen
layer: vendored libvterm (`third_party/libvterm`) and an embedded subset of
Unifont and Noto Emoji glyphs (`src/screen/font_data.h`, licenses in
`third_party/`). Each process's output is one plain log file, readable without
backmux's help; `backmux logs` prints it, and the screen of a TTY process is
what `peek` shows.

## The surface

`backmux` is already a verb. There is no `run` command: `backmux run` starts
a program named `run`.

```sh
# place a process (backmux help launch)
backmux <cmd> [args...]          # user mode: attached on a PTY, messages to chat
backmux -it <cmd> [args...]      # attached on a PTY, no chat window: messages typed in
backmux -i <cmd> [args...]       # attached, your input relayed on a pipe (no PTY)
backmux -d <cmd> [args...]       # detached: print the 64-hex process id
backmux -dw <cmd> [args...]      #  ...and keep a waiter that records the exit code
backmux -dt <cmd> [args...]      # detached on a PTY: a screen to drive
backmux -dt --size C R <cmd>     #  ...of that size
backmux shell [shell args...]    # your shell, with a chat window to your agents
backmux <id|name>                # redial: place a recorded process again

# drive a TTY process
backmux peek <target>            # the screen: text, --json, or --image PNG
backmux send <target> TEXT       # type and press keys (-k, --paste, --mouse)
backmux wait <target>            # until --idle, --match, --regex, or exit
backmux resize <target> C R      # change its terminal size (SIGWINCH)
backmux msg <target> TEXT        # message an agent; --received lists its history

# receive replies outside a process
backmux inbox [NAME]             # read retained inbox history
backmux inbox [NAME] --wait      # return one unread message, waiting if needed
backmux monitor [NAME]           # follow unread and new messages until stopped

# manage processes
backmux list                     # your ledger: your processes, live and past
backmux ps                       # the Docker view: running processes
backmux attach <target>          # pick the process back up (console is an alias)
backmux logs <target> [-f] [-n N]  # captured output
backmux tail <target>            # a running process's output from now on
backmux inspect <target>         # the record as JSON
backmux ports <target>           # listening sockets of its process group
backmux stats <target>           # CPU, memory, and process count
backmux end <target>...          # TERM, then KILL after 5 s (stop is an alias)
backmux kill <target>...         # KILL now
backmux save <target>            # protect from purge
backmux rename <target> <name>   # name a process; naming it also saves it
backmux purge [<target>]         # the removal verb (rm, prune, drop are aliases)
backmux config get|set|unset detach-size [C R]
```

A target is a process id, a leading id prefix, a process name, or a unique name prefix
(`backmux help targets`). Exit codes are listed in `backmux help scripting`.

What the short lines leave out:

- **Launch flags** come before the command: `--name`, `-e`/`--env-file`,
  `--rm`, `--restart` (not with `-t`), `--restart-delay`, `--detach-keys`,
  `--quiet`, and `--` before a command that looks like a flag or a verb.
- **Size of a detached TTY process:** `--size`, else the saved `detach-size`,
  else the invoking terminal's size, else 80x24. The launcher samples the
  terminal itself, because the broker runs with no terminal to ask.
- **Redial** reuses the process's id, name, and log, and honors the recorded
  session mode: a user-mode process (and a shell) comes back attached with
  chat, a `-i` recipe comes back on pipes, a detached recipe detaches, and a
  plain foreground recipe streams. A running process is not redialed.
- **`logs`** prints the raw captured bytes: plain text for a plain process, the
  raw terminal stream (escape sequences included) for a TTY process. For a TTY
  process's screen, use `peek`.
- **`wait`** with no condition waits for the process to end and prints its exit
  code, or `-1` for a plain `-d` process, which records none. It exits 7 on a
  timeout and 8 when the process ends before a screen condition holds.
- **`attach`** exits 0 when you detach (the process still runs), 8 when the
  call has ended (before or while you were on it), 3 when refused (attached
  elsewhere), and 10 when the process has no console - so a script that
  attached knows what it came back to.
- **`msg`** reaches a running terminal or pipe-input process. The receiver is told who sent it
  (the sender's `$BACKMUX_ID`, shown as that process's name) and how to reply -
  addressing, not proof, since anything running as the user could type the
  same text with `send`. Sending to a process prints the delivery ID on stdout,
  and `msg --received [ID]` is the receiving process's message history.
  Delivery to processes is capped at 20 messages a minute per receiver.
- **Outside senders** get an inbox name on their first `msg`. Replies to
  that name are stored, without a delivery ID on stdout. `inbox --wait`
  returns one unread message; `monitor` (also `inbox --monitor`) follows
  unread and new messages. `msg --wait-reply` waits for the first new inbox
  message from any sender, without request correlation. These commands
  share unread progress. See `backmux help messaging` for the workflow.
- **`inspect`** prints the stored record as a one-element JSON array, as
  `docker inspect` does: id, name, state, pids, times, argv, mode, and
  `log_path`. `state` is the evaluated state; a differing stored value is
  kept as `recorded_state`. A running process also gets `Stdio`, where fds
  0/1/2 point now.
- **`ports` and `stats`** read the process group from `/proc`, so your own
  processes need no root. They work on Linux only; on macOS `ports` lists
  nothing and `stats` reports zeros. `ports` lists listening TCP and bound
  UDP sockets.
- **`end` and `kill`** take several targets, and `--print` prints the
  equivalent `kill` command instead of sending a signal. Each target is
  acted on alone, as `docker stop a b c` does: one that does not resolve is
  reported and the rest are still ended; the exit code is the worst. (A
  mistyped name among several used to end none of them.)
- **Names** that are backmux verbs (`list`, `logs`, ...) are refused at
  `--name` and `rename`: the verb always wins at the top level, so such a
  call could never be redialed by name. Processes named so before keep their
  names. A name taken by another process or active inbox in the same store
  exits 11. Outside inboxes are addressed by their complete names.
- **`save`** has no inverse; `backmux purge <target> --force` removes a
  saved process.

### Purge

`backmux purge` is the one removal verb. With no target it sweeps ended
processes; `-a` includes stale ones. `-u` sweeps your personal store (the
default); `-s` sweeps the global store and, for a normal user, re-execs once
through `sudo` so `sudo` can prompt for the password. A target removes one
process. With a target, `--force` removes it even when saved or live (a live
call is ended first); in a sweep, `--force` only includes stale processes, like
`-a`, and never removes saved or live ones.

Purging a saved process without `--force` refuses safely (exit 2) and repeats
the command the user meant, ready to copy:

```text
$ backmux purge happy_tiger
backmux: 'happy_tiger' is saved — purging a saved process requires --force
  backmux purge happy_tiger --force
```

## ps speaks Docker; list speaks Backmux

There are two listings, and they answer different questions.

**`ps` is the Docker mirror.** Docker has no user/system split, so `ps`
takes no scope flags and has no USER column. It shows running processes across
the scopes you can see: a normal user sees their own processes in full and the
global (root-managed) processes redacted; root sees the global processes. Other
users' personal processes never appear in `ps`. `backmux ps -a` adds the ended
ones, as Docker's `-a` does. It renders Docker's columns minus IMAGE, which
backmux has no analogue for:

```text
PROCESS ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

**`list` is Backmux's scoped ledger.** The ledger of past processes is the
product, so `list` defaults to your whole personal ledger, live *and* past,
and adds a USER column in Docker's IMAGE slot (second, after PROCESS ID):

```text
PROCESS ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

The scope flags belong entirely to `list`:

- **`backmux list`** (a normal user) — your processes, live and past.
- **`backmux list -a`/`--all`** — your ledger plus every global process,
  redacted.
- **`backmux list -s`/`--system`** — the global processes only, none of yours.
- **`backmux list -u`/`--user`** — your personal processes only, even under sudo
  (the invoking user's store, never root's).
- **`sudo backmux list`** (root) — the global processes only, authoritative.
  `sudo backmux list -a` also walks every user's store under
  `/home/*/.local/state/backmux` (`/Users/*/…` on macOS, where homes live),
  and the USER column names each owner.
- **`-l`/`--live`** narrows any of these to running processes.

A process a user is not entitled to read shows the literal `hidden` in its USER
and COMMAND cells, not `-`. `-` reads as *none*; `hidden` reads as *exists,
but not yours to see*, which is the truth.

The PORTS a user sees for a global process come from the public index. Only
root can observe a root-managed process's process group, so root refreshes each
live global process's observed ports (listening TCP and bound UDP only, never
outbound connections) into its public entry whenever root runs `list` or
`ps`. The column is therefore eventually consistent: a port that opened a
moment ago appears the next time root lists.

Docker flags backmux cannot honor (`-p`, `-P`, `-v`, `--privileged`,
`--cap-add`, `--cap-drop`) are rejected with exit 1 and a note, never
accepted and ignored, so a script never believes they took effect.

Docker parity otherwise applies at the **flag level** (`-d`, `-i`, `-t`,
`-e`, `--env-file`, `--name`, `--rm`, `--restart`, `--detach-keys`) and at
the **output level**: both tables render Docker's look — content-sized
columns, 12-hex short ids, `2 days ago`, `Exited (0) 2 days ago`, names
always present. Verbs are Backmux-native.

## Saved processes are recipes

A process record is a launch recipe: `save` protects it from purge, `rename`
names it and saves it (naming a process says you want to keep it), and
`backmux <name>` redials it.

## A person's shell is a process too

`backmux shell` is the one launch meant for a person rather than a script:
the shell runs on a PTY like any `-it` process, and its broker draws a chat
window over it for talking to the agents placed with `-dt` (Ctrl-P Ctrl-T
opens it). Nothing new carries the words — the window sends an ordinary
`backmux msg` from the shell's process and shows the ordinary `backmux msg`
replies — and the launch command, not the program's name, decides that
messages for this process are shown in chat instead of typed at a prompt.
[shell.md](shell.md) has the mechanism.

## Screens, not guesses

A TTY process's broker keeps an exact headless terminal, so nothing about the
screen is inferred: `backmux peek` reads it, `backmux inspect` reports where
fds 0/1/2 point, and redirected stdio stays purposeful, not a failure.

## The plumbing it rests on

Hash-derived 64-hex process ids with generated `adjective_noun` names; the raw
log file (a plain process's program writes it directly, a TTY process's broker
writes the PTY stream); the console broker, which keeps a headless screen
(see [screen.md](screen.md)); the process record store, where redial reuses a
process's id and appends to its log; end/kill escalation with target
revalidation. [SPEC.md](SPEC.md) has the on-disk invariants.
