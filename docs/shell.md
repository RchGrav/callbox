# Shell and chat

[Docs index](index.md) | [Interactive controls](backmux.md) | [Console](console.md) | [Screens](screen.md)

`backmux shell` runs a shell as an attached terminal process. Chat and pane
controls are available when attached to terminal processes, including shells,
editors, and agent harnesses. The chat window belongs to the person's inbox
identity; messages addressed to the shell process itself are typed into the
shell, as for other processes without agent formatting.

```sh
backmux shell                       # $SHELL, login shell, or /bin/sh
backmux shell --name work zsh -l    # Backmux options precede the shell name
```

The default shell selection checks for an executable absolute path in
`$SHELL`, then the account's login shell, then `/bin/sh`. An explicit shell
name is resolved like any other program. Words after that name are shell
arguments.

The shell command accepts `--name`, `--size`, `-e`, `--env-file`, `--rm`,
`--detach-keys`, and `--quiet`. It rejects `-d`, `-w`, `--tail`, and
`--restart`. `--chat-key` is unsupported; the chat command is prefix c.
The shell command requires a terminal on both stdin and stdout. For a
shell controlled from a script, use `backmux detach -- sh` instead.

## Start a conversation

Start a detached agent harness from another command prompt, then enable
sender and reply-command headers:

```sh
backmux detach --name helper -- claude
backmux set helper agent
```

This example requires an installed, configured `claude` executable. Inspect
its screen for setup prompts before sending a task.

While attached to your shell, press **Ctrl-Space c**, then **Tab** to select
`helper`. Type a message and press Enter. The receiver sees a header with
your chat inbox name and the reply command:

```text
[msg from CHAT_NAME; reply: backmux msg CHAT_NAME "..."] Review the build output.
```

The recipient replies to `CHAT_NAME`, not the shell's process name. Its
reply appears in the chat conversation. `backmux msg work TEXT`, by
contrast, addresses the managed shell process named `work` and delivers
input to that process.

`backmux config set chat-name NAME` sets the preferred name used when a
chat identity is created. It does not rename an existing live inbox. The
current chat name is displayed in the window.

## Chat controls

| Task | Control |
| --- | --- |
| Open chat or give it keyboard focus | Ctrl-Space c |
| Hide focused chat | Ctrl-Space c, or click the close control |
| Choose a recipient | Tab; type to filter, then select a result |
| Send the draft | Enter |
| Insert a newline | Alt-Enter or Ctrl-J |
| Include the active pane's screen in the next message | Ctrl-S, or the screen checkbox |
| Scroll the conversation | Up/Down, PageUp/PageDown, or the mouse wheel |
| Move the window | Drag the title bar |
| Resize the window | Drag the lower-right corner |
| Return focus to a pane | Click outside the chat window in that pane |

Esc does not hide compositor chat. The prefix command remains available
while editing a draft. Hiding the window retains the draft while that
compositor is active. Opening another floating picker hides chat.

The screen attachment contains the active pane's plain-text screen and
cursor coordinates, labeled with its process name. It is enabled for one
send, then cleared. It contains neither the chat window nor other panes.

## Delivery and history

The chat window uses the same `msg` and inbox mechanisms as CLI messaging.
A process with the `agent` setting receives the sender/reply header; without
it, only the message text is typed, followed by Enter unless submission is
disabled. Message sanitization and bracketed-paste behavior are described in
[Screens](screen.md#messages-between-processes).

Sending to a process uses a short-lived helper so the display can continue
updating. The window distinguishes a pending send, confirmed delivery, and a
failed delivery with its reason. A delivery confirmation means Backmux
accepted the text for the recipient; it does not establish that an agent
processed it or completed a task. A reply to an outside sender is appended
to that sender's inbox.

The compositor checks its inbox and active-chat ownership every 250 ms.
Opening the same chat identity in another terminal hides the window in the
previous one. Sent and received conversation entries are appended to
`<store>/chat/<name>.log` and replayed when chat first opens in a compositor.
The log is separate from each program's output and received-message log.

The in-memory view retains up to 24 conversations with up to 300 messages
each. Older entries can leave that view as its limits are reached. Drafts,
window geometry, and pane arrangements are not persisted by the conversation
log. Reattaching a process does not restore an entire workspace.

## Display and input

Each terminal process's broker feeds its PTY output to a headless terminal
and appends it to the program log. An ordinary attach relays the terminal
stream. When pane or chat controls activate the compositor, it mirrors each
attached process into a local screen, lays out the panes, and draws the chat
window and help panels over them. `compose.c` emits changed cells and the
appropriate terminal modes and cursor.

These overlays exist only in the person's display. `peek`, screen-based
`wait` conditions, saved program screens, and program logs do not contain
chat or compositor help. Typing while chat has focus edits the draft;
typing while a pane has focus goes to that pane's program.

Pointer input is routed according to the pane and floating window under it.
Dragging the chat window changes only its geometry. Resizing the terminal
or changing a pane's layout resizes the corresponding PTY and screen;
the terminal driver sends SIGWINCH to the foreground process group.

The compositor uses Backmux's modeled terminal state. Terminal extensions
outside that model, such as inline images, are not reproduced. Raw output
remains available in `backmux logs`.

## Implementation map

| Source | Responsibility |
| --- | --- |
| `src/cli/main.c` | Shell selection, launch parsing, and supported options. |
| `src/console/attach.c` | Ordinary attachment and transfer to the compositor. |
| `src/console/mux.c` | Panes, windows, modes, chat ownership, inbox polling, and conversation log replay. |
| `src/console/chat.c` | Chat drawing, draft editing, recipient selection, and send helpers. |
| `src/console/compose.c` | Cell composition, terminal modes, and screen updates. |
| `src/runtime/settings.c` | Preparing the chat identity and process settings. |
| `src/runtime/inbox.c`, `src/store/inbox.c` | Inbox identity, delivery, and unread progress. |

The broker still contains support for stored `mode.chat` records from an
earlier launch model. Current launch commands do not enable that mode. Its
retained code is not the basis for the controls described on this page.
