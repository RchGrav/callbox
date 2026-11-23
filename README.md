# Callbox

**Run terminal apps your agents can see and control.**

[![CI](https://github.com/RchGrav/callbox/actions/workflows/ci.yml/badge.svg)](https://github.com/RchGrav/callbox/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/RchGrav/callbox?label=release)](https://github.com/RchGrav/callbox/releases/latest)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

Callbox gives a terminal program a name and a screen you can return to. An AI
agent can read that screen as text or a color image, locate the cursor, send
keys, and check what changed. You can attach to the same program to see and
test its work yourself.

It also runs background jobs and services, keeps their logs, and lets agents
in different harnesses exchange messages through shell commands. Callbox runs
ordinary programs on your machine, with no central daemon to start.

<img src="docs/images/vim-driven.png" alt="Callbox's color view of Vim, with a red cursor ring, cyan mouse marker, and coordinate rulers." width="900">

*A terminal screen from `callbox peek --image`. Red marks show the cursor;
cyan marks show the last mouse input. The footer gives their coordinates.*

## Install

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh
```

Release builds are available for Linux and macOS. On Windows, run Callbox
inside WSL. The installer checks the release checksum and prints the install
location; follow its PATH instructions if needed.

[Install options and checksums](docs/install.md) ·
[Build from source](docs/install.md#manual-build) ·
[Releases](https://github.com/RchGrav/callbox/releases)

## Try the screen and input loop

This example needs only a POSIX shell and `tr`. Start a detached terminal,
send a command, wait for its result, and read the screen:

```sh
callbox -dt --name demo -- sh
callbox send demo 'echo hello | tr a-z A-Z' --enter
callbox wait demo --match HELLO
callbox peek demo
```

The screen shows `HELLO`. `send` types into the running program; `wait`
checks its visible output. For a color image with cursor coordinates, use
`callbox peek demo --image demo.png`.

Join the program with `callbox attach demo`. Press **Ctrl-P, then Ctrl-Q**
to detach and leave it running. An agent can still inspect the screen and
send input while you are attached.

When finished:

```sh
callbox send demo exit --enter
callbox wait demo                 # prints the program's exit code: 0
callbox purge demo                # removes its record and log; frees the name
```

Use the same loop for an editor, a terminal UI, a REPL, or an agent harness.
The [TUI guide](src/cli/help/guides/tui.txt) adds keyboard navigation, mouse
input, resizing, and verification.

## Choose how a program runs

A **call** is a program managed by Callbox, with a name, an ID, and a log.
A call keeps its name after the program exits, until you purge its record.

| You want to… | Start it with… | What you get |
| --- | --- | --- |
| Use a program yourself | `callbox --name app -- PROGRAM` | An attached terminal with a chat window for messages. |
| Work with an agent in your terminal | `callbox -it --name lead -- AGENT` | An attached terminal; messages are typed into the agent's input. |
| Let an agent drive a terminal app | `callbox -dt --name app -- PROGRAM` | A detached terminal you can inspect, control, and attach to later. |
| Run a service or CI job | `callbox -dw --name job -- PROGRAM` | A background process with logs and a recorded exit code, without a terminal. |

Plain `-d` runs a background process without keeping an exit-code waiter.
Use `-dw` when a script needs to check the program's result. These processes
have logs but no screen for `peek` or `send`.

`callbox ps` shows running calls. `callbox logs NAME` reads output, and
`callbox end NAME` stops a program. `callbox NAME` reconnects to a running
call with a console, or starts an ended call again from its saved command.

## Find the next step

Start with `callbox help` for the short command overview. Each guide below
is available in the terminal and as a plain-text page in this repository.

| Goal | In the terminal | Read here |
| --- | --- | --- |
| Inspect and troubleshoot a terminal UI | `callbox help tui` | [Drive a TUI](src/cli/help/guides/tui.txt) |
| Debug a TUI with your coding agent, on a shared screen | `callbox help share` | [Work on the same app](src/cli/help/guides/share.txt) |
| Start another agent, give it work, and check progress | `callbox help agents` | [Work with another agent](src/cli/help/guides/agents.txt) |
| Exchange messages from inside or outside Callbox | `callbox help messaging` | [Send and receive messages](src/cli/help/guides/messaging.txt) |
| Run a background service or CI job and clean up | `callbox help service` | [Run in the background](src/cli/help/guides/service.txt) |

**Every command and flag:** `callbox help all`. For one command, use
`callbox COMMAND --help`. `callbox help launch` covers launch options;
`callbox help targets`, `scripting`, and `system` explain targeting,
exit codes, and root-managed calls.

[In-depth guide](docs/guide.md) · [Documentation index](docs/index.md) ·
[Contributing](CONTRIBUTING.md) · [Changelog](CHANGELOG.md)

## License

Apache-2.0. See [LICENSE](LICENSE). Vendored terminal and font components
have their own licenses under [third_party](third_party/).
