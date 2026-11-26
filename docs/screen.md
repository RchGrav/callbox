# Screens: peek, send, wait, resize, msg

[Docs index](index.md) | [Repository README](../README.md) | Related: [Console](console.md), [Implementation invariants](SPEC.md)

A terminal process runs on a PTY owned by its **console broker**. The
broker writes every output byte to the process's log, and also feeds it to a
headless terminal — vendored [libvterm](../third_party/libvterm/) 0.3.3 —
which tracks the modeled screen cells, colors, attributes, cursor, and
program input modes. Pipe-input processes also feed their output to a
headless screen, with line feeds translated for display.

## Layers

| Code | Role |
| --- | --- |
| `src/screen/screen.c` | The terminal wrapper: feed, resize, snapshot, mode-aware key/paste/mouse encoding, the HSNAP1 format, plain text, repaint. Depends on core only. |
| `src/screen/render.c` | Snapshot to PNG: 8x16 cells (GNU Unifont subset, Noto Emoji subset for wide emoji, procedural box drawing/blocks/Braille), ruler, cursor ring, mouse reticle, footer, and a small fixed-Huffman deflate encoder. |
| `src/console/broker.c` | Owns the PTY; one attach client plus up to 64 control clients (more are answered `error busy`, which is safe to retry because nothing was done; clients retry with backoff for up to 10 s); waits, queued messages, the final screen. |
| `src/console/mux.c`, `compose.c`, `chat.c` | The attach-client pane compositor, interactive help, and chat window ([shell.md](shell.md)). |
| `src/console/control.c` | The client side of a control request. |
| `src/runtime/screen_cmds.c` | `backmux peek / send / wait / resize / msg`. |
| `src/runtime/inbox.c`, `src/store/inbox.c` | Outside sender identity, inbox history, `inbox --wait`, `monitor`, and reply waits. |

## The control protocol

Every connection to a process's socket starts undecided. A client that opens
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
process has ended`, `error rate limit: 20 messages a minute to one process`,
`error too many undelivered messages`. When the program exits, a `wait` that asked
for the exit (`exit=1`, or no condition at all) replies `ok`; any other
replies `ended`. A `timeout` of 0 or none means no deadline.

`seq` is the count of output bytes the broker has seen; `backmux send --seq`
prints it and `backmux wait --since SEQ` requires output beyond it, which
closes the send-then-wait race. `--idle` holds once the program has drawn
something and then stayed quiet for the given time; silence before the first
output never counts. Without `--since`, silence counts from the later of the
wait's start and the last output, so a reply to input sent just before the
wait is still awaited.

## The size a process starts at

Sizes are `COLS ROWS` in character cells everywhere — `--size`, `backmux config
set detach-size`, `backmux resize` — parsed by one function
(`hold_parse_term_size`: two words, or `COLSxROWS`) with one set of bounds
(1 to 1000 each). Zero, negative, non-numeric, incomplete, and out-of-range
sizes are refused before anything is started or saved.

The launcher resolves a terminal process's first size, in this order:

1. `--size COLS ROWS` (recall reuses the recorded explicit size; an explicit
   size on the target shorthand overrides it);
2. for a detached launch (`-dt`, `-dwt`) only, the saved default
   (`backmux config set detach-size`, kept in `~/.config/backmux/config`);
3. the invoking terminal's size, when stdin or stdout is a terminal that
   reports a usable one;
4. 80 columns by 24 rows.

The broker opens the PTY and creates the headless screen at that size before
it forks the program, so a TUI that asks for its dimensions the moment it
starts gets the right answer. `--size` is where the process starts, not a lock:
`backmux resize`, and an attached terminal's window, change it afterwards.
`--size` requires a terminal mode: `call`, `agent`, `detach`, `shell`, a
bare terminal launch, or explicit `-t`. It does not turn a plain `-d` process into a terminal.
A saved default never touches a process that is already
running, and a saved value that no longer reads as a size is not guessed at:
the launch stops and says how to set or clear it.

## Query replies and attached terminals

Programs can query terminal state, such as cursor position or device
attributes. With no attach client, the broker's headless terminal answers.
With an ordinary interactive attach, the person's terminal answers and the
broker discards its own replies to avoid duplicates.

A framed client can send `N` (`CONSOLE_FRAME_NO_TERMINAL`) to request that
the headless terminal continue answering. Backmux sends this for a
noninteractive attach and for compositor panes. A raw client such as
`socat` does not send it; while that client is attached, a program can wait
indefinitely for a terminal reply. Stored broker-chat mode also uses the
headless terminal's replies. A normal interactive reattach repaints the
current screen rather than replaying the entire output log.

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
so the file records the exit. `backmux peek` reads it for ended processes, and
purge removes it with `<id>.altscreen` and `<id>.msgs`.

Full-screen programs draw on the alternate screen and switch back when they
exit, which leaves the normal screen showing none of what they displayed.
When output contains the switch-back (`CSI ?1049l`, `?1047l`, `?47l`), the
broker feeds the screen up to it, keeps a snapshot of that last full-screen
frame, then feeds the rest. `backmux peek --last-alt` asks for it (`peek
alt=1`); at exit it is saved as `<id>.altscreen`. Only the first
switch-back in each read of output is seen, and only while the alternate
screen is active; a sequence split across two reads is missed.

## Messages between processes

New terminal and pipe-input processes receive message text as program
input. The `agent` setting adds a sender and reply-command header. Chat
uses the person's separate inbox identity; see [shell.md](shell.md).
The retained broker-chat mode of older records is a separate path.

`backmux msg` hands text to the *receiver's* broker, which types it in at
once, without waiting for a quiet screen. The receiving harness decides
whether input received while busy is queued, handled immediately, or ignored;
Backmux does not track its tasks. Two things can hold a message
back. A sender whose process has no record yet gets up to 2 s, because a process
that messages the instant it starts can beat its own record, and the
header's name comes from that record. And
messages go in oldest first, one per broker pass, with at most 16 pending.

Message sanitization removes control characters other than newline and
tab, including C0 controls, DEL, and C1 controls. An escape
sequence (CSI, OSC, DCS and the other string forms) is removed whole, not just
its first byte, so `\e[31mRED` arrives as `RED` rather than `[31mRED`. The text goes
in as a bracketed paste when the program enabled mode 2004. Otherwise
newlines and carriage returns become spaces, before delivery.
Enter follows, unless `--no-enter` was given (`submit=0`).

The sender names its process by `BACKMUX_ID`, else `BACKMUX_NAME`; only an id
or a process name is taken (anything else could break the header or a `.msgs`
line). The receiver looks that process up and shows its name as the record has it
now, so a renamed process is shown, and answered, by its new name. When `agent` is enabled, the header precedes the text:

```text
[msg from NAME; reply: backmux msg NAME "..."]
```

A sender outside any process (`BACKMUX_ID` and `BACKMUX_NAME` unset) is given a
name of its own, made like a process's from its harness process (the first
process above backmux that is not a shell or a wrapper), and an inbox under
that name in `<store>/inbox/`. It sends that name with `op=1`; the receiver
shows it in the same header, and `backmux msg NAME` from anywhere appends to
the inbox instead of reaching a broker. `backmux inbox` reads retained
history and marks it read; `inbox --wait` returns one unread message.
`backmux monitor` follows unread and new messages, flushing each to stdout;
`inbox --monitor` remains an equivalent spelling. These readers share an
unread position, rather than independent subscriptions. Use an explicit
inbox name when reading from a different harness or terminal.

`backmux msg ... --wait-reply` records the inbox end before sending, then
waits for the first newer message from any sender. It does not correlate
messages with requests. The name expires after an hour without use;
individual messages expire one hour after delivery. The inbox supplies a
reply destination without requiring a managed input console. If no sender
name can be resolved, agent-formatted delivery uses
`[msg from outside backmux]`.

Inbox identity lookup and creation share the process-name lock, so parallel
first sends use one inbox and an active inbox name cannot also be allocated
to a process in the same store. Existing records are not renamed automatically.

The header provides a reply address, not authenticated proof of sender
identity. Console access checks admit root, the process owner, and the
authorized invoking sudo user. A process with equivalent access can type
the same text through `send`.

Each delivered message is appended to `<id>.msgs` as `ID UNIXTIME FROMCALL
SENDER HEXTEXT` (FROMCALL is 1 when the sender is a managed process).
`backmux msg --received` reads it: the process's message history. Delivery is
capped at 20 messages a minute per receiver, counted when a request is
accepted. This slows message loops; it does not establish task completion
or guarantee a conversation will end. Outside inbox delivery has no such
rate limit.
