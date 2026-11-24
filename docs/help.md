# Maintaining the help system

[Docs index](index.md) | [Contributing](../CONTRIBUTING.md)

Backmux's help has three levels: a short overview, task guides, and a
complete reference. Keep each level useful on its own. The overview helps
someone choose a next action; a guide carries one workflow from setup to
verification and cleanup; per-command pages describe syntax and options.

## Where the text lives

| Surface | Source | Rendering |
| --- | --- | --- |
| `backmux help`, `-h`, `--help` | [overview.txt](../src/cli/help/overview.txt) | `hold_usage` in [runtime/commands.c](../src/runtime/commands.c) adds the version banner. |
| Five workflows | [guides/](../src/cli/help/guides/) | `help tui`, `share`, `agents`, `messaging`, `service`. |
| One command | [commands/](../src/cli/help/commands/) | `help COMMAND` or `COMMAND --help`; aliases share the canonical page. |
| Complete reference | [reference/all.txt](../src/cli/help/reference/all.txt) plus the other reference and command pages | `show_all_help` in [cli/specs.c](../src/cli/specs.c) assembles them from the command table. |
| Additional examples and background | [extra/](../src/cli/help/extra/) | `help examples` and `help features`, listed by `help help`. |

[scripts/gen_help.sh](../scripts/gen_help.sh) converts the text pages into
`gen/help_text.c` during the build. [core/help.c](../src/core/help.c) prints
those compiled lines. Installed binaries need neither the source files nor
a network connection to show help. Keep `gen/` generated; edit the text.

`help all` includes launch options, targets, system scope, scripting,
every canonical command's **full** page, and aliases from the parser's
tables. Its source page is an introduction, not a second command inventory.
Adding a command requires its parser row, dispatch, and canonical help page;
the complete reference picks up that row automatically.

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
| Bare attached chat, `-it`, `-dt`, `-d`, `-dw`, and redial | [cli/main.c](../src/cli/main.c), [runtime/start.c](../src/runtime/start.c), [runtime/call.c](../src/runtime/call.c) | Launch-mode, plain-background, redial, and exit-code tests. |
| Screen text, color PNGs, cursor, keys, mouse, resizing, and waits | [runtime/screen_cmds.c](../src/runtime/screen_cmds.c), [console/broker.c](../src/console/broker.c), [screen/](../src/screen/) | Screen unit tests, Vim keyboard/mouse, pipe-input, saved-screen, idle, and output-sequence tests. |
| Shared attachment and separate chat | [console/attach.c](../src/console/attach.c), [console/chat.c](../src/console/chat.c), [console/compose.c](../src/console/compose.c) | Attach/detach and chat tests; the shared-screen guide also has a manual PTY walkthrough. |
| Outside sender identity, inbox history, monitor, and reply waits | [runtime/inbox.c](../src/runtime/inbox.c), [store/inbox.c](../src/store/inbox.c), [runtime/screen_cmds.c](../src/runtime/screen_cmds.c) | `test_operator_inbox_round_trip`, `test_operator_monitor_stream_and_seen`, `test_operator_inbox_timeout_bounds`, `test_operator_inbox_names_do_not_shadow_calls`, `test_operator_inbox_parallel_registration`. |
| Logs, stopping, saved records, and purge | [runtime/signal.c](../src/runtime/signal.c), [runtime/list.c](../src/runtime/list.c), [runtime/call.c](../src/runtime/call.c) | Log, signal escalation, save/rename, and targeted/sweeping purge tests. |

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

Render the overview and all five guides after editing. See
[CI parity](CI-PARITY.md) for platform and privilege coverage, and
[the readiness review](release-readiness.md) for the evidence and limitations
of this onboarding pass.
