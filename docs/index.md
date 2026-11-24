# Backmux documentation

Backmux runs named programs that you can return to, inspect, and stop.
Terminal calls have a screen for people and agents to read and control;
background calls keep output in a log. Agents can exchange messages even
when their harnesses run outside Backmux.

## Start with a task

New to Backmux? [Install it](install.md), then run the
[README quickstart](../README.md#try-the-screen-and-input-loop).

| Goal | Terminal help | Guide |
| --- | --- | --- |
| Troubleshoot a terminal UI | `backmux help tui` | [Drive a TUI](../src/cli/help/guides/tui.txt) |
| Debug with your coding agent while using the app yourself | `backmux help share` | [Share a terminal app](../src/cli/help/guides/share.txt) |
| Launch another agent and check its work | `backmux help agents` | [Work with another agent](../src/cli/help/guides/agents.txt) |
| Send messages and receive replies across harnesses | `backmux help messaging` | [Exchange messages](../src/cli/help/guides/messaging.txt) |
| Run a service or CI job, check it, and clean up | `backmux help service` | [Run in the background](../src/cli/help/guides/service.txt) |

## Look up a command

`backmux help` is the short overview. `backmux help all` prints the complete
reference, including every command's detailed help and all launch flags.
Use `backmux COMMAND --help` when you know which command you need.

| Topic | Terminal help | Read here |
| --- | --- | --- |
| Launch modes and options | `backmux help launch` | [Launch reference](../src/cli/help/reference/launch.txt) |
| Names, IDs, and prefixes | `backmux help targets` | [Target reference](../src/cli/help/reference/targets.txt) |
| Exit codes, standard output, and scripts | `backmux help scripting` | [Scripting reference](../src/cli/help/reference/scripting.txt) |
| User and root-managed calls | `backmux help system` | [System reference](../src/cli/help/reference/system.txt) |

The [in-depth guide](guide.md) explains screenshots, keyboard and mouse
input, waiting, messaging, and CI, with worked examples. The help pages
under [src/cli/help](../src/cli/help) are compiled into the binary, so help
is available without a network connection or separate documentation files.

## Understand the design

- [Backmux identity](backmux-identity.md): the product model and command vocabulary.
- [Docker familiarity contract](docker-familiarity-contract.md): shared flag and table conventions.
- [Console](console.md): terminal and pipe-backed calls, attaching, and detaching.
- [Screens](screen.md): screen state, the control protocol, snapshots, and messages.
- [Shell and chat](shell.md): the attached shell and its chat window.
- [Launcher](launcher.md): sessions, process groups, and launch behavior.
- [Store](store.md): user-local and root-managed records and logs.
- [Process identity](identity.md): validation before sending signals.
- [Implementation invariants](SPEC.md): the on-disk contract.
- [CI parity](CI-PARITY.md): local checks and their CI counterparts.
- [Help maintenance](help.md): page organization, source evidence, and validation.
- [Release readiness](release-readiness.md): evidence and limits of the onboarding review.

## Find the implementation

Public headers live under `include/backmux/`; implementation is layered under
`src/`, with `src/main.c` as the entry point. In dependency order:

- `core/`: file I/O, JSON, SHA-256, IDs, names, and shared utilities
- `screen/`: the headless terminal, snapshots, and PNG rendering
- `platform/`: OS, account, path, and process information
- `store/`: durable call records and layout
- `term/`: terminal spawning and output handling
- `console/`: the per-call broker, control transport, attach, and chat display
- `access/`: invocation identity and user context
- `runtime/`: product commands and lifecycle behavior
- `cli/`: command definitions, parsing, help, and dispatch

`scripts/lint_layers.sh`, run by `make lint` and CI, checks that a layer
does not include headers from the layers below it in this list. See
[Contributing](../CONTRIBUTING.md) for build and test instructions.
