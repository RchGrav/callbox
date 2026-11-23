# Screens: peek, send, wait, resize, msg

[Docs index](index.md) | [Repository README](../README.md) | Related: [Console](console.md), [Implementation invariants](SPEC.md)

A call placed with `-t` runs on a PTY owned by its **console broker**, a
process born with the call and gone with it (Callbox stays daemonless). The
broker writes every output byte to the call's log, and also feeds it to a
headless terminal — vendored [libvterm](../third_party/libvterm/) 0.3.3 —
so it always knows the call's screen: every cell's characters, colors and
attributes, the cursor, and the program's input modes.

## Layers

| Code | Role |
| --- | --- |
| `src/screen/screen.c` | The terminal wrapper: feed, resize, snapshot, mode-aware key/paste/mouse encoding, the HSNAP1 format, plain text, repaint. Depends on core only. |
| `src/screen/render.c` | Snapshot to PNG: 8x16 cells (GNU Unifont subset, Noto Emoji subset for wide emoji, procedural box drawing/blocks/Braille), ruler, cursor ring, mouse reticle, footer, and a small fixed-Huffman deflate encoder. |
| `src/console/broker.c` | Owns the PTY; one attach client plus up to 64 control clients (more are answered `error busy`, which is safe to retry because nothing was done; clients retry with backoff for up to 10 s); waits, queued messages, the final screen. |
| `src/console/compose.c`, `chat.c` | Only for a `callbox shell`: the composited display and the chat window ([shell.md](shell.md)). |
| `src/console/control.c` | The client side of a control request. |
| `src/runtime/screen_cmds.c` | `callbox peek / send / wait / resize / msg`. |

## The control protocol

Every connection to a call's socket starts undecided. A client that opens
with the 8-byte magic `holdctl1` is a **control client**: it sends one
request as a big-endian u32 length plus text, receives one reply in the same
framing, and is closed. Anything else is the attach client (`holdv1` framed,
or raw passthrough). A connection that sends nothing within 250 ms becomes a
raw attach, so output-only clients still see the screen. Over the FIFO
transport ([console.md](console.md)) the introduction's kind byte decides
instead, and a control client sends no magic.

This table is the protocol:

```text
peek [alt=1]                 -> ok\n + HSNAP1 snapshot (alt=1: last full-screen frame)
send\n text <hex> | paste <hex> | key <name> | mouse <action> <btn> <row> <col>
                             -> ok seq=N [mouse=off] [mouse=clamped]
resize rows=R cols=C         -> ok rows=R cols=C
wait [idle=MS] [since=SEQ] [match=<hex>] [regex=1] [exit=1] [timeout=MS]
                             -> ok reason=idle|match seq=N
                              | ok|ended reason=exit seq=N exit_code=C
                              | timeout seq=N
msg [from=<hex>] [submit=0]\n<hex text>
                             -> ok delivered id=<6 hex>
signal sig=N                 -> ok   (TERM, KILL, INT or HUP, to the program's process group)
nested keys=<hex>            -> no reply; held open while a nested attach client lives (console.md)
```

Errors are one line, `error <why>`: for example `error busy`, `error the
call has ended`, `error rate limit: 20 messages a minute to one call`,
`error too many undelivered messages`. When the program exits, a `wait` that asked
for the exit (`exit=1`, or no condition at all) replies `ok`; any other
replies `ended`. A `timeout` of 0 or none means no deadline.

`seq` is the count of output bytes the broker has seen; `callbox send --seq`
prints it and `callbox wait --since SEQ` requires output beyond it, which
closes the send-then-wait race. `--idle` holds once the program has drawn
something and then stayed quiet for the given time; silence before the first
output never counts. Without `--since`, silence counts from the later of the
wait's start and the last output, so a reply to input sent just before the
wait is still awaited.

## The size a call starts at

Sizes are `COLS ROWS` in character cells everywhere — `--size`, `callbox config
set detach-size`, `callbox resize` — parsed by one function
(`hold_parse_term_size`: two words, or `COLSxROWS`) with one set of bounds
(1 to 1000 each). Zero, negative, non-numeric, incomplete, and out-of-range
sizes are refused before anything is started or saved.

The launcher resolves a TTY call's first size, in this order:

1. `--size COLS ROWS` (a redial replays the size its recipe was placed with,
   unless the redial gives its own);
2. for a detached launch (`-dt`, `-dwt`) only, the saved default
   (`callbox config set detach-size`, kept in `~/.config/callbox/config`);
3. the invoking terminal's size, when stdin or stdout is a terminal that
   reports a usable one;
4. 80 columns by 24 rows.

The broker opens the PTY and creates the headless screen at that size before
it forks the program, so a TUI that asks for its dimensions the moment it
starts gets the right answer. `--size` is where the call starts, not a lock:
`callbox resize`, and an attached terminal's window, change it afterwards.
`--size` without `-t` is refused (a PTY is never allocated just because a
size was given), a saved default never touches a call that is already
running, and a saved value that no longer reads as a size is not guessed at:
the launch stops and says how to set or clear it.

## Query replies and attached terminals

Programs ask their terminal questions (cursor position, device attributes).
With no one attached, the headless terminal answers them, so programs that
wait for replies work under `-dt`. While any attach client is connected, the
broker drops its own replies so nothing is answered twice; the attached
terminal answers. A client that is not a terminal (a pipe, `socat`) answers
nothing, so a program waiting on a query stalls while it is attached. A
callbox shell is the exception: its headless screen always answers
([shell.md](shell.md)). A newly attached terminal is repainted from the screen —
cells, colors, input modes, cursor — rather than replayed from bytes.

## HSNAP1

The snapshot format, little-endian: magic `HSNAP1\0\0`; u16 rows, cols,
cursor row, cursor col; a mode byte (cursor visible, alternate screen,
application cursor keys, application keypad, bracketed paste, focus
reporting, SGR mouse) and the mouse tracking mode; the broker's metadata
(u64 output counter, u32 idle ms, flags for running / has exit code / has
mouse / size set by resize, i32 exit code, u16 last mouse row and col); a
u16-length title; then rows x cols cells of `u32 ch[3]` (base character and
two combining marks; 0 = blank), u8 width (1, 2, or 0 for the right half of
a wide character), u8 attributes, u8 default-color flags, RGB foreground,
RGB background. Colors are resolved RGB with reverse video not applied;
palette indexes are not carried.

When no more output can come, the broker writes the final snapshot to
`<id>.screen` beside `<id>.log`. It rewrites it once the program is reaped,
so the file records the exit. `callbox peek` reads it for ended calls, and
purge removes it with `<id>.altscreen` and `<id>.msgs`.

Full-screen programs draw on the alternate screen and switch back when they
exit, which leaves the normal screen showing none of what they displayed.
When output contains the switch-back (`CSI ?1049l`, `?1047l`, `?47l`), the
broker feeds the screen up to it, keeps a snapshot of that last full-screen
frame, then feeds the rest. `callbox peek --last-alt` asks for it (`peek
alt=1`); at exit it is saved as `<id>.altscreen`. Only the first
switch-back in each read of output is seen, and only while the alternate
screen is active; a sequence split across two reads is missed.

## Messages between calls

(A `callbox shell` call is the exception to what follows about typing: the
launch command sets its delivery mode, and a message for it goes to its chat
inbox, at once, and is never typed. See [shell.md](shell.md).)

`callbox msg` hands text to the *receiver's* broker, which types it in at
once. Waiting for a quiet screen would only block the sender until the
receiver's turn ends: an agent harness queues what is typed while it works,
and a busy agent's spinner never goes quiet. Two things can hold a message
back. A sender whose call has no record yet gets up to 2 s, because a call
that messages the instant it starts can beat its own record, and the
header's name comes from that record. And
messages go in oldest first, one per broker pass, with at most 16 pending.

Every control character except newline and tab (C0 bytes, DEL, C1) is
removed first, so a message can never type Escape, submit early with a
carriage return, end a bracketed paste, or drive the terminal. An escape
sequence (CSI, OSC, DCS and the other string forms) is removed whole, not just
its first byte, so `\e[31mRED` arrives as `RED` rather than `[31mRED`. The text goes
in as a bracketed paste when the program enabled mode 2004. Otherwise
newlines and carriage returns become spaces, so it cannot submit early.
Enter follows, unless `--no-enter` was given (`submit=0`).

The sender names its call by `CALLBOX_ID`, else `CALLBOX_NAME`; only an id
or a call name is taken (anything else could break the header or a `.msgs`
line). The receiver looks that call up and shows its name as the record has it
now, so a renamed call is shown, and answered, by its new name. The header,
on one line before the text, takes one of two forms:

```text
[msg from NAME; reply: callbox msg NAME "..."]   from a call
[msg from outside callbox]                       from no call callbox knows
```

The header is addressing, not proof. Only the console's own user can reach
it (the socket checks the peer's uid; FIFOs are the user's), and anything
running as that user could type the same text with `callbox send`. Why there
is no check beyond that: an earlier version verified the sender's session and
process ancestry, which guarded only against the user's own processes, put a
verify step and a longer header in front of every agent, and read as "safe"
when it meant only "from".

Each delivered message is appended to `<id>.msgs` as `ID UNIXTIME FROMCALL
SENDER HEXTEXT` (FROMCALL is 1 when the sender is a call; it is where that
earlier version wrote its verified flag, so older logs read the same).
`callbox msg --received` reads it: the call's message history. Delivery is
capped at 20 messages a minute per receiver, counted when a request is
accepted, to stop two agents replying to each other in a loop.
