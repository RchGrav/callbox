# Historical onboarding and 1.0 readiness review

[Docs index](index.md) | [Help maintenance](help.md) | [CI parity](CI-PARITY.md)

This is the recorded review of an earlier revision. Its command counts,
architecture descriptions, and test results are historical; they do not
validate the current branch. See [language audit](language-audit.md) for the
current wording review.

Reviewed on 2026-11-23, starting from `deb3ac3` (`v0.9.12`). This review
covers the onboarding, help, command surface, messaging fixes, and validation
in this change, with the successful upstream CI results added during 1.0.0
release preparation.

## Assessment

The exercised terminal, messaging, and background workflows work, and the
project has a functioning build, test, installer, and release pipeline.
The help now makes their boundaries explicit. The three reproduced inbox
bugs found during this review are fixed and have regression coverage.

The implementation at `efc0388` passed the full upstream
[CI workflow](https://github.com/RchGrav/backmux/actions/runs/45257369999)
and [multi-platform workflow](https://github.com/RchGrav/backmux/actions/runs/45257369892),
including privilege tests and all 13 platform builds. The maintainer has
chosen to release 1.0.0 and test it through the published installer.
Release preparation changes only the changelog and release documentation;
the tested implementation is unchanged.

Real cross-harness acceptance remains unverified. This environment
validated the messaging transport and a persistent stand-in harness,
but not actual Claude Code/Codex input queueing or agent wakeups. These
are harness-dependent behaviors, not guarantees added by this release.

## What the review established

| Area | Evidence |
| --- | --- |
| Help architecture | The existing plain-text generator is retained. The overview is 38 lines; all 36 compiled pages fit 80 columns. `help all` includes 23 canonical command pages, launch/reference sections, five command aliases, and four topic aliases. Source/render equality and coverage are tested. See [help maintenance](help.md). |
| TUI inspection and control | Executed the `help tui` Vim-compatible `vi` example: text, color PNGs, cursor JSON, mouse input, resize from 80×24 to 100×30, saved alternate screen, exit code, and purge. Inspected both PNGs. |
| Shared interactive use | A real PTY remained attached while independent `send`, `peek`, and `resize` commands operated the editor. Human and agent input appeared on the same screen. Detach returned 0 and left the process running; normal exit and purge then succeeded. |
| Agent messaging | Local test processes exercised inside/outside delivery, a stable process-derived inbox identity, unread messages, history, continuous monitoring, and reply waits. Real agent harnesses were not launched. |
| CI jobs and services | Executed the job flow with success and failure: printed child codes 0 and 2 propagated to the script. A Python HTTP server passed readiness/content checks; success and failing-test paths both logged output, stopped the service, and purged its record. |
| Presentation | A runnable `sh`/`tr` quickstart, explicit launch modes, and direct links to the same guides replace competing command summaries. Four existing PNGs were reviewed and retained unchanged; Vim is the README hero. No new artwork or screenshots were introduced. |
| Packaging baseline | [v0.9.12](https://github.com/RchGrav/backmux/releases/tag/v0.9.12) publishes 13 platform archives, an installer, and checksums. Its [CI](https://github.com/RchGrav/backmux/actions/runs/45244602235), [multi-platform build](https://github.com/RchGrav/backmux/actions/runs/45244602182), and [release](https://github.com/RchGrav/backmux/actions/runs/45244604485) completed successfully. Those runs establish the baseline, not validation of this change. |
| Updated implementation | The [CI](https://github.com/RchGrav/backmux/actions/runs/45257369999) and [multi-platform build](https://github.com/RchGrav/backmux/actions/runs/45257369892) for `efc0388` both completed successfully. They exercise the updated implementation across the existing platform, privilege, analysis, and sanitizer lanes. The versioned release workflow separately downloads and smoke-tests the published installer. |

The images were already in the repository in commits `35267e0` and
`1e6e4c4`; repository and vendored font licensing were reviewed. The README
caption describes the existing image rather than attributing it to a new
example. See [LICENSE](../LICENSE) and [third_party](../third_party/).

## Confirmed fixes

| Problem reproduced on the baseline | Change and regression |
| --- | --- |
| Parallel first messages could create several inbox names for one harness, splitting replies. A contention test produced 12 names for the same identity. | Keep identity lookup and allocation under the same directory lock used by process naming. `test_operator_inbox_parallel_registration` now observes one identity. |
| A process could take an active inbox name; `msg NAME` then went to the inbox instead of the process. | Check live inbox names during launch and rename in the same store. `test_operator_inbox_names_do_not_shadow_calls` covers refusal with status 11 and reuse after expiry. |
| An unrepresentable timeout was accepted as `LONG_MAX`, then overflowed into an immediate timeout. | Share checked parsing between inbox and reply waits; compare elapsed time instead of adding a deadline. `test_operator_inbox_timeout_bounds` covers invalid values, zero, and a delayed reply with `LONG_MAX`. |
| A monitor that had printed messages still reported “no reply” at its deadline. | Describe the end of the monitoring window; preserve timeout status 7. |
| A no-helper test mistook the sandbox parent's command-line text for a Backmux process. | Check its executable name, including Linux's 15-byte `comm` limit. Verified with an actual binary named `backmux-under-test`; the privileged companion uses the same helper. |

No existing duplicate identities or conflicting records are automatically
renamed or deleted. Allocation uses the project's existing advisory lock;
its existing best-effort behavior when locking is unavailable is unchanged.

## Validation commands and results

The final local gate completed with **216 passed, 0 failed, 23 skipped**
in the black-box runtime suite. Screen tests, version/build tests, release
installer tests, and the Docker-shaped smoke test also passed.

```sh
make -j4 CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' \
  backmux backmux-dynamic backmux-test

env PATH="/tmp/readiness-tools:$PATH" HOLD_TEST_SOCKETS_DENIED=1 \
  make test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'

HOLD_TEST_ONLY=help_pages_fit_and_say_it HOLD_BIN=./backmux-test \
  HOLD_TEST_SOCKETS_DENIED=1 bash tests/test_backmux.sh

bash scripts/lint_layers.sh
bash scripts/check_version_sync.sh
bash -n tests/test_backmux.sh
git diff --check
```

`/tmp/readiness-tools/vim` was a temporary symlink to the installed
Vim-compatible `/usr/bin/vi`, allowing the existing Vim regression to run.
No application was installed. `HOLD_TEST_SOCKETS_DENIED=1` identifies the
observed sandbox restriction: `socket(AF_UNIX)` returns `EPERM`, so live
console checks exercised automatic FIFO fallback. The 23 skips were 17
privilege/PID-namespace cases, three socket-specific cases, and three
unavailable optional TUIs (`whiptail`, `tmux`, `micro`).

The original baseline suite passed 211/0/24. The first changed full run
had one stale overview-text assertion; it was corrected before the final
216/0/23 run. A review also caught and corrected Linux process-name
truncation in the test helper before final validation. The three product
bug regressions were demonstrated to fail on the original binary and pass
on the fixed binary.

Rendered and read `help`, `help tui`, `help share`, `help agents`,
`help messaging`, `help service`, and `help all`. The full reference is 629
lines, with no line over 80 columns. Per-command and alias output was also
compared with the compiled source pages. All 114 local links in changed
Markdown files resolved, and 16 executable shell example blocks passed
`sh -n`; the identity document's illustrative placeholder syntax was excluded.

## Compatibility and known limits

- `monitor` is an additive spelling for `inbox --monitor`. Existing command
  and help-topic aliases remain available. The old abbreviation `m` is now
  ambiguous with `msg`; use `msg` or `monitor` explicitly.
- `monitor` is a reserved verb/name. An external program named `monitor`
  can still be launched with `backmux -- monitor`. An existing process with
  that name can be managed normally, redialed by ID, or renamed.
- An outside inbox starts with the first `msg`, not with `monitor`.
  Its default identity depends on process ancestry; explicit inbox names
  work when crossing harnesses or terminals. Reading history, waiting, and
  monitoring share unread progress; they are not independent subscriptions.
- `--wait-reply` takes the first new inbox message from any sender. It
  provides no request correlation. Backmux does not determine whether a
  harness queued a message, finished a task, or woke an agent on output.
- Backmux cannot adopt a program started outside it. Mouse input requires
  the application to enable reporting. Only one interactive client can
  attach to a process, while independent control commands remain available.
- Plain `-d` and `-dw` processes have logs but no screen or input console.
  Plain `-d` records no exit code. Restart policies do not install a boot
  service. `ports` and meaningful `stats` output are Linux-only.
- Root/sudo, other PID namespaces, macOS, native Unix sockets, unavailable
  TUIs, and real external harnesses were not exercised locally. Clang,
  cppcheck, and sanitizer lanes were not rerun locally. The upstream CI
  and cross-platform builds linked above passed on the updated implementation;
  they do not establish real external-harness behavior. Installer regression
  tests passed. See [CI parity](CI-PARITY.md) for the existing coverage.
