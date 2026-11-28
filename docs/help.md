# Maintaining the help system

[Docs index](index.md) | [Contributing](../CONTRIBUTING.md) | [Glossary](glossary.md)

Backmux help proceeds from functional command groups to command usage,
workflow guides, and detailed reference. Interactive help describes the
controls available in the current mode. Use the [glossary](glossary.md)
throughout, and apply the product-language rules in [AGENTS.md](../AGENTS.md).

## Where the text lives

| Surface | Source | Rendering |
| --- | --- | --- |
| `backmux help`, `-h`, `--help` | [overview.txt](../src/cli/help/overview.txt) for a person; [tasks/index.txt](../src/cli/help/tasks/index.txt) for an agent | `hold_usage` in [runtime/commands.c](../src/runtime/commands.c) adds the version banner to the person's page. |
| `help all`: the index | [index.txt](../src/cli/help/index.txt) for a person; the agent's task index for an agent | `hold_show_help_for` in [cli/specs.c](../src/cli/specs.c). Never every page: it is the first thing an agent runs. |
| Agent tasks | [tasks/](../src/cli/help/tasks/) | `help agents` (the index) and `help assist`, `look`, `drive`, `reply`, `replies`, `test`, `serve`, `job`, `delegate`, `check`, `cleanup`. |
| Task guides and command aliases | [guides/](../src/cli/help/guides/) | `help tui`, `share`, `messaging`, `service`, `panes`, and `aliases`. |
| One command | [commands/](../src/cli/help/commands/) | `help COMMAND` or `COMMAND --help`; aliases share the canonical page. |
| Complete reference (`backmux manual`) | [reference/all.txt](../src/cli/help/reference/all.txt) plus the other reference and command pages | `show_reference` in [cli/specs.c](../src/cli/specs.c) assembles them from the command table; `manual` adds the examples. |
| Keys: `help keys` and the Ctrl-Space ? panel | [keys/](../src/cli/help/keys/) | `help keys` prints the pages in order; [console/mux_help.c](../src/console/mux_help.c) draws the panel from `keys/prefix` and one page per mode, so the panel and the page cannot disagree. A line at the margin is a section; an indented line is a key, two or more spaces, then what it does. |
| Additional examples and background | [extra/](../src/cli/help/extra/) | `help examples` and `help features`, listed by `help help`. |

[scripts/gen_help.sh](../scripts/gen_help.sh) converts the text pages into
`gen/help_text.c` during the build. [core/help.c](../src/core/help.c) prints
those compiled lines. Installed binaries need neither the source files nor
a network connection to show help. Keep `gen/` generated; edit the text.

`backmux manual` includes launch options, targets, system scope,
scripting, every canonical command's **full** page, aliases from the
parser's tables, and the examples. Its source page is an introduction, not
a second command inventory. Adding a command requires its parser row,
dispatch, canonical help page, and a line in `index.txt`; the manual picks
up the row automatically.

## Page shapes and notation

- **Command pages:** the usage line(s), a blank line, then one purpose
  line that is word for word the command's line in `help all`
  ([index.txt](../src/cli/help/index.txt)), then the details. Why: a reader
  always knows where to look, and the help test fails if the index and a
  page disagree.
- **Notation:** arguments in angle brackets, lowercase (`<target>`,
  `<program>`, `<args>`, `--name <name>`), as Rich's front page wrote them;
  `[...]` optional, `...` repeatable. `<target>` replaced `<name|id>` when
  places (2C) became targets. `Ctrl-Space q` means press, release, then q;
  `Ctrl-x` means hold both.
- **Labels and one-line descriptions:** imperative, sentence case, verb
  then object ("Toggle zoom", "Swap two places"), with the consequence when
  it decides what to press ("Detach; it keeps running").
- **Mode names** are the ones the bar shows: Pane, Window, Resize, Move.
- **The panel** must fit 80x24: labels stay within 24 characters so two
  columns fit, and it never draws past its frame.

## Help for its reader

Help is for whoever runs it. Why: an agent pays for every token it reads,
and a page written for someone else's case reads to it as a missing
feature (an agent inside backmux, reading the page for senders outside it,
reported features as missing that it had).

[runtime/help_reader.c](../src/runtime/help_reader.c) works out the
reader, reading only (help never makes an inbox, a record, or a directory):

- **Inside or outside backmux:** `BACKMUX_ID` or `BACKMUX_NAME` set, the
  rule `msg` uses to choose its sender.
- **Agent or person:** a known harness among help's parent processes, up to
  the backmux broker: the list chat uses (`console/agents.c`) plus the
  config's `agents`, matched by process name, argv, or a directory of the
  executable's path (Claude Code's binary is named for its version).
- **Facts a page can name:** the process's name and who started it, who last
  wrote to it (its `.msgs` log), an outside agent's existing inbox.

A page marks blocks for one reader with a line of tags alone, `[agent]`,
`[person]`, `[inside]`, `[outside]`, combined as `[agent inside]`; `[all]`
ends the block. Lines name facts as `{name}`, `{parent}`, `{last_from}`,
`{inbox}`, `{next}` (and `_raw` forms for the name a command takes); a line
whose fact has no value is left out, and `{!key}` keeps a line only when it
has none. [core/help.c](../src/core/help.c) prints with those rules and no
IO of its own. Names show politely ("Your Agent ID: Sleek Carbon"), and
targets accept that form back. The tool states facts about the process; it
does not speak as a persona.

`backmux help --full PAGE` prints the whole page with no reader lines: for
docs, for the tests (which also set `HOLD_TEST_HELP_READER`, honored only by
`-DHOLD_TESTING` builds, so results don't depend on who runs the suite),
and for writing instructions for a reader in another situation. `manual` is
never tailored. Agent task pages stay within 25 lines and end with the next
step.

## Write and verify a workflow

State when it is useful, name any required application, show a short sequence,
and explain the commands where they are used. Separate interactive actors
and phases: a person, a lead agent, and a detached worker do not all run
the same block. Place stopping and cleanup after the result has been checked.
Use comments that are valid shell syntax in copyable command blocks.

Keep terminal text within 80 columns and keep the overview selective.
Detailed defaults, compatibility spellings, and less-common options belong
in per-command help and the complete reference. Use the existing topic
redirects in `help_renamed` when a guide moves.

Before documenting behavior, trace it to the implementation and an observable
result. The important boundaries in these guides are:

| Behavior | Implementation evidence | Regression coverage |
| --- | --- | --- |
| Native launch commands, flag forms, and recall | [cli/main.c](../src/cli/main.c), [runtime/start.c](../src/runtime/start.c), [runtime/call.c](../src/runtime/call.c) | Launch-mode, plain-background, recall, and exit-code tests. |
| Screen text, color PNGs, cursor, keys, mouse, resizing, and waits | [runtime/screen_cmds.c](../src/runtime/screen_cmds.c), [console/broker.c](../src/console/broker.c), [screen/](../src/screen/) | Screen unit tests, Vim keyboard/mouse, pipe-input, saved-screen, idle, and output-sequence tests. |
| Shared attachment, panes, and separate chat | [console/mux.c](../src/console/mux.c), [console/attach.c](../src/console/attach.c), [console/chat.c](../src/console/chat.c), [console/compose.c](../src/console/compose.c) | Attach/detach and chat tests; the shared-screen guide also has a manual PTY walkthrough. |
| Outside sender identity, inbox history, monitor, and reply waits | [runtime/inbox.c](../src/runtime/inbox.c), [store/inbox.c](../src/store/inbox.c), [runtime/screen_cmds.c](../src/runtime/screen_cmds.c) | `test_operator_inbox_round_trip`, `test_operator_monitor_stream_and_seen`, `test_operator_inbox_timeout_bounds`, `test_operator_inbox_names_do_not_shadow_calls`, `test_operator_inbox_parallel_registration`. |
| Logs, stopping, saved records, and purge | [runtime/signal.c](../src/runtime/signal.c), [runtime/list.c](../src/runtime/list.c), [runtime/call.c](../src/runtime/call.c) | Log, signal escalation, save/rename, delete, rm, and purge sweep tests. |

The black-box tests live in [tests/test_backmux.sh](../tests/test_backmux.sh).
The help test compares compiled output with each source page, verifies
canonical command and alias coverage, checks full-reference inclusion, and
checks line lengths. Syntax checks alone do not prove that an example works:
run the application, inspect its screen, and verify its expected result.

```sh
make -j4 CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' \
  backmux backmux-dynamic backmux-test
HOLD_TEST_ONLY=help_pages_fit_and_say_it HOLD_BIN=./backmux-test \
  bash tests/test_backmux.sh
make test
make lint
```

Render the overview, affected command pages and guides, and the interactive
help modes after editing. See
[CI parity](CI-PARITY.md) for platform and privilege coverage, and
[the readiness review](release-readiness.md) for the evidence and limitations
of that historical review.
