# Changelog

## Unreleased

The first release of callbox: one static C11 binary for Linux and macOS that
keeps programs running, gives every terminal program a screen that scripts and
AI agents can read and drive, and lets agents message each other.

### Added

- Calls, in Docker's dialect: `callbox <cmd>` runs in the foreground,
  `-d` detaches (printing the 64-hex call id), `-it` attaches a terminal,
  `-dt` starts on a headless terminal, `-w` keeps a waiter that records the
  exit code, `--rm` removes the call when it ends, `--name`, `-e`,
  `--restart`, `--size COLS ROWS`. Why Docker's spelling: it is the process
  vocabulary most developers and agents already know.
- Every call is a durable record (`~/.local/state/callbox`; root's in
  `/var/lib/callbox`, `/var/db/callbox` on macOS) that survives the shell that
  started it: `list`/`ps`, `logs`, `inspect`, `end`/`stop`/`kill`, `save`,
  `rename`, `purge`, and redial (`callbox <name>` starts an ended call's recipe
  again). Why a record and no daemon: there is nothing to keep running or
  crash, and every command can rediscover every call from disk.
- Signals are sent only after re-validating the recorded process identity
  (boot id, process start time, executable, process group, session). Why:
  a reused PID must never be signaled.
- A headless terminal for every TTY call (vendored libvterm): `peek` reads
  the screen as text, JSON, or a PNG; `send` types text, keys, pastes, and
  mouse events encoded as a real xterm would; `wait` waits for text, a regex,
  quiet, or the call's end; `resize`. Why: agents and test scripts cannot see
  a terminal UI through a stream of output lines.
- `callbox msg`: message another call, typically an agent harness, with the
  sender verified by the receiving call's console. Delivered at once (an
  agent harness queues what is typed while it works); control characters are
  stripped (the text comes from a sender callbox does not trust).
  `msg --received` lists what callbox really delivered.
- `callbox shell`: your own shell on a PTY with a chat window to the agents
  you run, drawn over the shell and never into it.
- `callbox attach` (Ctrl-P Ctrl-Q detaches), nested attaches, and
  `callbox config` for a saved default detach size.
- Root and sudo calls go to the system store with a redacted public view for
  other users; a sudo launch of a program in the invoking user's home stays
  in that user's store, with every file the user's.
- Console transport over Unix sockets, or FIFOs where sandboxes refuse
  sockets, with nothing given up (peer verification, attach, root access).
  Calls work across PID namespaces.
- Installer with checksum verification; release builds for Linux (glibc and
  musl: amd64, arm64, armhf, mipsel, riscv64) and macOS.
