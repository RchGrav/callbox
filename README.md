# Backmux

**Run, inspect, and control named terminal programs.**

[![CI](https://github.com/RchGrav/backmux/actions/workflows/ci.yml/badge.svg)](https://github.com/RchGrav/backmux/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/RchGrav/backmux?label=release)](https://github.com/RchGrav/backmux/releases/latest)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

Backmux gives a terminal program a name and a screen you can return to. An AI
agent can read that screen as text or a color image, locate the cursor, send
keys, and check what changed. You can attach to the same program to see and
test its work yourself.

It also runs background jobs and services, keeps their logs, and lets agents
in different harnesses exchange messages through shell commands. Backmux runs
ordinary programs on your machine, with no central daemon to start.

<img src="docs/images/vim-driven.png" alt="Backmux's color view of Vim, with a red cursor ring, cyan mouse marker, and coordinate rulers." width="900">

*Illustrative screenshot from an earlier revision. Red marks show the
cursor; cyan marks show the last mouse input. The footer gives coordinates.*

## Install

```sh
curl -LsSf https://github.com/RchGrav/backmux/releases/latest/download/install.sh | sh
```

Release builds are available for Linux and macOS. On Windows, run Backmux
inside WSL. The installer checks the release checksum and prints the install
location; follow its PATH instructions if needed.

[Install options and checksums](docs/install.md) ·
[Build from source](docs/install.md#manual-build) ·
[Releases](https://github.com/RchGrav/backmux/releases)

## Try the screen and input loop

This example needs only a POSIX shell and `tr`. Start a detached terminal,
send a command, wait for its result, and read the screen:

```sh
backmux detach --name demo -- sh
backmux send demo 'echo hello | tr a-z A-Z' --enter
backmux wait demo --match HELLO
backmux peek demo
```

The screen shows `HELLO`. `send` types into the running program; `wait`
checks its visible output. For a color image with cursor coordinates, use
`backmux peek demo --image demo.png`.

Join the program with `backmux attach demo`. Press **Ctrl-Space, then q**
to detach and leave it running. An agent can still inspect the screen and
send input while you are attached.

While attached, **Ctrl-Space l** opens the process launcher, which brings
another running process in beside this one as a pane. **Ctrl-Space c** opens
the chat window, which lists your agents, and **Ctrl-Space ?** displays
command help. `backmux shell` opens your shell on its own screen with chat
up and a bar along the bottom that the mouse can do all of this from. Press `?` within a navigation or
layout mode for its controls. See [Interactive controls](docs/backmux.md).

When finished:

```sh
backmux send demo exit --enter
backmux wait demo                 # prints the program's exit code: 0
backmux purge demo                # removes its record and log; frees the name
```

Use the same loop for an editor, a terminal UI, a REPL, or an agent harness.
The [TUI guide](src/cli/help/guides/tui.txt) adds keyboard navigation, mouse
input, resizing, and verification.

## Choose how a program runs

A **managed process** has a name, a Backmux process ID, and a log.
A process keeps its name after the program exits, until you purge its record.

| You want to… | Start it with… | What you get |
| --- | --- | --- |
| Use a program or an agent yourself | `backmux call --name app PROGRAM` | An attached terminal; messages include the sender and reply command. |
| Let an agent drive a terminal app | `backmux detach --name app PROGRAM` | A detached terminal you can inspect, control, and attach to later. |
| Run a service or CI job | `backmux server -w --name job PROGRAM` | A background process with a log and a recorded exit code, without a terminal. |

A plain `server` launch retains its record and log but leaves no Backmux
supervisor. `-w` keeps an exit waiter. Servers have no screen or input
console, so use `logs`, `tail`, and `end` to inspect and manage them.

`backmux list` shows running processes and records with unknown status;
`list all` includes stopped records. `backmux info NAME` shows process
details, and `backmux end NAME` stops the process. `backmux recall NAME`
relaunches a stopped process using its recorded program, arguments, and
working directory. `set NAME args=...` and `set NAME working=...` change
the next launch. See `backmux help aliases` for Docker-style spellings.

## Find the next step

Start with `backmux help` for the short command overview. Each guide below
is available in the terminal and as a plain-text page in this repository.

| Goal | In the terminal | Read here |
| --- | --- | --- |
| Navigate panes, windows, and chat | `backmux help panes` | [Interactive controls](docs/backmux.md) |
| Inspect and troubleshoot a terminal UI | `backmux help tui` | [Drive a TUI](src/cli/help/guides/tui.txt) |
| Debug a TUI with your coding agent, on a shared screen | `backmux help share` | [Work on the same app](src/cli/help/guides/share.txt) |
| Start another agent, give it work, and check progress | `backmux help agents` | [Work with another agent](src/cli/help/guides/agents.txt) |
| Exchange messages from inside or outside Backmux | `backmux help messaging` | [Send and receive messages](src/cli/help/guides/messaging.txt) |
| Run a background service or CI job and clean up | `backmux help service` | [Run in the background](src/cli/help/guides/service.txt) |

**Every command and flag:** `backmux help all`. For one command, use
`backmux COMMAND --help`. `backmux help launch` covers launch options;
`backmux help targets`, `scripting`, and `system` explain targeting,
exit codes, and root-managed processes.

[In-depth guide](docs/guide.md) · [Documentation index](docs/index.md) ·
[Glossary](docs/glossary.md) ·
[Contributing](CONTRIBUTING.md) · [Changelog](CHANGELOG.md)

## License

Apache-2.0. See [LICENSE](LICENSE). Vendored terminal and font components
have their own licenses under [third_party](third_party/).
