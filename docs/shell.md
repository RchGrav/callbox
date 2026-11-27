# Shell and chat

[Docs index](index.md) | [Interactive controls](backmux.md) | [Console](console.md) | [Screens](screen.md)

`backmux shell` runs a shell as an attached terminal process. Chat and pane
controls are available when attached to terminal processes, including shells,
editors, and agent harnesses. The chat window belongs to the person's inbox
identity; messages addressed to the shell process itself are typed into the
shell behind their sender header, as for any other process.

```sh
backmux shell                       # $SHELL, login shell, or /bin/sh
backmux shell --name work zsh -l    # Backmux options precede the shell name
```

A shell opens on a cleared screen of its own - the alternate screen, so the
person's lines from before come back when they leave - with the bar along the
bottom, the shell's name bar, and chat up but the keyboard on the shell
([Interactive controls](backmux.md)). Every other launch starts inline,
where the cursor is. Why: the shell is the person's own screen from here,
and chat is what they open it to reach; it is the one launch meant for a
person rather than a script.

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

Start a detached agent harness from another command prompt:

```sh
backmux detach --name helper -- claude
```

This example requires an installed, configured `claude` executable. Inspect
its screen for setup prompts before sending a task.

Chat lists agent harnesses only, each with its harness's name beside it
(`helper  claude`). Why: chat is for talking to agents; a shell, an editor
or a server on the list was one more name to skip past. A process is an
agent when the program it was started as, or the program in its terminal's
foreground now (a harness started from a shell), is one: `claude`, `codex`,
`agy`, `antigravity`, `pi`, `hermes`, `crush`, `opencode`, `gemini`,
`aider`, `goose`, `copilot`, or a name in the config's `agents`. A program
is known by its file name without an extension; a script run by an
interpreter (node, python, bun, npx, uv) by its script's name or a directory
of its path (`node_modules/@github/copilot/index.js` is `copilot`); a known
name also matches at the start of a longer one before `-` or `_`
(`codex-x86_64-linux`, `gemini-cli`). Launch anything else with `--agent`
and its program's name joins the list for good:

```sh
backmux --agent -dt --name helper -- my-harness   # my-harness is an agent from now on
backmux config get agents
```

A conversation with something else that has unread messages is listed too,
so a message is never hidden. Senders outside any process (an agent's
inbox) are listed as agents.

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
| Hide chat | Esc, Ctrl-Space c, the close control, or your name on the bar |
| Choose a recipient | Tab; type to filter, then select a result |
| Send the draft | Enter |
| Insert a newline | Alt-Enter or Ctrl-J |
| Ask for help with the app on screen (Assist) | Ctrl-S, or the Assist checkbox |
| Scroll the conversation | Up/Down, PageUp/PageDown, or the mouse wheel |
| Move the window | Drag the title bar |
| Resize the window | Drag the lower-right corner |
| Return focus to a pane | Click outside the chat window in that pane |

Esc hides chat. Why: chat must work from the keyboard alone, and Esc is the
key every window closes with. The prefix command remains available while
editing a draft. Hiding the window retains the draft while that compositor
is active. Opening another floating window hides chat.

Assist asks the agent for help with the app on screen - the pane last in
focus that is not an agent. Ticked (for one send), the message is your text,
then:

```text
The user is seeking assistance with the application running in Backmux
session "APP".

Immediately run `backmux help tui` to familiarize yourself with the
terminal inspection and control interface.

Capture and examine a screenshot of the application using:

    backmux peek APP --image /tmp/backmux-APP.png --cursor-ring

Use the Backmux interface to inspect and interact with the application
as needed to assist the user.

Reply to the user using:

    backmux msg CHAT_NAME "Your response"
```

With the box ticked and nothing typed, the request alone is sent. Why not
the screen's text, as the box sent before: text pasted into a prompt is
stale the moment it lands and loses colors and the cursor, while an agent
told how to peek sees the app live, as an image, and can drive it.

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
