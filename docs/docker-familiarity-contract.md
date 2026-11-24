# Docker familiarity contract

[Docs index](index.md) | [Backmux identity](backmux-identity.md)

Status: normative, subordinate to [backmux-identity.md](backmux-identity.md).
Backmux's verbs are its own. It borrows Docker's **flag behavior** and **table
appearance**, because that muscle memory is worth keeping. When Backmux's
behavior disagrees with this document, Backmux is wrong. Tests assert the
shapes written here.

## Flags

These flags borrow their `docker run` spelling on Backmux's launch form
(`backmux [flags] <cmd> ...`). The shapes are familiar, not identical: Backmux
runs host processes, not containers, and has no daemon.

```text
-d, --detach        detached; stdout is exactly one line: the full 64-hex
                    call id
-i, --interactive   your input relayed to the program on a pipe (no PTY)
-t, --tty           allocate a PTY
-e, --env           set one variable: KEY=VALUE only (Docker's bare -e KEY
                    pass-through is not supported)
    --env-file      load KEY=VALUE lines
    --name          choose the process's name (otherwise one is generated)
    --rm            remove the process's record and log after it ends
    --restart       no | always | unless-stopped | on-failure[:N]; not
                    with -t
    --detach-keys   detach sequence for a PTY attach (default ctrl-p,ctrl-q)
```

Value flags also take `--flag=value`. Flags come before the command; every
word after the command is the command's own.

The command is resolved against the invoking environment's `PATH`.
`-e`/`--env-file` set the held process's environment only, never Backmux's
own. A root-managed process starts from an empty environment with a fixed
`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, then
applies `-e`/`--env-file`. Every process also gets `BACKMUX_ID`, `BACKMUX_PARENT`
when placed from inside another process, and `BACKMUX_NAME` when named.

A foreground launch prints nothing of its own, only the process output, and
exits with the held process's exit status, as `docker run` does.

Where backmux differs: with no session flags, a program runs attached on a
PTY in user mode, with a chat window when there is a terminal (see backmux
help launch); `-it` is attached on a PTY without the window, so messages
for the program are typed in. Docker's bare `docker run` attaches without a TTY;
backmux's bare form is how a person runs a terminal program, and scripts and
agents name what they want (`-d`, `-dt`, `-dw`, `-i`).

The keys are Docker's, in every mode you are on a process: Ctrl-C goes to the
program (Docker's sig-proxy; without a terminal, a SIGINT sent to backmux is
passed on), and `Ctrl-P Ctrl-Q` detaches, leaving it running. Docker's detach
keys need a TTY; backmux reads keys itself, so they also detach `-i` processes
and plain foreground processes (`-w`). Following a log (`logs -f`, `tail`,
`--tail`) is not being on a process: Ctrl-C stops following, as `docker logs -f`
does.

`-i` gives the program a pipe its broker holds, never your terminal, as
Docker's `-i` gives a container a pipe: piped input streams in and ends with
EOF, Ctrl-D ends typed input, and `backmux attach` reconnects input and
output (`-di` starts it in the background with its input open).

Flags that need a container are rejected with exit 1 and an explanation, so
a script never believes they took effect:

- `-p`, `-P`, `--publish`, `--publish-all`: Backmux observes real host ports
  instead; see `backmux ports` and the PORTS column.
- `-v`, `--volume`: host paths are just paths.

Flags that grant or remove privilege are rejected the same way, never
remapped: `--privileged`, `--cap-add`, `--cap-drop`. Backmux launches
ordinary host processes with the invoker's rights. A process goes to the
root-managed store when backmux runs as root or under sudo. A normal user's
`--system` on a launch is refused with exit 3.

Backmux-native launch flags:

```text
-w, --wait              keep a waiter as the process's parent, recording its exit
                        code for list and backmux wait (clusters: -dw)
    --restart-delay N   seconds between restart attempts; requires --restart.
                        Without it: back off 1 s doubling to 30 s, reset
                        after a 10 s run (Docker backs off too), so a
                        program that fails at once never spins
                        with a policy other than no
    --size COLS ROWS    the PTY's size (with -t)
    --quiet             no status notes on stderr
    -f, --tail CALL     follow a process's log (no PTY, no redial); a word that
                        names no process is a command to launch
```

A plain `-d` process leaves nothing behind: one fork, a new session, output
straight into its log, exec. Nothing records how it ended, so it lists as
`Exited 2 minutes ago` and `backmux wait` prints `-1`. `-w` keeps a waiter.
Foreground launches, `--rm`, and `--restart` always have one. A PTY process
records its exit through its broker.

## The process table

`ps` and `list` both render Docker's table look, but they are distinct verbs.

`ps` is the Docker view. It shows running processes only; `-a` adds ended ones.
A normal user sees their own processes plus the root-managed processes (redacted).
Root sees the root-managed processes. It has Docker's columns minus IMAGE,
because Backmux has no image analogue and does not fake one:

```text
PROCESS ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

`list` is Backmux's scoped ledger. It adds a USER column in Docker's IMAGE
slot:

```text
PROCESS ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

- Both views list running processes first, then newest first.
- Columns are sized to their content, so a long value never shears a column.
- CREATED is humanized: `Less than a second ago`, `About a minute ago`,
  `2 days ago`. The table never shows a raw ISO timestamp. It shows `-` when
  the time is unknown.
- STATUS uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`. A
  death by signal N reads `Exited (128+N)`. Backmux adds:
  - `Exited 2 days ago` (no code): a plain `-d` process, which records none.
  - `Exited (?) 2 days ago`: a death its waiter did not witness.
  - `Failed 2 days ago`: the launch failed.
  - `Stale 2 days`: the boot id changed, so the process cannot be running.
  - `Unknown`: the record has no usable process identity.
  - a ` (saved)` suffix on protected processes.

  A redacted root-managed row shows `Up 2 minutes`, `Up`, `Exited (N)`, or
  `Exited`, without the exit age.
- COMMAND is the resolved command line, double-quoted and cut to 30
  characters plus `...`, so one long command never widens the table.
- NAMES is always filled for a user's processes, because every process gets a name.
- A process the caller may not read (a root-managed process in a normal user's
  view) shows `hidden` in its USER and COMMAND cells. `-` would read as
  *none*; `hidden` reads as *exists, not yours to see*.
- PORTS lists the listening TCP sockets, and the UDP sockets with no remote
  address, of a running process's process group, Docker-formatted and `, `-separated:
  `127.0.0.1:8080/tcp, [::]:80/tcp`. It is read from `/proc`, so it is
  always empty on macOS.

## Output discipline

stdout carries machine data only: IDs, tables, logs, JSON. Human notes,
hints, and errors go to stderr; `--quiet` suppresses normal human status.
Docker-familiar surfaces print nothing Docker would not print.
