# Docker familiarity contract

[Docs index](index.md) | [Callbox identity](callbox-identity.md)

Status: normative, subordinate to [callbox-identity.md](callbox-identity.md).
Callbox's verbs are its own. It borrows Docker's **flag behavior** and **table
appearance**, because that muscle memory is worth keeping. When Callbox's
behavior disagrees with this document, Callbox is wrong. Tests assert the
shapes written here.

## Flags

These flags borrow their `docker run` spelling on Callbox's launch form
(`callbox [flags] <cmd> ...`). The shapes are familiar, not identical: Callbox
runs host processes, not containers, and has no daemon.

```text
-d, --detach        detached; stdout is exactly one line: the full 64-hex
                    call id
-i, --interactive   your input relayed to the program on a pipe (no PTY)
-t, --tty           allocate a PTY
-e, --env           set one variable: KEY=VALUE only (Docker's bare -e KEY
                    pass-through is not supported)
    --env-file      load KEY=VALUE lines
    --name          choose the call's name (otherwise one is generated)
    --rm            remove the call's record and log after it ends
    --restart       no | always | unless-stopped | on-failure[:N]; not
                    with -t
    --detach-keys   detach sequence for a PTY attach (default ctrl-p,ctrl-q)
```

Value flags also take `--flag=value`. Flags come before the command; every
word after the command is the command's own.

The command is resolved against the invoking environment's `PATH`.
`-e`/`--env-file` set the held process's environment only, never Callbox's
own. A root-managed call starts from an empty environment with a fixed
`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, then
applies `-e`/`--env-file`. Every call also gets `CALLBOX_ID`, `CALLBOX_PARENT`
when placed from inside another call, and `CALLBOX_NAME` when named.

A foreground launch prints nothing of its own, only the process output, and
exits with the held process's exit status, as `docker run` does.

Where callbox differs: with no session flags, a program runs attached on a
PTY, as `-it` (user mode, with a chat window when there is a terminal; see
callbox help launch). Docker's bare `docker run` attaches without a TTY;
callbox's bare form is how a person runs a terminal program, and scripts and
agents name what they want (`-d`, `-dt`, `-dw`, `-i`).

The keys are Docker's, in every mode you are on a call: Ctrl-C goes to the
program (Docker's sig-proxy; without a terminal, a SIGINT sent to callbox is
passed on), and `Ctrl-P Ctrl-Q` detaches, leaving it running. Docker's detach
keys need a TTY; callbox reads keys itself, so they also detach `-i` calls
and plain foreground calls (`-w`). Following a log (`logs -f`, `tail`,
`--tail`) is not being on a call: Ctrl-C stops following, as `docker logs -f`
does.

`-i` gives the program a pipe its broker holds, never your terminal, as
Docker's `-i` gives a container a pipe: piped input streams in and ends with
EOF, Ctrl-D ends typed input, and `callbox attach` reconnects input and
output (`-di` starts it in the background with its input open).

Flags that need a container are rejected with exit 1 and an explanation, so
a script never believes they took effect:

- `-p`, `-P`, `--publish`, `--publish-all`: Callbox observes real host ports
  instead; see `callbox ports` and the PORTS column.
- `-v`, `--volume`: host paths are just paths.

Flags that grant or remove privilege are rejected the same way, never
remapped: `--privileged`, `--cap-add`, `--cap-drop`. Callbox launches
ordinary host processes with the invoker's rights. A call goes to the
root-managed store when callbox runs as root or under sudo. A normal user's
`--system` on a launch is refused with exit 3.

Callbox-native launch flags:

```text
-w, --wait              keep a waiter as the call's parent, recording its exit
                        code for list and callbox wait (clusters: -dw)
    --restart-delay N   seconds between restart attempts; requires --restart
                        with a policy other than no
    --size COLS ROWS    the PTY's size (with -t)
    --quiet             no status notes on stderr
    -f, --tail CALL     follow a call's log (no PTY, no redial); a word that
                        names no call is a command to launch
```

A plain `-d` call leaves nothing behind: one fork, a new session, output
straight into its log, exec. Nothing records how it ended, so it lists as
`Exited 2 minutes ago` and `callbox wait` prints `-1`. `-w` keeps a waiter.
Foreground launches, `--rm`, and `--restart` always have one. A PTY call
records its exit through its broker.

## The call table

`ps` and `list` both render Docker's table look, but they are distinct verbs.

`ps` is the Docker view. It shows running calls only; `-a` adds ended ones.
A normal user sees their own calls plus the root-managed calls (redacted).
Root sees the root-managed calls. It has Docker's columns minus IMAGE,
because Callbox has no image analogue and does not fake one:

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

`list` is Callbox's scoped ledger. It adds a USER column in Docker's IMAGE
slot:

```text
CALL ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

- Both views list running calls first, then newest first.
- Columns are sized to their content, so a long value never shears a column.
- CREATED is humanized: `Less than a second ago`, `About a minute ago`,
  `2 days ago`. The table never shows a raw ISO timestamp. It shows `-` when
  the time is unknown.
- STATUS uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`. A
  death by signal N reads `Exited (128+N)`. Callbox adds:
  - `Exited 2 days ago` (no code): a plain `-d` call, which records none.
  - `Exited (?) 2 days ago`: a death its waiter did not witness.
  - `Failed 2 days ago`: the launch failed.
  - `Stale 2 days`: the boot id changed, so the call cannot be running.
  - `Unknown`: the record has no usable process identity.
  - a ` (saved)` suffix on protected calls.

  A redacted root-managed row shows `Up 2 minutes`, `Up`, `Exited (N)`, or
  `Exited`, without the exit age.
- COMMAND is the resolved command line, double-quoted and cut to 30
  characters plus `...`, so one long command never widens the table.
- NAMES is always filled for a user's calls, because every call gets a name.
- A call the caller may not read (a root-managed call in a normal user's
  view) shows `hidden` in its USER and COMMAND cells. `-` would read as
  *none*; `hidden` reads as *exists, not yours to see*.
- PORTS lists the listening TCP sockets, and the UDP sockets with no remote
  address, of a running call's process group, Docker-formatted and `, `-separated:
  `127.0.0.1:8080/tcp, [::]:80/tcp`. It is read from `/proc`, so it is
  always empty on macOS.

## Output discipline

stdout carries machine data only: IDs, tables, logs, JSON. Human notes,
hints, and errors go to stderr; `--quiet` suppresses normal human status.
Docker-familiar surfaces print nothing Docker would not print.
