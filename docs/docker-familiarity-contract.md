# Docker familiarity contract

[Docs index](index.md) | [Backmux identity](backmux-identity.md)

This reference describes the Docker-style launch flags, aliases, and table
format implemented by Backmux. The parser and runtime are the authority for
current behavior. See [Product model](backmux-identity.md) for native commands
and [Glossary](glossary.md) for terminology.

## Flags

These flags borrow their `docker run` spelling on Backmux's launch form
(`backmux [flags] <cmd> ...`). The shapes are familiar, not identical: Backmux
runs host processes, not containers, and has no central daemon.

```text
-d, --detach        detached; stdout is exactly one line: the full 64-hex
                    Backmux process ID
-i, --interactive   your input relayed to the program on a pipe (no PTY)
-t, --tty           allocate a PTY
-e, --env           set one variable: KEY=VALUE only (Docker's bare -e KEY
                    pass-through is not supported)
    --env-file      load KEY=VALUE lines
    --name          choose the process's name (otherwise one is generated)
    --rm            remove the process's record and log after it ends
    --restart       no | always | unless-stopped | on-failure[:N]; not
                    with -t
    --detach-keys   detach sequence for a PTY attach (default ctrl-space,q)
```

Value flags also take `--flag=value`. Flags come before the command; every
word after the command is the command's own.

The command is resolved against the invoking environment's `PATH`.
`-e`/`--env-file` set the managed process's environment only, never Backmux's
own. A process launched into the system store starts from an empty environment with a fixed
`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, then
applies `-e`/`--env-file`. Every process also gets `BACKMUX_ID`, `BACKMUX_PARENT`
when placed from inside another process, and `BACKMUX_NAME` when named.

A foreground launch prints nothing of its own, only the process output, and
exits with the managed process's exit status, as `docker run` does.

With no session flags, the bare launch form runs on an attached PTY, as
`backmux call` does. `backmux run` accepts the same launch form. The
`agent` setting controls message headers independently of attachment mode.
Chat and pane controls belong to the interactive attach client.

Ctrl-C is forwarded while attached to a process. Ctrl-Space q detaches the
active pane without stopping its program. The detach sequence also works
for pipe-input attaches and plain foreground launches. Ctrl-C in a
standalone log follower (`logs -f`, `tail`, or `--tail NAME`) stops following.

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
ordinary host processes with the invoker's rights. Root and sudo launches normally use the system store. A program or
existing path argument under the invoking user's home selects that user's
store instead; see `backmux help system`. A normal user's `--system`
launch is rejected with exit 3.

Backmux-native launch flags:

```text
-w, --wait              keep a waiter as the process's parent, recording its exit
                        code for list and backmux wait (clusters: -dw)
    --restart-delay N   delay in seconds between restart attempts; requires
                        an active --restart policy. Default: 1 s, doubling
                        to 30 s, reset after a run lasting at least 10 s.
    --size COLS ROWS    the PTY's size (with -t)
    --quiet             no status notes on stderr
-f, --tail NAME         follow a process's log (no PTY, no relaunch); a word that
                        names no process is a command to launch
```

A plain `-d` process retains a record and log but leaves no Backmux
supervisor: its output goes directly to the log. No helper records its exit, so it lists as
`Exited 2 minutes ago` and `backmux wait` prints `-1`. `-w` keeps a waiter.
Plain foreground launches and `--rm` use a waiter; an active restart
policy uses a restart supervisor. Console-backed processes record exit
through their broker.

## The process table

`ps` and `list` both render Docker's table look, but they are distinct verbs.

`ps` is the Docker-style view. It shows running and unknown-state records;
`-a` includes stopped records.
A normal user sees their own processes plus the root-managed processes (redacted).
Root sees the root-managed processes. It has Docker's columns minus IMAGE,
because Backmux manages host programs rather than container images:

```text
PROCESS ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

`list` selects stores independently of its running/stopped filter. By
default it shows running and unknown-state processes; `list all` includes
stopped records. It adds a USER column:

```text
PROCESS ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

- Both views list running processes first, then newest first.
- Columns are sized to their displayed content.
- CREATED is humanized: `Less than a second ago`, `About a minute ago`,
  `2 days ago`. Public entries with no usable creation timestamp show `-`.
- STATUS uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`. A
  death by signal N reads `Exited (128+N)`. Backmux adds:
  - `Exited 2 days ago` (no code): a plain `-d` process, which records none.
  - `Exited (?) 2 days ago`: a death its waiter did not witness.
  - `Failed 2 days ago`: the launch failed.
  - `Stale 2 days`: the boot ID or recorded process identity no longer matches.
  - `Unknown`: available evidence cannot establish the process state.
  - a ` (saved)` suffix on protected processes.

  If no end timestamp is recorded, the displayed exit age uses the launch
  time; it is not an observed exit time.

  A redacted root-managed row shows `Up 2 minutes`, `Up`, `Exited (N)`, or
  `Exited`, without the exit age.
- COMMAND is the resolved command line, double-quoted and cut to 30
  characters plus `...`.
- NAMES is always filled for a user's processes, because every process gets a name.
- A process the caller may not read (a root-managed process in a normal user's
  view) shows `hidden` in its USER and COMMAND cells.
- PORTS lists the listening TCP sockets, and the UDP sockets with no remote
  address, of a running process's process group, Docker-formatted and `, `-separated:
  `127.0.0.1:8080/tcp, [::]:80/tcp`. It is read from `/proc`, so it is
  always empty on macOS.

## Output discipline

stdout carries command results, including IDs, tables, logs, JSON, readable
process details, and help. Status notes, hints, and errors go to stderr;
`--quiet` suppresses ordinary status notes.
