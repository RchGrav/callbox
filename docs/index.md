# Callbox documentation

Callbox is a daemonless host-process manager and a terminal for scripts and
agents. `nohup` hangs up gracefully; Callbox keeps the line open. It places
ordinary commands as durable **calls** — recorded, logged, listable,
reattachable — and later ends, redials, saves, or purges them. TTY calls
carry a live headless screen that `callbox peek`, `send`, `wait`, `resize`, and
`msg` read and drive.

## The contract

- [Callbox — identity](callbox-identity.md): what the tool is,
  the full verb surface, and what deliberately does not exist.
- [Docker familiarity contract](docker-familiarity-contract.md): the flags and
  table appearance borrowed from Docker, exactly.

## Start here

| Need | Read |
| --- | --- |
| Install Callbox | [Installing Callbox](install.md) |
| Learn the workflow | [Repository README](../README.md) |
| Understand how signals stay safe | [Identity](identity.md) |

## Technical references

- [Launcher](launcher.md): fork/session/process-group launch model.
- [Store](store.md): user-local and root-managed state layout.
- [Identity](identity.md): validation before signaling.
- [Console](console.md): PTY-backed calls, attach/detach.
- [Screens](screen.md): the headless terminal, the control protocol, HSNAP1, messaging.
- [callbox shell](shell.md): a person's shell with the chat window; the composited display.
- [Implementation invariants](SPEC.md): the on-disk contract everything relies on.
- [CI parity](CI-PARITY.md): keeping local checks equal to CI.

## Source map

Public headers live under `include/callbox/`; implementation is layered under
`src/`, lowest first, with `src/main.c` as the entry point.
`scripts/lint_layers.sh` (run by `make lint` and CI) keeps each layer from
including the headers of layers that come after it in this list, so
dependencies point one way only.

- `core/`: file I/O, JSON, SHA-256, id and name validation, small utilities
- `screen/`: the headless terminal, snapshots, and PNG rendering
- `platform/`: OS facts (boot ID), paths, user lookup, process facts
- `store/`: durable call records and layout
- `term/`: PTY spawn and the PTY-master read pump
- `console/`: the per-call broker, its control protocol over a Unix socket or
  FIFOs, attach, and the `callbox shell` chat window and compositor
- `access/`: invocation context (euid, invoking user)
- `runtime/`: product commands and lifecycle behavior
- `cli/`: the command table, flag parsing, help text, and dispatch
