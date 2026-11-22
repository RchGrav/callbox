# Changelog

## Unreleased

The first release of callbox: one static C11 binary for Linux and macOS that
keeps programs running, gives every terminal program a screen that scripts and
AI agents can read and drive, and lets agents message each other.

### Added

- Calls, in Docker's dialect: `-d` detaches (printing the 64-hex call id),
  `-dt` starts on a headless terminal, `-w` keeps a waiter that records the
  exit code, `--rm` removes the call when it ends, `--name`, `-e`,
  `--restart`, `--size COLS ROWS`, `--tail CALL` follows a call's log. Why
  Docker's spelling: it is the process vocabulary most developers and agents
  already know.
- User mode: `callbox <cmd>` with no session flags (or `-it`) runs the
  program attached on a PTY, as `callbox shell` runs a shell - with the chat
  window when there is a terminal, so messages for the call go there and are
  never typed into it - and redial brings that program back. Why: it is how
  a person runs a terminal program, and a shell's redial could only bring
  back the shell, never what ran in it. From a script it still runs on its
  PTY, and its output arrives as a stream.
- `-i` relays your input to the program on a pipe its broker holds. Why: a
  program handed your terminal keeps reading your keystrokes after you
  leave; through the broker, Ctrl-P Ctrl-Q can detach it, `callbox attach`
  reconnects its input and output, piped input still works (EOF at its
  end), and `-di` keeps the input open for a later attach. At a terminal,
  callbox edits each line (echo, backspace, Ctrl-U, Ctrl-D = end of input).
- The same keys wherever you are on a call: Ctrl-C goes to the program
  (Docker's sig-proxy; without a terminal, a SIGINT sent to callbox is passed
  on), Ctrl-P Ctrl-Q detaches and leaves it running. Why: one rule is easier
  than Docker's (whose detach needs a TTY), and leaving a program running
  should never depend on how it was launched. Following a log (`tail`,
  `--tail`, `logs -f`) is watching, not being on a call: Ctrl-C stops
  following. Programs start with default Ctrl-C handling, whatever callbox
  inherited, so a forwarded Ctrl-C always takes effect.
- An attached launch passes a SIGTERM on to its program (`timeout 8 callbox
  foo` ends foo too). Why: the program was run there and must not be left
  behind unseen; a SIGHUP still only detaches, because callbox keeps programs
  running. `callbox foo | head` ends with 141 and says foo keeps running.
  Why: the reader having had enough is not a reason to end the call.
- Each run records where its output starts in the shared log (`log_start`).
  Why: redials append to one log, and a run that ended before its launch
  could attach should show its own output, not every earlier run's.
- A client with no terminal on its side (a script, `attach` into a file)
  lets the call's screen answer the program's terminal queries. Why: nobody
  else will, and a program that waits for its cursor position would stall.
- A launch from a script passes the end of its input on to a PTY program as
  the terminal's end-of-input key, with its echo kept out of the output.
  Why: macOS echoes it as `^D` into what a script reads; Linux does not.
- `wait --match` finds text that wrapped at the right edge. Why: a long
  line split across two screen rows is still one line to the person reading
  it.
- Shortened words: the start of a name is a target when one call the verb
  can act on fits (`callbox logs hel`), and a top-level word that is no
  verb, no call and no program may be the start of one verb or call name
  (`callbox li`, `callbox hel`). Why: less typing, and nothing that meant
  something before changes meaning - whole names, id prefixes and programs
  on PATH still come first.
- More than one match is never a guess: callbox lists the calls (or verbs)
  and exits 6, for every verb. Why: "no call matches" (5) was false, and
  acting on one of several would be a guess.
- Naming a call that is still running gets you back to it: attached if it
  has a console, its log followed otherwise (`-d`: its id, exit 0). Why:
  there is nothing to start, and getting back is why you typed its name.
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
