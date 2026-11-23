# Changelog

## 0.9.11 - 2026-11-23

### Added

- An agent outside callbox can hold a conversation. Its first `callbox msg`
  gives it a name, made like a call's from its harness process (Claude Code,
  say: shells and wrappers are stepped over), and an inbox under that name.
  The receiver sees an ordinary header and replies with an ordinary
  `callbox msg <name>`, which lands in the inbox. `callbox inbox` lists the
  replies; `--wait` returns the next unread one (one that came while nobody
  waited included); `--monitor` prints each as it arrives, one line each,
  until stopped. `callbox msg ... --wait-reply` sends, then waits for the
  first reply. The first message prints these options once. Names and
  messages expire an hour after their last use. Why: such an agent had no
  prompt for a reply to be typed into, so it could only shout into a call
  and never hear back.
- The chat window files an outside sender's messages under its name, and a
  reply from the window goes to its inbox (it said "no call to answer").

### Changed

- From outside any call, the header names the sender
  (`[msg from melodic_silver; reply: ...]`) instead of
  `[msg from outside callbox]`, which is left only for when no name can be
  made.

## 0.9.10 - 2026-11-23

### Added

- `callbox help tui`: troubleshoot a terminal UI - a fixed size, peek with
  coordinates or as a picture, exact keys with send --seq and wait --since,
  the raw bytes in the log, the last full-screen frame, and TERM.
- `callbox help comms`: agents in different harnesses (Claude Code, Codex,
  opencode, ...) talking through callbox msg, and what to put in each
  harness's instructions file.

### Changed

- The main help ends with TUTORIALS (examples, tui, comms, features) and
  REFERENCE (all, launch, scripting and system, one command's -h).
- The README is a short synopsis: what callbox is, install, three things to
  try, and where to learn more. Its former body is `docs/guide.md`.

## 0.9.9 - 2026-11-23

### Added

- `callbox help features`: what callbox does under the hood - one binary
  and no daemon, signals that cannot hit a reused pid, the headless
  terminal, calls that come back, names, messages, and where it keeps
  working (FIFOs, PID namespaces, sudo).
- `callbox help service` and `callbox help examples` show testing a network
  service in CI (start it, wait until it is ready, keep its log when the
  tests fail) and a QEMU mesh driven through each node's serial console.

### Fixed

- `callbox help service` gave a ~/.bashrc line with `-q`, which callbox
  does not have (only `--quiet`), and which would have printed "already
  exists" in every new shell. The line is now silent and was run as shown.

## 0.9.8 - 2026-11-23

### Changed

- The help, rewritten. `callbox help` lists the commands by use case - run
  agents and programs yourself, orchestrate agents, drive terminal apps,
  services and background processes - in usage notation (`<call>`,
  `[<args>]`), each section pointing at a page with examples. New pages:
  `callbox help all` (every command and flag), `users`, `agents`,
  `messaging`, `service` (background processes without systemd, including
  one idempotent ~/.bashrc line to keep one running), and `examples`
  (whole workflows). `help you` and `help background` from 0.9.7 are gone:
  `users` and `service` replace them.

## 0.9.7 - 2026-11-23

### Added

- A launch says the call's name. `-d` / `-dt` print the id on stdout as
  before, then `name: NAME` on stderr; `-it` (and `callbox attach`) shows
  `callbox: NAME` in the top-right corner for 5 seconds, then the screen as
  the program drew it. Why: with several calls running, whoever started one
  could not tell which name was theirs, the name msg, peek and attach take.
- `callbox help you`, `callbox help agents`, `callbox help background`: the
  help by use case - starting an agent or program yourself, driving
  terminal apps and messaging from an agent, and background processes (like
  nohup or setsid, with control kept). The main screen leads with the three.

### Changed

- The chat window pads names to 15 columns, so message times line up; a
  longer name pushes its time out and is never cut.

## 0.9.6 - 2026-11-23

### Fixed

- `callbox she` (any shortened `shell`) starts a callbox shell; it printed
  the help. Why: `shell` was checked before the word was expanded.

### Changed

- The chat window labels your own messages with your call's name (the name
  the other side sees and replies to) instead of "you".

## 0.9.5 - 2026-11-23

### Changed

- macOS: the boot id is `kern.bootsessionuuid` (`macos-<UUID>`), compared
  exactly, replacing 0.9.3's boot time matched within 60 seconds. Why: the
  UUID is new at each boot and ignores the clock, so a badly wrong clock
  that NTP corrects by minutes no longer makes calls stale. Calls started
  before this read as stale; restart them.

## 0.9.3 - 2026-11-23

### Fixed

- macOS: calls no longer turn "stale" a few minutes after they start. The
  boot id there is built from `kern.boottime`, which macOS shifts slightly
  when it adjusts the clock (one Mac: 0.066 s), and callbox compared it
  exactly; every call then read as stale, and `msg`, `send`, `end` and
  `list` treated live calls as ended ("savvy_gecko has ended"). Boot times
  within 60 seconds are now the same boot. Calls already running read
  correctly again with this binary, without a restart.

## 0.9.2 - 2026-11-23

### Fixed

- `callbox -it <cmd>` no longer opens the chat window: messages for the call
  are typed into the program, as for `-dt`. The chat window is for the
  switchless `callbox <cmd>` and `callbox shell` only. Why: 0.9.0 made `-it`
  user mode, so an agent started with `-it` never saw its messages, and one
  from outside any call sat in a corner window with nothing to answer it.

## 0.9.1 - 2026-11-23

### Changed

- `msg` headers say who sent a message and how to reply, and nothing more:
  `[msg from lead; reply: callbox msg lead "..."]`, or `[msg from outside
  callbox]`. The sender is named by its call's record as it is now (a renamed
  call shows its new name). Removed: the session and process-ancestry check
  on the sender, the `(unverified)` label and chat conversations, and the
  `verify:` step in the header. `msg --received [ID]` stays, as the call's
  message history. Why: only the console's own user can reach it, and anything
  running as that user can type the same text with `callbox send`, so the
  check guarded nothing; it cost every agent a longer header and a verify
  instruction, and "verified" read as "safe" when it meant only "from".

### Fixed

- `callbox help` keeps its first line within 80 columns when the version is
  long (a development build's `0.9.0-1e6e4c4-dirty`, or a packager's): the
  description then goes on its own line, with the same words. Why: every help
  line is meant to fit 80 columns, and the suite's check of that failed on any
  build from uncommitted work.
- A redial keeps the call's saved flag and name. Why: it rewrote the record
  from the new run alone, so a saved call lost its protection and the next
  plain `callbox purge` deleted it.
- Redials of one call at the same moment start it once; the others exit 6
  (already running). Why: "not running" was checked before launching, with
  nothing between the two, and 16 simultaneous redials started 16 programs
  under one id - `end` stopped one and the rest ran on. A redial now holds a
  lock on the call through its launch and checks after taking it.
- Purge leaves a call alone while a redial is launching it, and checks a
  call is still ended under the store lock before removing it. Why: the
  redial's running record is written at the end of its launch, so a purge
  in between removed a call that was starting.
- `--restart` reports a program that cannot start (126/127) and leaves no
  call, as a launch without it does. Why: the supervisor signalled success
  before its first worker ran, so `-d --restart` printed an id for a program
  that never ran.
- A command found on `PATH` but not executable is "permission denied"
  (126), not "command not found" (127). Why: 127 sends the person looking
  for a missing program when the fix is chmod; by path the same file was
  already 126.
- `make test` builds and runs `./callbox-test` and no longer replaces
  `./callbox`. Why: it left a dynamically linked test build where the
  release binary goes, and a later `make` called it up to date.
- `end`, `stop` and `kill` of a call that has already ended say
  `<name> already exited` and exit 0. Why: they said "no call matches" (exit
  5) about a call `list -a` shows, and `callbox help scripting` promises 0
  for a known name with nothing to do - a cleanup step failed though its
  goal already held.
- `send --wait` says the input was sent when the wait then fails: "input
  sent; q then ended (exit code 0) before going quiet" (8), "input sent; busy
  did not go quiet within 1000 ms" (7). Why: it printed `wait`'s own text, so
  a failed step read as if the keys had not arrived. `wait` and `send --wait`
  also count a console that closes while the request is on its way (the
  broker's "the call has ended" reply, or a broken connection, as the FIFO
  transport shows) as the call ending, not as an error (1 or 3).
- `msg` removes a whole escape sequence from message text, not only its
  first byte: `\e[31mRED` arrives as `RED`, not `[31mRED`. Why: the leftover
  bytes were noise in the receiver's prompt that no sender meant as text.
- `logs -h` says what `logs` prints - the captured bytes, escape sequences
  included for a TTY call - instead of "plain text"; `peek -h` lists every
  image option and the default scale.
- A refused `attach` always says who has the call (exit 3). Why: when the
  broker turned it away before the attach's opening bytes reached it, the
  attach's own write failed first and it either died of SIGPIPE (exit 141) or
  called the console gone - "has closed its terminal and runs on", about a
  call that was running and attached elsewhere. It now reads the broker's
  reason before deciding, and ignores SIGPIPE for the whole attach.
- Every CI suite lane and the Linux CI container install the programs the
  screen tests drive (tmux, whiptail, vim, micro), and CONTRIBUTING lists
  them. Why: no lane installed it, so the micro mouse test
  (click, double-click, drag select, wheel) skipped everywhere and its SKIP
  looked like coverage. It passes.
- README: a curses program (htop) can read `-k Escape` and the next key in
  the same `send` as Alt-key - send Escape on its own for those; the script
  claims say what the renderer does (cells drawn one by one: Arabic is not
  joined); the CI example purges its call on exit, so a second run can use
  the same name.

## 0.9.0 - 2026-11-22

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
- Exit codes that say what happened: 127 command not found and 126 found
  but not runnable (what shells, CI and `docker run` use); 10 the call has
  no terminal screen or console and 11 the name is taken (both were 5, "no
  such call", which scripts could not tell apart); `attach` exits 0 when you
  detach, 8 when the call ended, 3 when refused.
- `callbox msg` prints the delivered message's id. Why: a sender can then
  match `msg --received` to what it sent.
- `end`/`kill` with several targets act on each: the ones that resolve are
  ended, the rest reported, the worst code returned. Why: one mistyped name
  used to end none of them.
- `--restart` backs off when no `--restart-delay` is given: 1 s, doubling to
  30 s, reset after a 10 s run. Why: a program that fails at once would
  otherwise restart in a tight loop (forever under `always`).
- Call names that are callbox verbs are refused at `--name` and `rename`.
  Why: the verb always wins, so the call could never be redialed by name.
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
