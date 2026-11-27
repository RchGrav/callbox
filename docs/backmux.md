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

This reference describes the controls in `src/console/attach.c` and the
compositor, `src/console/mux*.c` (`mux.h` says which file does what). The
terminal reference is `backmux help panes`.

## Command prefix

The default prefix is **Ctrl-Space**. Release it, then press the command key.
The prefix has no timeout. Press Ctrl-Space twice to send one Ctrl-Space to
the active program. An unrecognized command key cancels the sequence without
sending either byte to the program.

| Task | Keys | Result |
| --- | --- | --- |
| Detach pane | Ctrl-Space q | Detach the active pane and leave its process running. Detaching the last pane returns to the invoking terminal. |
| Open or hide chat | Ctrl-Space c | Open chat, focus it if visible, or hide it when it already has keyboard focus. Esc also hides it. |
| New terminal | Ctrl-Space Enter | Start your shell in the active pane's directory, as the next pane (see Placement). |
| Split right / down | Ctrl-Space \| / Ctrl-Space - | Start your shell beside or under the active pane, halving it; the window takes the shape that names it. |
| Open a process | Ctrl-Space l | Open the process launcher: running terminals, consoles that scroll, ended processes. |
| Layouts | Ctrl-Space L | The Layouts panel: load, save, save as, set the default. |
| Next / previous pane | Ctrl-Space o / O | Every pane in order, A to E, window 1 to 9, round again; the view follows. tmux's key. |
| Command help | Ctrl-Space ? | Display the prefix-command reference. |
| Pane Navigation | Ctrl-Space p | Enter pane navigation mode. |
| Window Navigation | Ctrl-Space t | Enter window navigation mode. |
| Pane Resizing | Ctrl-Space r | Enter pane resizing mode. |
| Pane Placement | Ctrl-Space m | Enter pane placement mode. |
| Swap places | Ctrl-Space s | Two places: `2C`, or `C` for the active pane's window. A free place reshapes its window. Works in every view. |
| Maximize pane | Ctrl-Space z | Toggle the active pane between its window layout and full-window display. |
| Bottom bar | Ctrl-Space f | Hide or show the bar along the bottom. |
| Info | Ctrl-Space i | Hide or show every pane's name bar and stats. |
| Rename process | Ctrl-Space n | Prompt for the active pane's process name; renaming also saves its profile. |
| Save profile | Ctrl-Space ; | Prompt for a name and save the active pane's process record. |
| Pane lock | Ctrl-Space \\ | Toggle whether Alt-arrow keys are forwarded to the active program. Prefix commands remain available. |
| Sort (view) | Ctrl-Space a | Show panes by name, in the base's places. Again: the base's order. |
| Spread (view) | Ctrl-Space w | Show panes over every window, as evenly as they go. Again: the base. |
| Pack (view) | Ctrl-Space d | Show panes four to a window from the first. Again: the base. |

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
| Window Navigation | 1–9 | Go to that window (if it has panes) and exit the mode. |
| Window Navigation | Arrow keys or h/j/k/l | Go to the window that way on the 3×3 grid (Left/Right: the previous or next with panes). |
| Pane Resizing | Arrow keys or h/j/k/l | Grow the active pane toward an adjacent split boundary. |
| Pane Resizing | Shift-arrow keys or H/J/K/L | Shrink the active pane from that split boundary. |
| Pane Resizing | = | Reset the current window's split proportions to equal divisions. |
| Pane Placement | Arrow keys or h/j/k/l | Swap the active pane with its neighbor in that direction (base view only). |
| Pane Placement | 1–9 | Move the active pane to that window and exit the mode if space is available (base view only). |

Resizing works on one window; between windows the lines sit halfway. Moving
works in the base view only: a view is computed from the base, so a pane
moved there would have nowhere to stay; the mode says to turn the view off.

Outside these modes, **Alt-arrow keys** focus the adjacent pane, and past
the screen's edge slide the view half a window that way across the grid of
windows (past empty halves), focusing the pane that came in nearest to where
you were. At the grid's edge, or when the active pane is locked, the keys go
to the program. The terminal must encode an Alt-arrow sequence recognized by
Backmux; ordinary Escape-b/Escape-f word movement sequences are not pane
navigation.

## Add and select panes

Press **Ctrl-Space l** (or **Ctrl-Space p a**, or click a counter on the
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

## Placement: the base, places, shapes and the grid

**The base layout is where panes really are.** Every attach, new terminal,
split, swap and move changes it, and nothing else does. What the screen
shows is the base passed through the view (Sort, Spread, Pack). Why one
truth and views over it: turning a view off is always the base again,
exactly as it was, so sorting or spreading can never lose an arrangement.

**Places.** A window has five places: A top left, B right, C bottom left,
D bottom right, E the bottom across. A place is written with its window's
number: `1A` to `9E`. Which places are filled is the window's shape - the
letters name it, nothing else is stored:

| Shape | Screen |
| --- | --- |
| A | One pane fills the window. |
| AB | Left and right. |
| AE | Top and bottom. |
| ABC | A over C on the left, B the whole right. |
| ABD | A the whole left, B over D on the right. |
| ACD | A across the top, C and D below. |
| ABE | A and B on top, E across the bottom. |
| ABCD | Four quarters. |

Why letters for shapes: a place is then a name for a spot that a person, a
script or an agent can say (`backmux swap reviewer,2E`) and mean the same
thing, and one function draws every window. A window holds at most four
panes.

**Where a new pane goes.** An attach (the launcher, Ctrl-Space Enter) goes
into the base where the view says: with Spread on, the window with the
fewest panes (the lowest number on a tie); otherwise the active pane's
window if it has room, else the first with room. The window grows into its
next shape (A, AB, ABC, ABCD; AE grows to ABE). A split halves the active
pane: from A alone, right gives AB and down gives AE; E split right becomes
C and D (ACD); B tall split down gives ABCD. A split with no room says so.
The new pane is always the active one, and the view goes to it.

**Swap.** Two places: panes there trade them; a free place reshapes its
window around the pane sent there, keeping the others where they can and
filling the rest in reading order (A, B, C, D, E). From ABC, sending B to E
makes A|C over B; sending C to E makes A|B over C. A pane leaving lets the
rest keep their places when that is a shape, else close up into the nearest
one. `backmux swap` takes places, names or ids (see below).

**The views.** Sort shows panes by name in the base's places; Spread shows
them over all nine windows; Pack four to a window from the first (Sort and
Spread or Pack combine). Each is a toggle on its key and on the bar, and
none writes to the base.

**The grid.** The nine windows form a 3×3 grid, read in order (1 2 3 / 4 5
6 / 7 8 9), and the screen is a two-quadrant-square view onto it that moves
in half-window steps: on one window, between two side by side, between two
stacked, or where four meet. Why half steps: the neighbor across a window's
edge is never more than half a screen away, and moving keeps what was beside
you in view. Between windows a pane is fitted to what is in view: side by
side only tall panes keep their height, stacked only wide ones keep their
width, where four meet every pane is one quadrant; each goes back to its
size when the view lines up with one window again. Between windows the lines
sit halfway.

**The slide.** When the view moves half a window or a whole one along one
axis, the panes slide over for about 140 ms instead of jumping. Why: a
half-window shift puts what was on the right on the left, and a jump reads as
everything changing at once; the slide shows which way it went and that
nothing was lost. A key pressed mid-slide ends it at once and acts; held
keys move at full speed. It is drawn from the panes' screens as they are (the
programs are not asked anything); over a slow link, turn it off with
`backmux config set slide off` or Slide on the bar.

**Layouts.** A layout keeps the base by name, in
`~/.config/backmux/layouts/NAME`: plain text a person can read and edit -
the view's place, each window's split lines, and each pane's place with its
process id (the name beside it is for reading; a rename does not break it).
Why ids, not commands: each process's record already says how it was
started (program, mode, user, store), so a layout only says where things
go. Saving with a view on saves what is on screen (it becomes the base).
Loading never ends anything: panes not in the layout are let go and run on,
panes already here move, running members are attached (taken from another
terminal, as attach does), ended ones are started again from their records.
Ctrl-Space L opens the Layouts panel; the bar shows the layout's name, with
`*` once the base has changed since it was saved or loaded. `backmux layout
default NAME` (or d in the panel) is what `backmux shell` opens with; its
own shell joins it. See `backmux help layout`.

**One way to name a pane.** `backmux swap` (and the compositor's own
requests) take a place, a process name, or a process id, by the shortest
prefix that is unique. An exact name or id wins; anything that could mean
two things is refused, naming both. Ids start with a letter, so a word
starting with a digit is never an id. Places only mean something on a
particular terminal: run the command inside a pane when several terminals
have compositors; see `backmux help swap`.

## Name bars and prompts

With Info on (Ctrl-Space i, or Info on the bar), each pane has a name bar on
the line above it: its place in the base (`1A` - the same in every view)
and state (LOCKED, ZOOM, the active mode) at the left, its name in a box in the middle, and at the right what it runs -
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
| Minimap | Two braille cells: a dot for each window of the 3×3 grid in view | |
| Window numbers | Windows with panes; those in view highlighted (1, 2 or 4) | Go to that window |
| Layout | The layout loaded (`*`: changed since) | The Layouts panel |
| Sort, Spread, Pack | The view; highlighted while on | Toggle it |
| Slide | Whether the view slides when it moves | Toggle it (kept in the config) |
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
