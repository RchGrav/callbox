# Backmux product model

[Docs index](index.md) | [Glossary](glossary.md) | [Interactive controls](backmux.md)

Backmux manages named process groups on the host. It records how each program
was launched, captures its output, and checks process identity before sending
signals. Terminal processes also have a screen that people and agents can
inspect and control. Messaging connects managed processes and named inboxes.

The command definitions in `src/cli/main.c` and `src/cli/specs.c`, the runtime
handlers, and the console input handlers define the implemented behavior.
This page summarizes that behavior; `backmux help COMMAND` describes each
command's syntax and options.

## Objects and names

| Term | Meaning |
| --- | --- |
| Managed process | A program and its managed process group, identified by a Backmux process ID and name. Its record can remain after the program exits. |
| Process record | Stored launch settings, identity information, and recorded status. |
| Saved profile | A process record protected from ordinary purge operations by `save` or `rename`. |
| Terminal process | A managed process running on a pseudoterminal (PTY), with a console broker and a headless terminal screen. |
| Pane | An attached view of a terminal process within the compositor. |
| Window | A numbered group of up to four panes. One terminal can contain nine windows. |
| Chat window | The person's messaging interface, drawn over the attached panes. |
| Inbox | A named message destination for a sender outside a managed process, including the person's chat identity. |

Use **PID** for an operating-system process identifier and **Backmux process
ID** for the 64-character hexadecimal record identifier. A name, full ID,
or supported unique prefix selects a process; see `backmux help targets`.
Names remain reserved until the record is removed. Renaming changes the name
without changing the Backmux process ID.

## Launch a program

| Command | Terminal and attachment | Message delivery |
| --- | --- | --- |
| `backmux call PROGRAM [ARGS...]` | PTY, attached | Typed into the program behind a sender and reply-command header. |
| `backmux detach PROGRAM [ARGS...]` | PTY, detached | As for `call`. |
| `backmux server PROGRAM [ARGS...]` | Background process with a log, without a console | No interactive message input. |
| `backmux server -w PROGRAM [ARGS...]` | As above, with an exit waiter | The waiter records the program's exit status. |
| `backmux shell [SHELL [ARGS...]]` | PTY, attached | As for `call`; chat has a separate inbox identity. |

All launch commands accept `--name NAME` before the program. Arguments after
the program belong to that program. `call`, `agent`, `server`, and `detach`
select the launch mode; do not combine them with conflicting session flags.

The bare form `backmux PROGRAM [ARGS...]` defaults to an attached PTY.
`backmux run` accepts the same launch syntax. The supported flag forms
(`-it`, `-dt`, `-d`, `-dw`, and pipe-input `-i`) remain available;
`backmux help aliases` and `backmux help launch` describe them.

A plain `server` or `-d` launch leaves no Backmux supervisor to record its
exit code. `server -w` or `-dw` keeps a waiter. A console broker or restart
supervisor records the exit for the modes it manages. `wait NAME` prints
the recorded exit code, or `-1` when the process ended without one.

## Inspect and control a process

| Task | Commands |
| --- | --- |
| List processes | `list` shows running and unknown-state processes in the selected scope; `list all` includes stopped records. |
| Inspect a record | `info NAME` shows readable details and environment variable names; `inspect NAME` prints the JSON record, including recorded values. |
| Read output | `logs NAME` reads captured output; `tail NAME` follows new output from a running process. |
| Inspect a screen | `peek NAME`, `peek NAME --json`, `peek NAME --image FILE` |
| Provide input | `send NAME` supports text, keys, paste, and mouse events; `msg NAME TEXT` delivers a message. |
| Wait for a result | `wait NAME` waits for exit; screen conditions include `--match`, `--regex`, and `--idle`. |
| Resize a terminal | `resize NAME COLS ROWS` |
| Attach interactively | `attach NAME`; the default detach sequence is Ctrl-Space, then q. |
| Stop a process | `end NAME` sends SIGTERM, waits up to five seconds, then revalidates and sends SIGKILL if needed. `kill NAME` sends SIGKILL immediately. |
| Relaunch a stopped process | `recall NAME` reuses its record, name, launch settings, and log. |

Only one interactive client attaches to a process at a time. A new attach
replaces the previous client without stopping the program. Independent
`peek`, `send`, and other control requests remain available while attached.
A pipe-input process also has a broker and can be attached; a plain server
has logs but no screen or input console.

`ports` and `stats` inspect process groups through Linux `/proc`. On macOS,
`ports` has no socket results and `stats` reports zero usage values.

## Process settings and saved profiles

`set` and `unset` edit the record of a running or stopped process:

| Setting | Effect | `unset` result |
| --- | --- | --- |
| `working=DIR` | Use this working directory on the next launch. | Use the working directory of the next recall invocation. |
| `program=PROGRAM` | Change the executable for the next launch. | Rejected; a program is required. |
| `args=ARGS` or `args -- ARG...` | Change arguments for the next launch. The `args=...` form splits on spaces and tabs; `args --` preserves shell argument boundaries. | Launch without arguments. |
| `name=NAME` | Rename the record immediately and save it. The running program's existing environment does not change. | Rejected; the record retains a name. |

`save NAME` protects a record from ordinary cleanup. `rename NAME NEWNAME`
also saves it. `delete NAME` removes a stopped record, including a saved
profile; it refuses a process confirmed to be running. `purge NAME --force` can stop and
remove a running process or remove a saved record.

`purge` without a target removes eligible stopped, unsaved records.
`purge all` also stops unsaved running processes before removing their
records. Saved profiles are excluded from both sweeps. These are different
operations: positional `all` is not the same as the `-a` scope/state option.
See `backmux help purge` for scope flags and stale or unknown states.

## List scope

`list` includes a USER column and has explicit store selection. A normal
user's default scope is their own store; root's default is the system store.
`-u` selects the user's store, `-s` selects the system store, and `-a` selects
both (root also enumerates other users' stores). Positional `all` includes
stopped records, independently of store selection. A name filter includes
matching stopped records unless `--live` is given.

`ps` is the Docker-style view: a normal user sees their own processes and
redacted system entries; root sees the system store. `ps -a` includes stopped
records. It omits the USER column and does not accept `list`'s scope flags.

Root-managed records expose only their public projection to normal users.
Private command and owner fields appear as `hidden`. Public ports reflect
root's last refresh during `list` or `ps`, so they can lag behind the process.
See [Store](store.md) and [Docker-style syntax](docker-familiarity-contract.md).

## Chat, panes, and help

Interactive terminal controls use Ctrl-Space followed by a command key.
Ctrl-Space l opens the process launcher; Ctrl-Space p enters Pane Navigation.
Ctrl-Space c opens the person's chat window over the attached process; it
lists agent harnesses only. `backmux shell` starts with chat open and the bar
along the bottom, which the mouse can reach every control from.
Chat uses its own inbox name; it is not the shell process's message receiver.

Ctrl-Space ? shows the command help. Within Pane Navigation, Window
Navigation, Pane Resizing, or Pane Placement, `?` shows the relevant controls.
The [interactive reference](backmux.md) lists the implemented shortcuts.
The [shell and chat guide](shell.md) covers messaging and display behavior.

At the CLI, `backmux help` groups commands by task, `help COMMAND` gives
usage details, task guides explain workflows, and `help all` assembles the
complete reference. `backmux manual` prints the complete reference followed by the worked
examples.
