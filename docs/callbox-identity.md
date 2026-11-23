# Callbox — identity

Status: normative. This document sits above
[docker-familiarity-contract.md](docker-familiarity-contract.md), which governs flag
behavior and table output only. `callbox help <verb>` is the reference for
every flag; this page orients.

## The name

A call box is where you place a call and walk away while it goes on, where
you reach someone else on the line, and a terminal you walk up to and
operate. Those are the tool's three jobs:

- keeping a program running after the shell that started it is gone, with a
  durable record, identity checks before any signal, and an exit code for CI
  (from a foreground, `-dw`, `--rm`, `--restart`, or TTY call; a plain `-d`
  call leaves no process behind to record one);
- messaging between agents, with the sender's name and how to reply;
- a headless screen for every TTY call that scripts and agents read and
  type into (`peek`, `send`, `wait`, `resize`, `msg`).

The telephone vocabulary follows from it: calls, redial, attach to pick the
line back up, caller ID.

More than nohup, less than systemd.

The unit callbox manages is a **call**: one held process group. You place a
call, list your calls, look at its screen, type into it, pick it back up,
end it, save the ones worth keeping. "Run" and "container" do not appear in
the vocabulary.

## The shape: one binary

One binary, small enough to code-review in an afternoon, statically linked
on Linux and dynamically on macOS. It holds the lean core plus the screen
layer: vendored libvterm (`third_party/libvterm`) and an embedded subset of
Unifont and Noto Emoji glyphs (`src/screen/font_data.h`, licenses in
`third_party/`). Each call's output is one plain log file, readable without
callbox's help; `callbox logs` prints it, and the screen of a TTY call is
what `peek` shows.

## The surface

`callbox` is already a verb. There is no `run` command: `callbox run` starts
a program named `run`.

```sh
# place a call (callbox help launch)
callbox <cmd> [args...]          # user mode: attached on a PTY, messages to chat
callbox -it <cmd> [args...]      # attached on a PTY, no chat window: messages typed in
callbox -i <cmd> [args...]       # attached, your input relayed on a pipe (no PTY)
callbox -d <cmd> [args...]       # detached: print the 64-hex call id
callbox -dw <cmd> [args...]      #  ...and keep a waiter that records the exit code
callbox -dt <cmd> [args...]      # detached on a PTY: a screen to drive
callbox -dt --size C R <cmd>     #  ...of that size
callbox shell [shell args...]    # your shell, with a chat window to your agents
callbox <id|name>                # redial: place a recorded call again

# drive a TTY call
callbox peek <target>            # the screen: text, --json, or --image PNG
callbox send <target> TEXT       # type and press keys (-k, --paste, --mouse)
callbox wait <target>            # until --idle, --match, --regex, or exit
callbox resize <target> C R      # change its terminal size (SIGWINCH)
callbox msg <target> TEXT        # message an agent; --received lists its history

# receive replies outside a call
callbox inbox [NAME]             # read retained inbox history
callbox inbox [NAME] --wait      # return one unread message, waiting if needed
callbox monitor [NAME]           # follow unread and new messages until stopped

# manage calls
callbox list                     # your ledger: your calls, live and past
callbox ps                       # the Docker view: running calls
callbox attach <target>          # pick the call back up (console is an alias)
callbox logs <target> [-f] [-n N]  # captured output
callbox tail <target>            # a running call's output from now on
callbox inspect <target>         # the record as JSON
callbox ports <target>           # listening sockets of its process group
callbox stats <target>           # CPU, memory, and process count
callbox end <target>...          # TERM, then KILL after 5 s (stop is an alias)
callbox kill <target>...         # KILL now
callbox save <target>            # protect from purge
callbox rename <target> <name>   # name a call; naming it also saves it
callbox purge [<target>]         # the removal verb (rm, prune, drop are aliases)
callbox config get|set|unset detach-size [C R]
```

A target is a call id, a leading id prefix, a call name, or a unique name prefix
(`callbox help targets`). Exit codes are listed in `callbox help scripting`.

What the short lines leave out:

- **Launch flags** come before the command: `--name`, `-e`/`--env-file`,
  `--rm`, `--restart` (not with `-t`), `--restart-delay`, `--detach-keys`,
  `--quiet`, and `--` before a command that looks like a flag or a verb.
- **Size of a detached TTY call:** `--size`, else the saved `detach-size`,
  else the invoking terminal's size, else 80x24. The launcher samples the
  terminal itself, because the broker runs with no terminal to ask.
- **Redial** reuses the call's id, name, and log, and honors the recorded
  session mode: a user-mode call (and a shell) comes back attached with
  chat, a `-i` recipe comes back on pipes, a detached recipe detaches, and a
  plain foreground recipe streams. A running call is not redialed.
- **`logs`** prints the raw captured bytes: plain text for a plain call, the
  raw terminal stream (escape sequences included) for a TTY call. For a TTY
  call's screen, use `peek`.
- **`wait`** with no condition waits for the call to end and prints its exit
  code, or `-1` for a plain `-d` call, which records none. It exits 7 on a
  timeout and 8 when the call ends before a screen condition holds.
- **`attach`** exits 0 when you detach (the call still runs), 8 when the
  call has ended (before or while you were on it), 3 when refused (attached
  elsewhere), and 10 when the call has no console - so a script that
  attached knows what it came back to.
- **`msg`** reaches a running terminal or pipe-input call. The receiver is told who sent it
  (the sender's `$CALLBOX_ID`, shown as that call's name) and how to reply -
  addressing, not proof, since anything running as the user could type the
  same text with `send`. Sending to a call prints the delivery ID on stdout,
  and `msg --received [ID]` is the receiving call's message history.
  Delivery to calls is capped at 20 messages a minute per receiver.
- **Outside senders** get an inbox name on their first `msg`. Replies to
  that name are stored, without a delivery ID on stdout. `inbox --wait`
  returns one unread message; `monitor` (also `inbox --monitor`) follows
  unread and new messages. `msg --wait-reply` waits for the first new inbox
  message from any sender, without request correlation. These commands
  share unread progress. See `callbox help messaging` for the workflow.
- **`inspect`** prints the stored record as a one-element JSON array, as
  `docker inspect` does: id, name, state, pids, times, argv, mode, and
  `log_path`. `state` is the evaluated state; a differing stored value is
  kept as `recorded_state`. A running call also gets `Stdio`, where fds
  0/1/2 point now.
- **`ports` and `stats`** read the process group from `/proc`, so your own
  calls need no root. They work on Linux only; on macOS `ports` lists
  nothing and `stats` reports zeros. `ports` lists listening TCP and bound
  UDP sockets.
- **`end` and `kill`** take several targets, and `--print` prints the
  equivalent `kill` command instead of sending a signal. Each target is
  acted on alone, as `docker stop a b c` does: one that does not resolve is
  reported and the rest are still ended; the exit code is the worst. (A
  mistyped name among several used to end none of them.)
- **Names** that are callbox verbs (`list`, `logs`, ...) are refused at
  `--name` and `rename`: the verb always wins at the top level, so such a
  call could never be redialed by name. Calls named so before keep their
  names. A name taken by another call or active inbox in the same store
  exits 11. Outside inboxes are addressed by their complete names.
- **`save`** has no inverse; `callbox purge <target> --force` removes a
  saved call.

### Purge

`callbox purge` is the one removal verb. With no target it sweeps ended
calls; `-a` includes stale ones. `-u` sweeps your personal store (the
default); `-s` sweeps the global store and, for a normal user, re-execs once
through `sudo` so `sudo` can prompt for the password. A target removes one
call. With a target, `--force` removes it even when saved or live (a live
call is ended first); in a sweep, `--force` only includes stale calls, like
`-a`, and never removes saved or live ones.

Purging a saved call without `--force` refuses safely (exit 2) and repeats
the command the user meant, ready to copy:

```text
$ callbox purge happy_tiger
callbox: 'happy_tiger' is saved — purging a saved call requires --force
  callbox purge happy_tiger --force
```

## ps speaks Docker; list speaks Callbox

There are two listings, and they answer different questions.

**`ps` is the Docker mirror.** Docker has no user/system split, so `ps`
takes no scope flags and has no USER column. It shows running calls across
the scopes you can see: a normal user sees their own calls in full and the
global (root-managed) calls redacted; root sees the global calls. Other
users' personal calls never appear in `ps`. `callbox ps -a` adds the ended
ones, as Docker's `-a` does. It renders Docker's columns minus IMAGE, which
callbox has no analogue for:

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

**`list` is Callbox's scoped ledger.** The ledger of past calls is the
product, so `list` defaults to your whole personal ledger, live *and* past,
and adds a USER column in Docker's IMAGE slot (second, after CALL ID):

```text
CALL ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

The scope flags belong entirely to `list`:

- **`callbox list`** (a normal user) — your calls, live and past.
- **`callbox list -a`/`--all`** — your ledger plus every global call,
  redacted.
- **`callbox list -s`/`--system`** — the global calls only, none of yours.
- **`callbox list -u`/`--user`** — your personal calls only, even under sudo
  (the invoking user's store, never root's).
- **`sudo callbox list`** (root) — the global calls only, authoritative.
  `sudo callbox list -a` also walks every user's store under
  `/home/*/.local/state/callbox` (`/Users/*/…` on macOS, where homes live),
  and the USER column names each owner.
- **`-l`/`--live`** narrows any of these to running calls.

A call a user is not entitled to read shows the literal `hidden` in its USER
and COMMAND cells, not `-`. `-` reads as *none*; `hidden` reads as *exists,
but not yours to see*, which is the truth.

The PORTS a user sees for a global call come from the public index. Only
root can observe a root-managed call's process group, so root refreshes each
live global call's observed ports (listening TCP and bound UDP only, never
outbound connections) into its public entry whenever root runs `list` or
`ps`. The column is therefore eventually consistent: a port that opened a
moment ago appears the next time root lists.

Docker flags callbox cannot honor (`-p`, `-P`, `-v`, `--privileged`,
`--cap-add`, `--cap-drop`) are rejected with exit 1 and a note, never
accepted and ignored, so a script never believes they took effect.

Docker parity otherwise applies at the **flag level** (`-d`, `-i`, `-t`,
`-e`, `--env-file`, `--name`, `--rm`, `--restart`, `--detach-keys`) and at
the **output level**: both tables render Docker's look — content-sized
columns, 12-hex short ids, `2 days ago`, `Exited (0) 2 days ago`, names
always present. Verbs are Callbox-native.

## Saved calls are recipes

A call record is a launch recipe: `save` protects it from purge, `rename`
names it and saves it (naming a call says you want to keep it), and
`callbox <name>` redials it.

## A person's shell is a call too

`callbox shell` is the one launch meant for a person rather than a script:
the shell runs on a PTY like any `-it` call, and its broker draws a chat
window over it for talking to the agents placed with `-dt` (Ctrl-P Ctrl-T
opens it). Nothing new carries the words — the window sends an ordinary
`callbox msg` from the shell's call and shows the ordinary `callbox msg`
replies — and the launch command, not the program's name, decides that
messages for this call are shown in chat instead of typed at a prompt.
[shell.md](shell.md) has the mechanism.

## Screens, not guesses

A TTY call's broker keeps an exact headless terminal, so nothing about the
screen is inferred: `callbox peek` reads it, `callbox inspect` reports where
fds 0/1/2 point, and redirected stdio stays purposeful, not a failure.

## The plumbing it rests on

Hash-derived 64-hex call ids with generated `adjective_noun` names; the raw
log file (a plain call's program writes it directly, a TTY call's broker
writes the PTY stream); the console broker, which keeps a headless screen
(see [screen.md](screen.md)); the call record store, where redial reuses a
call's id and appends to its log; end/kill escalation with target
revalidation. [SPEC.md](SPEC.md) has the on-disk invariants.
