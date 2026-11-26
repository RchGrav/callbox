# Product-language audit

Reviewed on 2026-11-26 against the `backmux` branch of `rchgrav/callbox`,
starting at commit `a716055f3ebfdba102f246f9fc95b03d0a4311cd`.

This review covers first-party explanatory text and the implementation needed
to verify its claims. It does not establish that every runtime path is correct
or that every supported platform and terminal was exercised.

## Reviewed scope

| Surface | Actual review |
| --- | --- |
| CLI help | All 48 existing text files under `src/cli/help/`; 44 updated, four retained after review. Added `guides/panes.txt`, for 49 final help sources. |
| Command definitions | All 40 rows in `src/cli/specs.c` (34 canonical entries and six aliases), topic lookup in `src/core/help.c`, and launch/option dispatch in `src/cli/main.c`, including the `run` form handled outside that table. |
| First-party source and interfaces | All 51 `.c` files under `src/`, the 15 direct headers in `include/backmux/`, `src/console/overlay.h`, and `src/store/store_internal.h`: 68 source/header files. Reviewed comments, explanatory strings, and relevant handlers; files without product text were checked for that absence. |
| Interactive interface | Prefix help; Pane Navigation, Window Navigation, Pane Resizing, and Pane Placement help; attach and pane-name pickers; pane footers and prompts; chat recipient selection, hints, notices, and delivery errors. |
| Existing documentation | `README.md`, `CONTRIBUTING.md`, and all 16 pre-existing Markdown files in `docs/`, listed below. |
| Scripts, tests, and build text | All ten files in `scripts/`, five in `.github/scripts/`, six in `tests/`, `install.sh`, and `Makefile`. Reviewed explanatory comments/docstrings, diagnostics, test descriptions, and affected expected output. |
| CI descriptions | The three files in `.github/workflows/` and their referenced scripts were inspected to check documented triggers and verification lanes. |
| New maintenance material | Root `AGENTS.md`, [glossary.md](glossary.md), and this audit record. |

The existing documentation files reviewed were:

- [CI-PARITY.md](CI-PARITY.md), [SPEC.md](SPEC.md),
  [backmux-identity.md](backmux-identity.md), and [backmux.md](backmux.md).
- [console.md](console.md),
  [docker-familiarity-contract.md](docker-familiarity-contract.md),
  [guide.md](guide.md), and [help.md](help.md).
- [identity.md](identity.md), [index.md](index.md),
  [install.md](install.md), and [launcher.md](launcher.md).
- [release-readiness.md](release-readiness.md), [screen.md](screen.md),
  [shell.md](shell.md), and [store.md](store.md).

Historical changelog entries, third-party code and text, licenses, generated
font/name tables, and image contents were excluded from language revision.
The earlier release-readiness findings remain historical and are labeled as
such. Existing screenshots were not regenerated; their introductory captions
identify them as illustrations from an earlier revision. Generated
`gen/help_text.c` was rebuilt from source rather than edited directly.

## Capability and terminology map

The application provides a CLI, terminal attachment, a pane compositor, and
a floating chat interface. This review found no separate graphical settings
application or saved multi-pane workspace editor to document.

| Functional group | Verified operations and boundaries | Implementation evidence |
| --- | --- | --- |
| Launch programs | `call`, `agent`, `server`, `detach`, `shell`, the bare-program form, and supported Docker-style launch flags. Agent formatting is a message-header setting, not an AI model selector. | [cli/main.c](../src/cli/main.c), [runtime/start.c](../src/runtime/start.c) |
| Manage processes | Listing, inspecting, following logs, viewing resource/port information, stopping, and purging. A process, its record, and its operating-system PID are distinct. | [runtime/list.c](../src/runtime/list.c), [runtime/observe.c](../src/runtime/observe.c), [runtime/signal.c](../src/runtime/signal.c) |
| Manage profiles | `save`, `rename`, `set`, `unset`, `info`, `recall`, and `delete`. A saved profile is a protected process record and launch recipe. It does not preserve pane geometry. | [runtime/call.c](../src/runtime/call.c), [runtime/settings.c](../src/runtime/settings.c) |
| Terminal controls | Attach/detach, screen snapshots, exact supported key and mouse forms, terminal resizing, output waits, panes, and numbered windows. Pane controls require a terminal process and the applicable interactive attach mode. | [console/attach.c](../src/console/attach.c), [console/mux.c](../src/console/mux.c), [runtime/screen_cmds.c](../src/runtime/screen_cmds.c) |
| Messaging | Process input messages, outside-sender inboxes, retained conversations, and the floating chat window. Transport delivery and output inactivity do not establish semantic completion. | [runtime/inbox.c](../src/runtime/inbox.c), [console/chat.c](../src/console/chat.c), [console/broker.c](../src/console/broker.c) |
| Configuration and stores | `detach-size`, `chat-name`, process settings, user/system stores, and the public system index. Scope depends on the operation and invoking identity. | [runtime/config_cmd.c](../src/runtime/config_cmd.c), [store/layout.c](../src/store/layout.c), [access/invocation.c](../src/access/invocation.c) |

[The glossary](glossary.md) defines the shared vocabulary and links each
concept to implementation evidence. Key distinctions are **program**, **managed
process**, **process record**, **saved profile**, **console**, **pane**,
**window**, **chat window**, and **inbox**. Internal identifiers and accepted
syntax retain their spelling, including `MODE_TAB`, `hold_cmd_redial`,
`working`, `args`, and protocol fields.

## Help hierarchy

The overview now groups entry points under Launch Programs, Manage Processes,
Manage Profiles, Terminal Controls, and Help and Reference. `help help` lists
command, workflow, and reference topics. Command pages describe the operation,
syntax, conditions, outcomes, and relevant related help; `help all` assembles
the complete reference using the existing command table.

The new `help panes` page documents existing controls. The keyboard panel is
a compact task reference, with separate contextual panels for **Pane
Navigation**, **Window Navigation**, **Pane Resizing**, and **Pane Placement**.
It links to `backmux help panes` for details. It does not add a control mode.

Key notation uses `Ctrl-Space`, `Alt-arrows`, `Enter`, `Esc`, and `Tab`.
Spaces in `Ctrl-Space p a` denote successive keypresses. Screen and CLI mouse
coordinates are documented as zero-based, in column/row order where accepted.
Help-source lines stay within 80 columns. Existing rendering behavior at
smaller terminal sizes is discussed under limitations.

## Material findings and corrections

| Priority | Finding and correction | Evidence |
| --- | --- | --- |
| High | Stale comments identified prefix `a` as the attach picker. The actual sequence is `Ctrl-Space p a`; prefix `a` sorts panes by name. Related help, comments, and guides now agree. | `command()` and `mode_key()` in [mux.c](../src/console/mux.c); single-process command registration in [attach.c](../src/console/attach.c) |
| High | Earlier descriptions said another attach was refused. A newer attach replaces the current client without ending the program. Retained error handling was not treated as proof of present broker behavior. | `console_taken()`, `sniff_connection()`, and FIFO admission in [broker.c](../src/console/broker.c) |
| High | Documentation advertised obsolete profile-editor and workspace-layout behavior. Replaced it with the implemented process-profile, pane/window, and separate chat-inbox model. | [mux.c](../src/console/mux.c), [settings.c](../src/runtime/settings.c), [call.c](../src/runtime/call.c) |
| High | Shell usage advertised rejected `--chat-key`. Removed it from both usage definitions. Documented the actual attached chat command and the distinction between compositor-hosted and retained broker-hosted chat paths. | `OPT_CHAT_KEY` handling in [cli/main.c](../src/cli/main.c), `chat_key_pressed()` in [mux.c](../src/console/mux.c), `esc_key()` in [chat.c](../src/console/chat.c) |
| High | Lifecycle wording conflated `recall` with bare-target reconnect/restart, and `delete` with merely clearing a saved flag. Help now describes the actual operations, confirmed-running checks, Unknown-state limits, and forced-purge delegation. | `hold_cmd_recall_action()` and `hold_cmd_delete_action()` in [settings.c](../src/runtime/settings.c); `hold_cmd_redial()` in [call.c](../src/runtime/call.c); `hold_cmd_purge_action()` in [list.c](../src/runtime/list.c) |
| High | Missing endpoints or unavailable records were sometimes described as proof of process exit or inbox expiry. Diagnostics now identify the observed condition. Output-idle and delivery descriptions no longer imply program readiness or agent completion. | [console/chat.c](../src/console/chat.c), [console/mux.c](../src/console/mux.c), [console/broker.c](../src/console/broker.c), [runtime/inbox.c](../src/runtime/inbox.c) |
| Medium | Pane/window vocabulary, mode descriptions, picker prompts, and short help were inconsistent or conversational. Standardized visible labels and added explicit effects, mode exits, and related help. | [mux.c](../src/console/mux.c), [chat.c](../src/console/chat.c), [guides/panes.txt](../src/cli/help/guides/panes.txt) |
| Medium | CLI `swap` was described as accepting keyboard slot letters/numbers. Its CLI lookup uses process names and ID prefixes; keyboard swapping retains slot-letter/window selection. | `slot_of_name()` in [mux.c](../src/console/mux.c), `hold_cmd_swap_action()` in [screen_cmds.c](../src/runtime/screen_cmds.c) |
| Medium | Listing, store scope, and ports text overstated coverage. Documented Unknown states, named filters, actual root/user store selection, TCP listeners, and bound unconnected UDP endpoints. | [list.c](../src/runtime/list.c), `scan_proc_net_file()` in [platform/process.c](../src/platform/process.c) |
| Medium | Message “raw” wording promised byte-exact delivery, and `chat-name` implied immediate renaming. Clarified header formatting versus sanitization/Enter handling, and preferred names at new inbox creation. | `handle_msg()`/`deliver_msg()` in [broker.c](../src/console/broker.c), `operator_name_for()` in [runtime/inbox.c](../src/runtime/inbox.c) |
| Medium | Lifecycle and supervisor descriptions omitted signal escalation or exit-tracking conditions. `end` documents TERM, waiting, and validated KILL escalation; launch documentation accounts for `--rm` and restart policies. | [signal.c](../src/runtime/signal.c), [start.c](../src/runtime/start.c) |
| Medium | Script/CI prose overstated parity or the effect of dry-run/skip flags. Corrected the documented checks, branch triggers, and validation that still occurs. | [scripts/ci.sh](../scripts/ci.sh), [scripts/deploy.sh](../scripts/deploy.sh), [ci.yml](../.github/workflows/ci.yml) |

### Comment and docstring audit

The review explicitly included source comments, header contracts, shell/test
commentary, Markdown example annotations, and Python docstrings. Most C
interfaces use comments rather than language-level docstrings.

Material corrections include hash field separation versus collision immunity;
the timestamp parser's actual accepted input; input-pipe versus PTY brokers;
prefix commands and attach replacement; output-idle conditions; the meaning of
an unavailable console; supported Unicode/font ranges; and CI/deployment
coverage. Reservation age is now described as an age check, not proof that a
launch died. Rename is described as setting the saved flag without inferring
the user's intent.

Removed redundant narration and unsupported rationale where encountered.
Preserved useful interface, ownership, state, and ordering explanations. Test
assertions were updated where they consume edited wording; no new test
framework or runtime conditions were introduced.

## Verification

The production, dynamic, and test binaries were built with:

```sh
make -j4 backmux backmux-dynamic backmux-test \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'
```

The complete test target was run with the existing FIFO transport selected:

```sh
BACKMUX_CONSOLE_TRANSPORT=fifo make -j4 test \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'
```

| Check | Result |
| --- | --- |
| Strict production/dynamic/test builds | Passed. |
| FIFO regression suite and focused rechecks | 225 distinct cases passed and 23 were skipped for environment/tool availability. The complete regression run passed 220 and exposed five remaining text-assertion mismatches; all five passed after those assertions were corrected. See the execution details below. |
| Remaining `make test` constituents | Screen unit tests, version/Makefile tests, release-installer tests, and `test-040` Docker-style command smoke tests passed when invoked separately. |
| Layer dependency lint | `make lint` passed. |
| Version synchronization | `scripts/check_version_sync.sh` passed at version `1.0.0`. |
| Help sources and output | All 49 sources fit within 80 columns; overview is 39 lines including the version banner. The existing help test passed compiled-source equality, command/alias discovery, full-reference inclusion, topic redirects, and output-width checks. |
| Documentation examples | All 32 fenced `sh`/`bash` examples in the 18 pre-existing documentation files, plus both verification snippets in this audit, passed syntax checks. This is not proof of every example's platform behavior. |
| Changed scripts | Nine edited shell scripts passed `bash -n`; `scripts/gen_font.py` passed Python syntax parsing. |
| Rendered UI | Inspected actual screen snapshots for prefix help at 80x24 and 60x24; all four contextual modes at 60x24; and pane-navigation help, resizing help, the attach picker, and the chat recipient picker at 80x24. Shortened hints to fit the reviewed widths. |
| Source/diff review | Across 53 changed C/header files, 52 differ only in comments, strings, or whitespace. The remaining file, `mux.c`, adds help-array entries, adjusts the pane-name picker width for the longer word “window,” and removes an unused formatting argument. Manual review found no changes to runtime control flow, commands, settings, or protocol fields. |
| Links and whitespace | All 268 relative file/image targets and nine Markdown fragment links across the 21 Markdown files resolved. All 35 distinct glossary source paths exist. `git diff --check` passed. |

The first regression run used the environment's automatic transport fallback
and an intermediate set of text assertions. Its result was 207 passed,
20 failed, and 21 skipped. Most failures were expected-output mismatches,
corrected with the wording, or tests requiring a Unix-domain socket file.
That run is not a clean verification result.

An isolated checkout of the baseline reproduced the default-transport
`plainwait` failure exactly: the test expected a free log lock while the
implementation had selected FIFO fallback and held that lock. The test's
expectation uses explicit environment flags; runtime selection also probes
socket availability. Both the baseline `plainwait` test and inline-screen
restoration test passed with FIFO selected (2 passed, 0 failed). The initial
inline-screen failure's cause was not established by those focused passes.

The later complete FIFO regression run passed 220 cases, failed five, and
skipped 23. The failures came from two remaining old-text expectations:
the send/wait early-exit diagnostic and the picker helper's `args` field label.
Updating them to the actual diagnostic and **Arguments** label preserved the
same checks. The five affected tests then passed in a focused run (5 passed,
0 failed, 0 skipped), including pane attachment, detach/split behavior,
window/swap controls, and footers/lock/rename/save controls. The inline-screen
restoration case passed in the complete FIFO run.

Because `make test` stops after a failing regression command, its screen,
version, installer, and Docker-style smoke steps were run separately and all
passed. The aggregate `make test` command was not rerun after the assertion
corrections. Final edits to help spacing and the launch-option diagnostic
received a strict rebuild and focused help/CLI checks (4 passed, 0 failed,
0 skipped).

## Limitations and unresolved questions

- This execution environment rejects Unix-domain socket creation with `EPERM`.
  FIFO was tested; socket-only cases require an environment that permits the
  socket transport.
- Privilege-delegation tests requiring `sudo` and real-program integration
  tests requiring unavailable applications are reported as skips. macOS,
  other terminal emulators, and other architecture/privilege lanes were not
  executed here. This review did not run the full sanitizer or cppcheck lanes.
- The existing help renderer clips content when the terminal cannot fit its
  panel. No wrapping, scrolling, or new minimum-size rule was added. The listed
  dimensions and surfaces are the actual visual coverage; arbitrary narrow
  terminals and assistive-technology behavior remain unverified.
- A target in the Unknown state is not proof that execution ended. The current
  `recall` and `delete` paths can proceed beyond their initial confirmed-running
  checks. `delete` delegates to forced purge, whose later state check can stop
  a target then found running. The wording records this behavior; the audit
  does not change the state-handling policy.
- Configured release URLs still name `RchGrav/backmux`, while the reviewed
  source is `rchgrav/callbox` on `backmux`. Publication destinations were
  preserved because they are used by the installer and release workflow;
  external release availability was not verified.
- Existing CI automatically targets `main`. Pushing this branch does not by
  itself establish that those GitHub Actions lanes ran.

Future product-language changes should follow [AGENTS.md](../AGENTS.md),
[the glossary](glossary.md), and [help maintenance](help.md), with coverage and
verification reported for the actual changed surfaces.
