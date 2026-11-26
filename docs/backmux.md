# Interactive controls

[Docs index](index.md) | [Product model](backmux-identity.md) | [Shell and chat](shell.md) | [Glossary](glossary.md)

Backmux attaches one terminal process initially. Panes are added when you
select another running terminal process in the attach picker. The attach
client composes the panes, help panels, and chat window; each process's
broker continues to maintain its screen and handle control requests.

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
| Open or hide chat | Ctrl-Space c | Open chat, focus it if visible, or hide it when it already has keyboard focus. |
| Command help | Ctrl-Space ? | Display the prefix-command reference. |
| Pane Navigation | Ctrl-Space p | Enter pane navigation mode. |
| Window Navigation | Ctrl-Space t | Enter window navigation mode. |
| Pane Resizing | Ctrl-Space r | Enter pane resizing mode. |
| Pane Placement | Ctrl-Space m | Enter pane placement mode. |
| Swap panes | Ctrl-Space s | Select two pane letters, or a pane letter and destination window number. |
| Maximize pane | Ctrl-Space z | Toggle the active pane between its window layout and full-window display. |
| Pane footers | Ctrl-Space f | Toggle persistent pane footers. |
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
| Pane Navigation | a | Open the attach picker for another running terminal process. |
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

Press **Ctrl-Space p a**, use Up/Down to select a process, then Enter to
attach it. Esc cancels. The picker includes terminal processes with usable
console endpoints, excluding those already present in this compositor.
Selecting a process already attached in another terminal transfers its
console here and disconnects the previous attach client.

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

## Footers and prompts

With more than one pane attached, each pane's footer displays its letter,
process name, and relevant state: active mode, lock, maximized view, unread
chat count, or a status message. Ctrl-Space f toggles persistent footers.
Temporary prompts and status messages can still use the last pane row when
persistent footers are disabled.

Rename and save prompts accept Enter to confirm and Esc to cancel.
`save` protects the process record from ordinary cleanup; it does not save
an entire window layout. See `backmux help save` and `backmux help delete`.

## Chat and display lifetime

Ctrl-Space c opens chat over the active pane and selects its process as the
initial recipient. Chat is associated with the person's inbox identity.
Opening the same chat in another terminal transfers the visible chat window
there. Conversation entries are recorded under the store's `chat/` directory
and replayed when the window opens. Unsent drafts and window geometry are
local to the active compositor; they are not a saved workspace layout.

The compositor displays one floating selection window or chat window at a
time. Opening the attach picker or pane list hides chat. Hiding chat leaves
the managed programs running. See [Shell and chat](shell.md) for message
controls and delivery status.

When only one pane remains and no compositor interaction is active, Backmux
can return to the ordinary attach path. Opening a split from an inline
terminal display switches to the alternate screen; leaving restores the
invoking terminal's screen. Pane arrangements are held in the attach client;
there is no command to save and restore a complete multiwindow workspace.
