# Interactive controls

[Docs index](index.md) | [Product model](backmux-identity.md) | [Shell and chat](shell.md) | [Glossary](glossary.md)

Backmux attaches one terminal process initially. Panes are added when you
select another running terminal process in the process launcher. The attach
client composes the panes, name bars, the bar along the bottom, help panels,
and the chat window; each process's broker continues to maintain its screen
and handle control requests.

`backmux shell` is the exception to "one process, plain attach": it opens on
a cleared screen of its own (the alternate screen, so the lines from before
come back on leaving), with the bar, the shell's name bar, and chat up with
the keyboard left on the shell. Reattaching to a shell brings that back.
Why: the shell is the one launch meant for a person rather than a script,
and the bar is how the mouse reaches everything the keys do.

This reference describes the controls in `src/console/attach.c` and
`src/console/mux.c`. The terminal reference is `backmux help panes`.

## Command prefix

The default prefix is **Ctrl-Space**. Release it, then press the command key.
The prefix has no timeout. Press Ctrl-Space twice to send one Ctrl-Space to
the active program. An unrecognized command key cancels the sequence without
sending either byte to the program.

| Task | Keys | Result |
| --- | --- | --- |
| Detach pane | Ctrl-Space q | Detach the active pane and leave its process running. Detaching the last pane returns to the invoking terminal. |
| Open or hide chat | Ctrl-Space c | Open chat, focus it if visible, or hide it when it already has keyboard focus. Esc also hides it. |
| Open a process | Ctrl-Space o | Open the process launcher: running terminals, consoles that scroll, ended processes. |
| Command help | Ctrl-Space ? | Display the prefix-command reference. |
| Pane Navigation | Ctrl-Space p | Enter pane navigation mode. |
| Window Navigation | Ctrl-Space t | Enter window navigation mode. |
| Pane Resizing | Ctrl-Space r | Enter pane resizing mode. |
| Pane Placement | Ctrl-Space m | Enter pane placement mode. |
| Swap panes | Ctrl-Space s | Select two pane letters, or a pane letter and destination window number. |
| Maximize pane | Ctrl-Space z | Toggle the active pane between its window layout and full-window display. |
| Bottom bar | Ctrl-Space f | Hide or show the bar along the bottom. |
| Info | Ctrl-Space i | Hide or show every pane's name bar and stats. |
| Rename process | Ctrl-Space n | Prompt for the active pane's process name; renaming also saves its profile. |
| Save profile | Ctrl-Space ; | Prompt for a name and save the active pane's process record. |
| Pane lock | Ctrl-Space \\ | Toggle whether Alt-arrow keys are forwarded to the active program. Prefix commands remain available. |
| Spread panes | Ctrl-Space w | Distribute panes across the available windows. |
| Sort panes | Ctrl-Space a | Sort panes by process name within the occupied slots. |
| Pack panes | Ctrl-Space d | Fill windows from the first, with up to four panes per window. |

`--detach-keys` changes the detach sequence. A two-key sequence also supplies
the compositor's prefix and detach key. Other compositor command keys retain
their listed meanings.

## Navigation and layout modes

Press `?` in a mode for contextual help. Esc or Enter exits the mode; when
mode help is visible, Esc closes the help first. A mode retains keyboard
input until it exits, so repeated navigation or resizing needs no prefix.

| Mode | Control | Operation |
| --- | --- | --- |
| Pane Navigation | Arrow keys or h/j/k/l | Focus the adjacent pane in that direction. |
| Pane Navigation | a | Open the launcher on its list of running terminal processes. |
| Pane Navigation | g | Open the pane list and select a pane by process name. |
| Pane Navigation | z | Toggle the active pane's maximized view. |
| Window Navigation | 1–9 | Select a nonempty window and exit the mode. |
| Window Navigation | Left/Right or h/l | Select the previous or next nonempty window. |
| Pane Resizing | Arrow keys or h/j/k/l | Grow the active pane toward an adjacent split boundary. |
| Pane Resizing | Shift-arrow keys or H/J/K/L | Shrink the active pane from that split boundary. |
| Pane Resizing | = | Reset the current window's split proportions to equal divisions. |
| Pane Placement | Arrow keys or h/j/k/l | Swap the active pane with its neighbor in that direction. |
| Pane Placement | 1–9 | Move the active pane to that window and exit the mode if space is available. |

Outside these modes, **Alt-arrow keys** focus an adjacent pane when one
exists and the active pane is unlocked. Otherwise the key sequence is
forwarded to the active program. The terminal must encode an Alt-arrow
sequence recognized by Backmux; ordinary Escape-b/Escape-f word movement
sequences are not pane navigation.

## Add and select panes

Press **Ctrl-Space o** (or **Ctrl-Space p a**, or click a counter on the
bar) to open the process launcher. Tab or Left/Right steps through its three
lists, Up/Down selects, Enter acts, Esc cancels:

| List | Holds | Enter |
| --- | --- | --- |
| Terminals | Running terminal processes with usable console endpoints, except those already panes here | Attach it as a pane. Selecting a process attached in another terminal transfers its console here. |
| Consoles | Running processes whose output scrolls: programs on pipes (`-i`) and servers | Open it in a viewer pane: a terminal process running `backmux attach` (pipes: its input too) or `backmux logs -f` (a server), named `NAME-view` or `NAME-log`, started with `--rm` so it leaves no record once closed. |
| Ended | Processes that have ended | Start it again in the background (`-dt`, `-di` or `-d`, as it first ran); a terminal process becomes a pane. A shell comes back with `backmux NAME` instead. |

Why three lists in one window: what can be shown on this screen is a
running terminal, a console that scrolls, or something to start again, and
the bar's three counters say how many of each there are.

Clicking a pane focuses it. Ctrl-Space p g opens a list of the panes already
attached; Up/Down and Enter select one, and Esc cancels.

Each window holds up to four panes. Placement is fixed:

| Pane count | Layout |
| --- | --- |
| 1 | One pane fills the window. |
| 2 | Left and right columns. |
| 3 | The left column splits into upper and lower panes; the right remains full height. |
| 4 | Both columns split into upper and lower panes. |

New panes join the current window if it has room, then the first window
with space. There are nine windows, numbered 1–9. Pane letters identify
slots across windows: A–D in window 1, E–H in window 2, continuing through Z.
Slots after Z have no letter; window numbers still select their destinations.

The interactive swap command uses these letters. The CLI command
`backmux swap NAME1,NAME2` uses process names or ID prefixes of at least
four characters. Both processes must already have panes in the selected
terminal. Run the command inside a pane when several terminals have active
compositors; see `backmux help swap`.

## Name bars and prompts

With Info on (Ctrl-Space i, or Info on the bar), each pane has a name bar on
the line above it: its letter and state (LOCKED, ZOOM, the active mode) at
the left, its name in a box in the middle, and at the right what it runs -
the agent harness, or the program - with its CPU and memory (its terminal's
foreground job: a harness started from a shell is what is busy, not the
shell). The bar sits on the split line where there is one, so it costs a row
only at the top of the screen. Why: the name is what `msg`, `attach` and every
agent take, and status belongs in one steady place per pane; nothing else
puts names on the screen (no names on attach, focus or click).

Click the name box, or press Ctrl-Space n, to type a new name over it: the
first key replaces the name, Backspace edits it, Enter renames (renaming also
saves the profile), and Esc or a click anywhere else keeps the name as it
was. Ctrl-Space ; asks the same way and saves. `save` protects the process
record from ordinary cleanup; it does not save an entire window layout. See
`backmux help save` and `backmux help delete`.

Name bars and the bar show while the screen is the compositor's for good -
`backmux shell`, or more than one pane - never while chat or a list is up for
a moment over one program. Why: a row taken from that program and given
back would make it redraw twice for nothing.

## The bar

The bar along the bottom (Ctrl-Space f hides it) is the mouse's way to what
the keys do. From the left:

| Item | Shows | Click |
| --- | --- | --- |
| `[N]` green | Running terminal processes | The launcher's Terminals list |
| `[N]` blue | Running consoles that scroll (`-i`, servers) | The launcher's Consoles list |
| `[N]` red | Ended processes | The launcher's Ended list |
| Your chat name | Chat: highlighted while open, red with a count while messages wait | Open or put away chat |
| Info | Highlighted while name bars are on | Name bars on or off |
| Mode, notices | The prefix waiting, a mode, the last notice | Mode: the keys |
| Window numbers | Windows with panes; the current one highlighted | Go to that window |
| Zoom, Lock | Highlighted while on, for the active pane | Toggle |
| `?` | | The keys |
| CPU, MEM, time | The machine's load and memory | |

Toggles have two looks, on and off. When the terminal is narrow, items take
short forms (the least useful first), then the least useful go (the clock,
Lock, Zoom, the machine's load), so what is left is always whole.

## Chat and display lifetime

Ctrl-Space c opens chat over the panes; when the active pane is an agent
harness, its conversation is the one shown. Chat lists agent harnesses only,
with each harness's name beside the process's (`reviewer  codex`). Esc hides
chat, as the prefix then c and its close button do. Chat is associated with
the person's inbox identity. See [Shell and chat](shell.md) for how a
harness is told and for Assist.
Opening the same chat in another terminal transfers the visible chat window
there. Conversation entries are recorded under the store's `chat/` directory
and replayed when the window opens. Unsent drafts and window geometry are
local to the active compositor; they are not a saved workspace layout.

The compositor displays one floating selection window or chat window at a
time. Opening the launcher or pane list hides chat. Hiding chat leaves
the managed programs running. See [Shell and chat](shell.md) for message
controls and delivery status.

When only one pane remains and no compositor interaction is active, Backmux
returns to the ordinary attach path - except in `backmux shell`, which keeps
its bar. Opening a split from an inline
terminal display switches to the alternate screen; leaving restores the
invoking terminal's screen. Pane arrangements are held in the attach client;
there is no command to save and restore a complete multiwindow workspace.
