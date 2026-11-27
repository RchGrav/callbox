# Backmux terminology

Use these terms consistently in first-party product language. The definitions
describe the implementation; identifiers and required command syntax retain
their exact spelling. Source references identify where to verify each concept.

## Processes and records

| Term | Meaning and usage | Implementation evidence |
| --- | --- | --- |
| **Backmux** | Product name in prose. Use `backmux` for the executable. | `src/cli/main.c`, `Makefile` |
| **program** | Executable launched by Backmux, with its arguments. A program is not a pane or a saved profile. | `src/term/spawn.c`, `src/runtime/start.c` |
| **managed process** / **process** | A program launched and tracked by Backmux. Lifecycle operations may apply to its process group. Use **process record** when referring to retained data after execution ends. | `src/runtime/start.c`, `src/runtime/signal.c`, `include/backmux/types.h` |
| **process group** | Operating-system group identified by a PGID; the target of group signaling. | `src/runtime/signal.c`, `src/term/spawn.c` |
| **session** | Operating-system session identified by an SID when discussing process ownership or liveness. Do not use it as an interchangeable name for a pane, window, or profile. | `src/platform/process.c`, `src/runtime/state.c` |
| **PID** | Operating-system process identifier. It is distinct from a Backmux process ID. | `include/backmux/types.h`, `src/runtime/state.c` |
| **Backmux process ID** / **ID** | Identifier of the stored process record. Recall reuses it; a new launch gets a new ID. Use the qualified form where confusion with a PID is possible. | `src/store/lifecycle.c`, `src/runtime/call.c` |
| **name** | Human-readable process name, generated or explicitly assigned. Renaming updates the record's name and saves it. | `src/runtime/start.c`, `src/runtime/call.c` |
| **target** | Command argument identifying a managed process by name or ID, or an inbox where supported. Scope and prefix forms depend on the command. Use `backmux help targets` for exact syntax. | `src/runtime/resolve.c`, `src/store/resolve.c`, `src/runtime/inbox.c` |
| **process record** | Stored metadata for a managed process, including identity, launch recipe, and lifecycle information. Records can outlive the process. | `include/backmux/types.h`, `src/store/record.c`, `src/store/record_read.c` |
| **launch recipe** | Stored program, arguments, environment overrides, working directory, and launch options used by recall. `recipe` remains the internal field name. | `struct hold_recipe` in `include/backmux/types.h`, `src/runtime/call.c` |
| **profile** / **saved profile** | A process record and launch recipe marked `saved`, protecting it from ordinary purge operations. Explicit `delete` or targeted `purge --force` can remove it. It is not a separate file format or a saved multi-pane layout. | `src/runtime/call.c`, `src/runtime/list.c`, `src/runtime/settings.c` |
| **working directory** | Directory in which a program starts. The `working` setting changes the next launch; clearing it uses the directory of the recall invocation. | `src/runtime/settings.c`, `src/runtime/call.c` |
| **environment override** | Assignment supplied through `-e KEY=VALUE` or `--env-file` and stored for subsequent launches. `info` reports override names; `inspect` includes stored values. | `src/cli/main.c`, `src/runtime/settings.c`, `src/runtime/observe.c` |

`call` is the launch command. Use its exact spelling in commands and when
discussing an existing identifier; use **process** or **process record** for the
object in explanatory prose. Preserve internal identifiers such as
`hold_cmd_redial`; describe the public `recall` operation separately from the
bare-target reconnect-or-restart behavior.

## Operations and states

| Term | Meaning and usage | Implementation evidence |
| --- | --- | --- |
| **launch** | Start a program under Backmux and establish its tracking and I/O resources. | `src/runtime/start.c` |
| **recall** | Restart from the stored recipe using the existing process ID and log. The `recall` command refuses a target confirmed running; bare `backmux NAME` instead attaches to or follows a running target. | `src/runtime/settings.c`, `src/runtime/call.c` |
| **attach** | Connect the current terminal to a process console. A newer attach replaces the previous client. | `src/console/attach.c`, `src/console/broker.c` |
| **detach** | Disconnect the terminal while leaving the process running. In pane controls it removes the active pane. The `detach` launch command starts a detached terminal process. | `src/console/mux.c`, `src/console/attach.c`, `src/cli/main.c` |
| **follow** | Continue reading output as it is appended to a log. It does not provide interactive program input. | `src/runtime/observe.c`, `src/console/client.c` |
| **end** | Send SIGTERM, wait up to five seconds, then send SIGKILL if the validated process group remains running. `stop` is an alias. | `src/runtime/signal.c`, `src/cli/specs.c` |
| **kill** | Send SIGKILL to the target process group. | `src/runtime/signal.c` |
| **save** | Mark the process record as saved. | `src/runtime/call.c` |
| **delete** | Remove a process record and associated artifacts, including a saved profile. Its initial check refuses a target confirmed running; deletion then delegates to targeted forced purge. | `src/runtime/settings.c`, `src/runtime/list.c` |
| **purge** | Remove eligible process records and associated artifacts. `purge all` also ends unsaved processes; explicit targets and `--force` have separate rules documented in command help. | `src/runtime/list.c`, `src/runtime/settings.c` |
| **running** | Liveness checks find a matching process/group, or the supported cross-namespace supervision evidence. | `src/runtime/state.c` |
| **exited** | Liveness or recorded completion evidence shows execution ended. An exit code is available only when recorded. | `src/runtime/state.c`, `src/store/lifecycle.c` |
| **stale** | Recorded identity no longer matches the current boot or observed process identity. | `src/runtime/state.c` |
| **failed** | Record identifies a launch failure. Do not use this state to mean every nonzero program exit. | `src/runtime/state.c` |
| **unknown** | Available evidence is insufficient to determine the process state. | `src/runtime/state.c` |

Keep literal state values and machine-readable fields unchanged. Distinguish
**console closed** from **process exited**: a program can close its terminal
output and continue running.

The initial `recall` and `delete` checks reject the **running** state, not every
uncertain state. An **unknown** target can reach the underlying relaunch or
purge operation, where further identity and liveness checks apply. The forced
purge used by `delete` can stop a target found running on that later check.

## Terminals, panes, and controls

| Term | Meaning and usage | Implementation evidence |
| --- | --- | --- |
| **terminal process** | Managed process connected to a pseudoterminal (PTY). It can appear in a pane. | `src/runtime/start.c`, `src/console/mux.c` |
| **PTY** / **pseudoterminal** | Operating-system terminal device pair used for terminal input, output, size, and modes. Explain the abbreviation in introductory material. | `src/term/spawn.c`, `src/term/tty.c` |
| **pipe-input process** | Process launched with `-i` without `-t`; on the non-restart path, a broker relays input/output through pipes. It is not a terminal process. | `src/runtime/start.c`, `src/console/broker.c` |
| **console** | Connection providing a managed process's input and output through its broker. The endpoint may be a Unix-domain socket or FIFO. | `src/console/broker.c`, `src/console/socket.c`, `src/console/fifo.c` |
| **broker** | Process that owns the program's terminal or pipes, maintains its screen model and log, and serves attach/control clients. | `src/console/broker.c` |
| **pane** | Region displaying an attached terminal process. A pane is a view, not the process or its profile. | `src/console/mux.c` |
| **active pane** | Selected pane receiving forwarded program input. The chat window or a control mode may temporarily own keyboard focus. | `focused`, `key_in`, and `set_focus` in `src/console/mux.c` |
| **window** | Numbered group of up to four panes. There are nine window positions, selected with keys 1–9. Use **window**, not **tab**, for this object; `MODE_TAB` remains an internal identifier. | `MUX_MAX_WINDOWS`, `place`, and `window_of_key` in `src/console/mux.c` |
| **pane slot** | Position within the compositor. Letters span windows: A–D in window 1, E–H in window 2, through Z. Later slots have no letter. | `slot_letter` in `src/console/mux.c` |
| **name bar** | Line above a pane (Info): its letter and state, its name in a box (click to rename), and what it runs with its CPU and memory. | `draw_name_bar` in `src/console/mux.c` |
| **bar** | Row along the bottom of the compositor's screen: process counters, the person's chat name, Info, windows, toggles, the machine's load. Ctrl-Space f hides it. | `draw_bar` in `src/console/mux.c` |
| **launcher** | Window listing running terminals, consoles that scroll, and ended processes, to attach, view, or start again. | `draw_procs` and `launch_pick` in `src/console/mux.c` |
| **agent harness** | A program chat talks to: a known harness (claude, codex, ...) or one named with `--agent`. | `src/console/agents.c` |
| **Assist** | Chat's checkbox: the next message asks the agent to inspect the app on screen with `peek` and answer with `msg`. | `with_assist` in `src/console/chat.c` |
| **full screen** | Pane display filling its current window. Use **full-screen** before a noun, as in “full-screen program.” | `relayout` and `command` in `src/console/mux.c` |
| **pane lock** | Setting that forwards Alt-arrow keys to the active program instead of using them for adjacent-pane focus. Prefix commands remain available. | `pane_locked` and `key_in` in `src/console/mux.c` |
| **prefix** | First key of a control sequence, normally Ctrl-Space. A command key follows it. Repeating the prefix forwards one prefix byte to the program. | `src/term/pump.c`, `src/console/attach.c`, `src/console/mux.c` |
| **control mode** | Pane, window, resize, or move mode. Subsequent keys act within that mode; `?` shows its help. | `mode_key` in `src/console/mux.c` |
| **picker** | List for selecting a pane or a message recipient. | `draw_jump` in `src/console/mux.c`; `draw_picker` in `src/console/chat.c` |
| **alternate screen** | Terminal screen buffer used by many full-screen programs and by the compositor after a split. It is distinct from the main terminal screen. | `src/console/attach.c`, `src/screen/screen.c` |
| **screen snapshot** | Captured screen state. Text, JSON, and image representations are available through `peek`; it is not the same as the output log. | `src/runtime/screen_cmds.c`, `src/screen/render.c` |
| **idle output** | No new program output for the requested interval, subject to the wait command's sequence conditions. It does not mean the program is ready for input or has completed a task. | `check_wait` in `src/console/broker.c` |

Use functional help headings such as **Pane Navigation**, **Window Navigation**,
**Pane Resizing**, and **Pane Placement**. Use **focus** for keyboard selection,
**move** for relocating a pane, and **swap** for exchanging two panes.

## Messaging and storage

| Term | Meaning and usage | Implementation evidence |
| --- | --- | --- |
| **chat window** | Floating message interface drawn over the program display. **Overlay** describes how it is rendered. | `src/console/chat.c`, `src/console/mux.c` |
| **recipient** | Managed process or named inbox addressed by a message. | `src/runtime/inbox.c`, `src/console/chat.c` |
| **chat name** | Per-user sender identity for the floating chat window and its inbox. | `hold_person_chat_name` in `src/runtime/inbox.c`, `src/runtime/config_cmd.c` |
| **inbox** | File-backed destination for messages to a sender outside a managed process. | `src/store/inbox.c`, `src/runtime/inbox.c` |
| **conversation** | Chat-window history associated with a process ID or inbox name. | `struct chat_thread` in `src/console/chat.c` |
| **sender header** | `[msg from NAME; reply: backmux msg NAME "..."]`, put before every message typed into a process. It is an address, not proof of who sent it. | `deliver_msg` in `src/console/broker.c` |
| **delivered** | Message transport accepted the message for program input or its destination inbox. It does not establish that an agent read, understood, or acted on it. | `src/console/broker.c`, `src/console/chat.c`, `src/runtime/inbox.c` |
| **output log** | Recorded program output. Keep it distinct from the message log and conversation history. | `src/console/broker.c`, `src/term/pump.c` |
| **user store** | Per-user records and artifacts. | `src/store/layout.c`, `src/access/invocation.c` |
| **system store** | Root-managed records and artifacts, with a separate public index for limited visibility. | `src/store/layout.c`, `src/store/public_index.c` |
| **public index** | Limited metadata for system-managed processes, without private command arguments or logs. | `src/store/public_index.c`, `src/runtime/list.c` |

## Writing and notation

- Keep literal commands, flags, setting keys, environment variables, and paths
  in code formatting in Markdown. Explain `working` as “working directory” and
  `args` as “arguments”; do not change their accepted syntax.
- Write key names as `Ctrl-Space`, `Alt-arrows`, `Shift-arrows`, `Enter`, `Esc`,
  and `Tab`. Spaces in `Ctrl-Space p a` mean sequential keypresses; `Ctrl-`
  identifies a chord. Preserve parser spelling such as `--detach-keys ctrl-space,q`.
- Use concise operation/object labels: “Focus adjacent pane,” “Open attach
  picker,” “Select recipient.” Put conditions and side effects in the related
  help entry when a label cannot carry them clearly.
- Screen coordinates in JSON/snapshots and `send --mouse ACTION,COL,ROW` are
  zero-based. State the coordinate order where it matters; mouse arguments
  use column, then row.
- Follow each surface's heading convention; use sentence case for descriptions.
  Use punctuation for sentences and omit final periods in compact labels.

See [Help Maintenance](help.md) for help organization and
[the language audit](language-audit.md) for reviewed scope and remaining gaps.
