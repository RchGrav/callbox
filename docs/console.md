# Console

[Docs index](index.md) | Related: [Screens](screen.md), [Launcher](launcher.md)

A process placed with `-t` - and every user-mode launch, `backmux <cmd>` with no
session flags - runs on a pseudo-terminal owned by its **console broker**:
`backmux <cmd>` and `backmux -it <cmd>` attach to it at once, `backmux -dt
<cmd>` leaves it running in the background, and `backmux attach <target>`
picks it back up later (`console` is an alias). A process placed with `-i` (no
`-t`) has a broker too, holding its input pipe instead of a PTY (see [Programs
on pipes](#programs-on-pipes)). `Ctrl-P Ctrl-Q` detaches without stopping the
process; `--detach-keys` changes the sequence.

The broker is forked per process and lives exactly as long as the process. It owns
the PTY master and a private socket under the store's `console/` directory
(0700 directory, 0600 socket). The broker admits root, the process's owner, or
the sudo-named user, judged by peer credentials. The client in turn refuses a
listener owned by anyone but root or the socket's owner, so a swapped-in
listener cannot impersonate the broker. The broker:

- appends every output byte to the process's log;
- feeds the same bytes to a headless terminal, which `backmux peek`, `send`,
  `wait`, `resize`, and `msg` use (see [screen.md](screen.md));
- serves one attach client at a time, plus short-lived control clients and
  the registrations of nested attaches (below). A second attach is told
  where the console is held: `backmux: web is already attached in another
  terminal (pid N). Detach it there: Ctrl-P Ctrl-Q.`, or `...from inside the
  call X...` when the holder runs inside another process;
- repaints a newly attached terminal from the live screen (cells, colors,
  input modes, cursor), so an idle reattach shows the real current state;
- forwards window-size changes to the PTY (the kernel signals SIGWINCH);
- stamps the process's exit status when the target ends and writes its final
  screen beside the log.

### Letting go

`Ctrl-P Ctrl-Q` is read by the attach client, which sends a detach frame;
the broker answers by closing the connection. `Ctrl-P` is a lead key: it is
held until the next key arrives, with no timer: not everyone types fast, and Docker
and similar tools leave the lead key untimed too. `Ctrl-Q` then detaches,
`Ctrl-P` again sends one `Ctrl-P` to the program (the quick way through), and any other key sends
both through. (A `backmux shell`'s broker reads these keys itself, so it can
show what the lead key offers while it waits; see [shell.md](shell.md).)

Ctrl-C means one thing wherever you are on a process: it goes to the program. On
a PTY the key reaches the program's terminal, which signals it. For a program
on pipes, the attach client turns Ctrl-C into an interrupt frame and the
broker sends SIGINT to the program's process group. Following a log (`backmux
tail`, `--tail`, `logs -f`) is not being on a process: there, Ctrl-C stops
following.
Two cases need more than that.

**Nested attach.** Attach to one process from inside another — `backmux attach
ktui` typed in a `backmux shell` — and there are two clients in a row, with the
detach keys reaching the outer one first. Taken there, they would put the
shell in the background with the inner attach still running inside it,
holding its process's console where no key can reach it. So an interactive
attach client (stdin and stdout both terminals) started inside a process
(`BACKMUX_ID` names it, and its console endpoint exists) tells that process's
broker so: a
`nested keys=<its detach keys>` control request on a connection it keeps
open while attached (the broker accepts it only from a process in its own
program's session; the connection closing is how it learns the client has
gone). When a detach frame arrives and such a client is at the front of the
terminal — in the PTY's foreground process group — the broker types that
client's keys into the PTY instead of detaching its own. (Where the PTY will
not say which group is in front, the most recently registered nested client
is chosen.) The keys therefore
let go of the innermost attach, the one on screen; pressed again, with
nothing nested, they detach the outer process. And when an attach client goes
away for any other reason while one is nested in its terminal, the nested
one is detached the same way: nobody is looking at it any more.

**A client that vanishes.** A closed window or a killed process sends no
detach frame. The broker sees the connection's input end, which a client
that only reads (`printf 'q\n' | backmux attach web`) also produces, so that
alone is not the client leaving. It keeps watching for the connection to
hang up, and for the client's process to stop existing (what macOS and FIFOs
can report), on every pass and again before turning another client away. A
quiet program prints nothing that would reveal a dead client by a failed
write, so without these checks its console would stay "already attached".

The attach client saves the terminal settings, switches to raw mode and the
alternate screen, and on detach resets the modes a program may have left on
(mouse reporting, bracketed paste, application keys, a hidden cursor) before
restoring the terminal. Non-TTY attaches stream stdin/stdout without screen
switching. A connection that sends nothing for 250 ms becomes a raw attach,
so a client that only reads (`socat -u`) still sees the screen; anything it
sends later is typed as raw input.

When the program of a foreground `-it` launch ends, `backmux` exits with its
exit status, as a launch without `-t` does. The alternate screen goes with
the attach, so the process's last screen (the frame its broker saved) is then
printed on the terminal's own screen, where it stays to be read; the same
when a process ends under a `backmux attach`. A program that is over before the
attach reaches it is not an error: its console is gone, so the last screen
(or, with no terminal attached, its log) is all there is to show, and the
exit status is still its own.

A program that closes its terminal and runs on (a daemon dropping its
stdio) has nothing more to show, and nothing typed can reach it. Its broker
lets the attached client go at once, without waiting for the program to
exit, and the client says so: `backmux: web has closed its terminal and runs
on; there is nothing to attach - see 'backmux logs web'`.

A `backmux shell` process uses the same broker, attach client, and wire protocol,
with one difference in what travels: its attach client is sent frames the
broker composes (the screen with the chat window over it) instead of the
PTY's bytes, and what it sends back is routed between the shell and chat.
See [shell.md](shell.md).

The terminal a process starts on is settled by the launcher before the broker
exists — `--size COLS ROWS`, else the saved `detach-size` for a detached
launch, else the invoking terminal's size, else 80x24 — and both the PTY and
the headless screen are that size before the program is started, so its
first look at its terminal is already right. See [screen.md](screen.md).

The wire protocol: a client opening with the 8-byte magic `holdv1\0\0` sends
3-byte `type + be16 length` frames (`D` data; `W` resize, payload be16 rows
then be16 cols; `X` detach; `E` end of input (a program on pipes reads EOF; on a PTY the broker types its end-of-input key, keeping its echo out of the output); `S`
interrupt - empty, Ctrl-C for a program on pipes, or one byte naming SIGINT
or SIGTERM for either kind; `P` "send me a stream", from a launch attaching
for a script; `N` "no terminal here", from any client whose stdin or stdout
is not a terminal, so the broker's screen answers the program's terminal
queries - nobody else will, and a program waiting for a reply would stall) and receives the screen as plain, unframed bytes. A client opening with `holdctl1` is a control client (see
[screen.md](screen.md)). Anything else is raw passthrough in both
directions.

## Programs on pipes

`backmux -i <cmd>` (without `-t`) puts the program on pipes under a broker:
its stdin is a pipe the broker holds, and its stdout and stderr a pipe the
broker copies into the log and the headless screen (reading `\n` as `\r\n`,
as a PTY would, so `peek` and `wait --match` work). Why a broker for a
program without a PTY: so the program never holds your terminal (handed your
terminal directly, it would keep reading your keystrokes after you left), so
Ctrl-P Ctrl-Q can detach it, and so `backmux attach` can reconnect its input
and output later. `-di` starts it in the background with its input open.

The attach client for such a process keeps your terminal's lines and scrollback
(no alternate screen) and reads keys one at a time, editing each line itself:
echo, backspace, Ctrl-U, Enter sends the line, Ctrl-D on an empty line is end
of input (an `E` frame: the broker closes the program's stdin). With piped
input, bytes go through as they are and the end of the pipe is end of input.
Instead of a screen repaint, the first client - the launcher, attaching as
the program starts - gets this run's output so far from the log, so nothing
printed before it connected is lost; a client attaching later gets what
comes next, as `docker attach` does.

A launch that attaches from a script (stdin or stdout not a terminal) asks
for the same stream on a PTY process too (`P` frame), so a script gets the
output, not cursor moves. `backmux attach` keeps its repaint of the live
screen.

## Where Unix sockets are denied

Some sandboxes refuse Unix-domain sockets: `socket`, `bind`, or `listen`
fails with `EPERM`, `EACCES`, or `EAFNOSUPPORT`. There the broker serves
the same console over FIFOs instead, automatically.
`BACKMUX_CONSOLE_TRANSPORT=fifo` forces that transport for tests. FIFO is a
supported way to operate a process when sockets are unavailable: peek, send,
wait, resize, msg, and attach with detach and reattach retain their usual
behavior. A successful test with sockets available does not exercise the
automatic fallback; the socket-denied tests below cover that condition.

The endpoint is a FIFO (0600) at the usual `<id>.sock` path, and clients pick
the transport by its file type. It only ever carries fixed-size 17-byte
introductions — `kind` (`C` control, `A` attach), the 6-character suffix of
a private `<id>.sock.XXXXXX` directory the client made beside it, and the
client's pid — which are atomic, so concurrent clients never interleave. The
directory holds an `in`/`out` FIFO pair: the broker opens it, removes the
directory, and answers on `out`: `R` ready, `D` denied, or `B` busy (every
control slot is taken; for an attach refused because the console is held,
`B` is followed by the holder's pid as 10 digits). From then on the pair is the connection, speaking the framed protocol above
(control clients skip the `holdctl1` magic; the kind already said it).

Access follows the socket rules. The client accepts an endpoint it owns (root
accepts the process owner's, and hands its channel to that owner); the broker
admits root, the process's owner, or the sudo-named user, judged by who owns the
channel's FIFOs, which must be private. A client's claimed pid is accepted
only if that process holds the channel's `in` FIFO open (Linux
`/proc/<pid>/fd`, macOS libproc). If the pid cannot be observed, it remains
unknown; this does not reject an otherwise authorized control request.
Channels left half-made by a client that died are removed by the broker
when it exits and by `backmux purge`.

On macOS, `poll()` never reports end-of-file on a FIFO (Linux does), so on
quiet passes the attach client and the broker probe their FIFOs with
non-blocking reads to notice a party that has gone. The client probes every
250 ms, so on a Mac over FIFOs the end of a process can reach it up to that
late. This is compiled into the macOS build only; Linux needs no probe and
has no delay. Kept rather than replaced with kqueue (which does report a
FIFO's end on macOS): it is the fallback transport, a quarter second at a
process's end costs nobody anything, and kqueue would add a second waiting
mechanism to the client loop on one platform.

When the program can produce no more output — it exited, or it closed its
terminal and runs on — the broker stops listening at once (either transport)
and saves the final screen before waiting for the process, so `backmux peek`
answers immediately with that screen and says whether the program still runs.

## Across PID namespaces

Some sandboxes give each shell its own PID namespace over a shared
filesystem. A process ID from another namespace may be invisible here, or
may identify an unrelated process. Backmux records the launching namespace
(`pid_ns`, the inode of `/proc/self/ns/pid`) and, from a different namespace,
uses the shared store and the process's broker instead of consulting that pid:

- **Alive?** Under the fallback (Unix sockets denied, or
  `BACKMUX_CONSOLE_TRANSPORT=fifo`), a shared lock on the process's log, held for
  its life, says it runs. A supervisor holds it — the broker of a `-t` or
  `-i` process whose endpoint came up as a FIFO, or the waiter that a `-w`,
  `--rm`, `--restart`, or foreground launch leaves behind. A recorded exit
  gives `Exited (N)`. Successfully checking that the lifetime lock is no
  longer held gives `Exited (?)` if no exit was recorded. A missing or
  unreadable log, or an error checking its lock, gives `Unknown`: failure to
  inspect a lock is not evidence that it was released. A plain `-d` process has
  no supervisor; its output descriptors carry the lock, but its program may
  close them, so even a successfully observed unlocked log proves no exit.
- **Drive it.** `peek`, `send`, `wait`, `resize`, and `msg` talk to the broker
  through the console endpoint, a path both shells reach. `attach` uses the
  same endpoint. They can try that connection even when status is `Unknown`:
  a broker can still answer when this shell cannot inspect the process's pid or
  log lock. The connection uses the same access checks as usual.
- **Stop it.** For a `-dt` or `-di` process, `end`/`kill` ask the broker (a
  `signal sig=N` console request) to signal its program, which it can see.
  This can work when status is `Unknown` too. A plain `-d` or `-dw` process has
  no broker; end it from the environment that started it. An unreachable
  broker alone does not establish that the program exited.
- **Can't tell.** No exit recorded and nothing vouching for the process — a
  plain `-d` process whose lock is gone, a process with an unavailable lock probe,
  or a process placed without the fallback — reads `Unknown`. Purge sweeps
  leave it alone; a targeted `backmux purge NAME --force` removes its record.
  Control goes through an available broker, never a foreign pid.

Without the fallback, nothing is added: no lock is taken, and processes run
exactly as they do anywhere else. A plain `-d` process never opens a socket, so
the launcher checks for the fallback explicitly: is FIFO forced, or is
`socket` refused — and, on Linux, `bind` or `listen` on a throwaway abstract
address? In the launching namespace, and on
macOS (no PID namespaces), local evidence decides.

### Regression coverage

The `fifo-console` CI job runs the suite with FIFO explicitly selected and
again with Unix sockets denied by `tests/socket_ban_shim.c`. The second run
exercises automatic fallback. In `tests/test_backmux.sh`:

- `test_fifo_console_full_capability` exercises the FIFO console itself.
- `test_other_pid_namespace_follows_chain_of_command` checks live processes,
  broker control, and exit evidence across PID namespaces.
- `test_foreign_namespace_unavailable_log_is_unknown` checks missing and
  unreadable logs, including protection from an ordinary purge.
- `test_foreign_namespace_unknown_console_remains_usable` checks screen
  inspection, input, resizing, messaging, and waiting through a reachable
  FIFO broker while liveness evidence is unavailable.
- `test_foreign_namespace_unknown_exit_wait_times_out` checks that an
  unobservable process without a broker does not acquire an invented exit.
- `test_foreign_namespace_unreachable_console_is_not_ended` checks that a
  connection failure is reported without claiming that the process exited.

These cases preserve the distinction between socket availability, process
visibility, and evidence that a process has actually ended.
