# callbox shell: a person's shell, with chat

[Docs index](index.md) | [Repository README](../README.md) | Related: [Console](console.md), [Screens](screen.md)

`callbox shell` opens your shell on a PTY with a floating chat window for
talking to the agents you run under callbox. It is user mode for a shell:
`callbox <cmd>` (or `callbox -it <cmd>`) does the same for any program, and
redial brings that program back - a shell's redial can only bring back the
shell, not what you ran in it. The shell
is an ordinary call: it has a name, a log, and a screen that `callbox peek`
reads. What differs is how its display is produced, and where a message for
it goes. Both are fixed by the launch command — the record carries
`"mode": {"chat": true}` — never guessed from the program's name:

| Launched with | A message (`callbox msg`) for it | Its attach client is sent |
| --- | --- | --- |
| `callbox -dt` or `-it` (any program, shells included) | is typed into the program's prompt at once | the PTY's raw bytes |
| `callbox shell`, and `callbox <cmd>` (no switches) at a terminal (user mode) | goes to the chat inbox; nothing is ever typed | composed frames |

```sh
callbox shell                       # $SHELL, else your login shell, else /bin/sh
callbox shell zsh -l                # callbox's options end at the shell's name
callbox shell --name work zsh -l    # ...so --name is callbox's, -l is zsh's
callbox -dt --name helper -- claude # an agent, placed the ordinary way
```

A shell is something you sit at: `-d`, `-w`, `--tail`, and `--restart` are
refused, and so is starting one without a terminal on stdin and stdout.
`--size`, `--name`, `-e`, `--env-file`, `--rm`, `--detach-keys`, and
`--chat-key` apply. A shell's `--detach-keys` must be exactly two keys (the
lead key, then the detach key), because its broker reads them itself;
`--chat-key` is pressed after the lead key, so it cannot be either of them.
`Ctrl-P Ctrl-Q` detaches; `callbox attach <name>` picks the
shell back up with the chat window, its conversations, and an unsent draft
as they were; `callbox <name>` redials an ended shell as a shell.

## One screen, two layers

There is one terminal emulator: the broker's headless screen (see
[screen.md](screen.md)), fed every byte the PTY produces, whether or not
anyone is attached and whatever covers it.

For an ordinary call the attach client is handed those bytes too, and the
person's own terminal interprets them. For a callbox shell it is not. Each
frame the broker (`src/console/compose.c`):

1. takes a snapshot of the shell's screen — the **base layer**, clipped or
   padded to the attached terminal;
2. draws the chat window (or, when it is hidden, the corner badge) over it —
   the second pass;
3. writes to the attach client only the cells that differ from what that
   terminal already shows, then the cursor.

The compositor is the only writer to that terminal, so a redraw by the shell
cannot paint over the window. Because every frame starts again from the live
screen, moving, resizing, or hiding the window uncovers what the shell shows
*now*, not what it showed when chat opened. Frames are paced (at most one
every 12 ms; output that arrives faster is coalesced), wrapped in a
synchronized update where the terminal supports it, and a newly attached or
resized terminal is cleared and repainted whole.

The window exists only in those frames. It is never fed to the screen, so
`callbox peek`, `callbox wait --match`, the saved final screen, and the log never
contain it, and nothing of it reaches the shell's input.

What the person's terminal is told besides cells, so that it behaves as it
would have with the raw bytes:

- **Colors.** A cell colored from the palette is sent by its index (SGR
  30–37 and 90–97 for the 16 basic colors, `38;5;N` for the rest; likewise
  for backgrounds), so it is drawn in the terminal's own palette; true colors
  are sent as RGB; default colors as default.
- **Input modes.** Application cursor keys and keypad, bracketed paste, and
  focus reporting follow the shell's program while the shell has the
  keyboard. While chat has it, bracketed paste is on (a paste arrives whole,
  as text) and the others are off.
- **Mouse reporting** follows the program — and is at least press/release/drag
  while the chat window is up, so the window can be dragged. Reports are
  always asked for in SGR form and re-encoded for the program in whatever
  protocol it chose.
- **The cursor**: the shell's, at the shell's position and in the style its
  program chose — or chat's, while chat has the keyboard. It is hidden while
  the window covers it.
- The window **title** and the **bell**.

The program's terminal queries (cursor position, device attributes) are
always answered by the headless screen: the person's terminal never sees
them.

Lost in translation, as in any terminal multiplexer: the terminal's own
scrollback (the display is the alternate screen, as with `callbox -it`), and
extensions the headless terminal does not model (inline images, hyperlinks,
the kitty keyboard protocol). `callbox logs` has everything the shell printed.

## Size

The shell's PTY is the size of the attached terminal's window. A real window
resize travels the normal path: the attach client reports it, the broker
sets the PTY's size (the kernel signals the foreground program), the screen
reflows, the base is redrawn, and the chat window is refitted inside the new
bounds — smaller if it must be, back to the size you gave it when there is
room again. Its title bar and close control always stay on the terminal.

Opening, closing, moving, or resizing the chat window touches none of that:
it is geometry in the compositor, in terminal cells, and the PTY's size and
the program are left alone.

`callbox resize <shell> C R` (an agent's resize) changes the PTY only; the
display shows the screen clipped or padded until the window is next resized.

## Keyboard and mouse

Everything the attached terminal sends passes through one router
(`chat_input` in `src/console/chat.c`).

- **The lead key** (`Ctrl-P`, the first of the detach keys) is the only key
  callbox takes, and it only waits: the next key says what it meant, with no
  time limit (not everyone types fast, and Docker's lead key is untimed
  too; `Ctrl-P Ctrl-P` is the quick way to send one through), and the corner (or the window, when it is up) shows what it
  offers meanwhile. `Ctrl-Q` detaches. `Ctrl-T` (`--chat-key ctrl-g` picks
  another) opens chat, or moves the keyboard between chat and the shell
  while the window stays up. While the shell has the keyboard, `Ctrl-P`
  again sends one `Ctrl-P` to the shell, and any other key sends both
  through, in order. While chat has it, the lead key is dropped and the
  other key goes to chat. A mouse report is not an
  answer: it is dealt with and the lead key goes on waiting. The broker
  reads these keys itself (the attach client passes every key through), so
  it can draw the menu. `Esc` hides the window while chat has the keyboard;
  hiding or leaving chat gives the keyboard, and the cursor, back to the
  shell.
- **On-screen help.** What just happened and the key that undoes it are
  shown where the eye is, for a few seconds: `callbox shell | Ctrl-P Ctrl-T:
  chat` on attach, `chat hidden | Ctrl-P Ctrl-T: bring it back` when the
  window closes, `Ctrl-P Ctrl-T: chat | typing in the shell` in the window
  when the keyboard is handed over. Like the window, none of it reaches the
  shell, its screen, or its log.
- **While the shell has the keyboard**, its bytes pass as typed. With no
  mouse reporting to pick out they are not parsed or delayed at all.
- **While chat has the keyboard**, nothing typed reaches the shell: text
  edits the draft, `Enter` sends, `Alt-Enter` (or `Ctrl-J`) starts a new
  line, `Tab` chooses who to talk to, `Ctrl-S` toggles *Include screen*,
  `PgUp`/`PgDn` and `Up`/`Down` scroll the conversation. A lone `Esc` is
  told from the start of an escape sequence by what follows within 40 ms.
- **The mouse.** Drag the title bar to move the window, the lower-right
  corner (`◢`) to resize it, click the `×` to close it; the wheel scrolls
  the conversation. While the window is hidden, clicking the corner badge
  opens it. A gesture belongs to whatever it started on until the
  button is released: a drag begun on the window stays the window's when
  the pointer leaves it, and one begun on the shell stays the shell's when
  it crosses the window. Outside the window, events go to the shell's
  program only if its mouse mode asks for them — otherwise they are
  dropped — and a press there also returns the keyboard to the shell.
  Clicks and drags on the window never reach the program.

While the window is up, the terminal's own text selection needs its usual
override (often Shift), as in any program that takes the mouse; hidden, the
terminal's mouse is the terminal's again unless the shell's program wants
it.

## Messages

Chat uses `callbox msg`'s transport; there is no second one.

**Out.** `Enter` sends the draft to the selected call as a `msg` request
whose sender is the shell's own call id. The request returns once the
receiver has the text: at once, apart from up to 2 s while a just-launched
sender's record is written and up to 10 s of retries while the receiver's
console is busy. A send with no answer after 90 s is abandoned. The broker
forks a short-lived helper per send (at most 8 at a time) and stays with the
display, so the window never freezes. The helper names the shell's call as
the sender, so the agent sees an ordinary header —

```text
[msg from work; reply: callbox msg work "..."] can you look at the build?
```

— and answers with an ordinary `callbox msg work "..."`. It needs to know
nothing about the chat window. A send is shown as `sending...` until the
receiver confirms it, then `delivered`. If the call is gone or not running,
its console cannot be reached or does not answer within 90 s, it refuses the
message (rate limit, too large), or too many sends are in flight, the
message is marked `NOT DELIVERED` with the reason, and never shown as
delivered. The `outside callbox` conversation cannot be replied to: there
is no call to send to.

**In.** A message for the shell is recorded like any other (so
`callbox msg --received` works inside the shell) and filed in the chat inbox
under the call that sent it:

- from a call: that call's conversation, keyed by its id, so it follows a
  rename;
- sent from outside any call: `outside callbox`.

The
selected conversation stays selected for follow-ups; `Tab` lists the calls
that can be messaged (running, on a PTY) and every conversation, with what
is unread. While the window is hidden a message only raises the corner
badge (`chat: 2 new from helper`): the window does not open and the shell
is not interrupted. Hiding the window clears nothing and ends nothing.

**Include screen** (`Ctrl-S`, or click `[ ] screen`) appends the shell's
clean screen — the rows `callbox peek` prints, so never the chat window — to
the next message only; it switches itself off after one send.

Sending a message grants nothing. The agent is not given control of the
shell, the shell's input is never written by a message, and whatever could
already reach the shell (its owner's `callbox send`, for one) is unchanged.

Conversations live in the broker and last as long as the shell does; they
are not written to disk. At most 24 conversations of 300 messages each are
kept. Within a conversation the oldest message gives way. A new
conversation replaces the one left alone longest, never the one on show
or one with a send in flight; if every one is on show or sending, the
incoming message is not shown (`callbox msg --received` still has it).

## Layers

| Code | Role |
| --- | --- |
| `src/console/compose.c` | Frames: base layer from a snapshot, overlay drawing, the cell diff, terminal modes. |
| `src/console/chat.c` | The window, conversations, the input router, the send helper. |
| `src/console/broker.c` | The `b->chat` branches: frames instead of raw bytes, the inbox instead of typing, naming the sender. |
| `src/console/overlay.h` | The two modules' interface, private to the console layer. |
