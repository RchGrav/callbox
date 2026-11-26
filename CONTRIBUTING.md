# Contributing to Backmux

Backmux is a C11 process and terminal management tool. Validate changes
with checks that exercise the affected behavior. Product wording follows
[AGENTS.md](AGENTS.md), the [glossary](docs/glossary.md), and
[help maintenance](docs/help.md).

## Checks

| You want | Run | What it does |
| --- | --- | --- |
| The full local check (macOS or Linux) | `make ci` | Runs `scripts/ci.sh`: version-sync check, default-link and dynamic `-Werror` builds, `make test`, ASan/UBSan, cppcheck (if installed), and the layer lint. A missing tool is reported as SKIPPED, never passed. Refuses to run as root. |
| Linux results from any host | `scripts/linux.sh` | Builds `docker/Dockerfile.linux-ci` with Apple `container`, Docker, or Podman (first found), copies your working tree into the container, and runs `scripts/ci.sh` there as the non-root `ci` user. Your tree is not changed. |
| The privilege-delegation lane | `scripts/linux.sh root` | Runs `scripts/test_root.sh` in the container as the non-root `ci` user with passwordless sudo. Tests elevate one at a time, and `HOLD_REQUIRE_ROOT_TESTS=1` turns any skip into a failure. On a Linux host with passwordless sudo, `bash scripts/test_root.sh` runs it directly. |
| Release tarballs | `scripts/linux.sh release [dir]` | Runs `scripts/release_build.sh` in the container. It builds the native-arch Linux `gnu-static` and `gnu-dynamic` tarballs and the musl matrix (amd64, arm64, armhf, mipsel, riscv64) into `dist/` (or `dir`). Each is packaged with `.github/scripts/package_tarball.sh`, the script CI uses. |
| A macOS tarball | `scripts/release_build.sh` | On a Mac, builds and packages the native macOS tarball into `dist/`. |

`make lint` runs `scripts/lint_layers.sh` alone.

`scripts/release_build.sh` refuses to build from a tagged checkout with
uncommitted changes, so a tagged artifact always matches its tag. It does not
build the cross-arch GNU tarballs (made with dockcross in `release.yml`), and
it does not produce `install.sh` or `SHA256SUMS`. Its GNU builds do not pass
`-Werror`; `release.yml`'s native GNU builds do.

The container image pins Zig (`ZIG_VERSION`), which the musl builds use. The
`ubuntu:24.04` base and the apt packages are not pinned.

GitHub CI runs the same build, sanitizer, cppcheck, and lint commands as
separate jobs, and adds lanes `make ci` does not have. See
[docs/CI-PARITY.md](docs/CI-PARITY.md) for every lane.

On macOS, `make ci` is the only place the `__APPLE__` code is sanitized, and
GitHub's macOS job does not run `tests/test_backmux.sh`. Linux runtime paths require Linux, through `scripts/linux.sh` or a Linux
host. cppcheck runs locally wherever it is installed. Before a release,
run `make ci` on a Mac and the Linux checks as well.

## Tests

- `tests/test_backmux.sh` is the black-box regression suite. It runs the built
  binary and checks stdout, stderr, exit codes, and the filesystem.
  `HOLD_TEST_ONLY='<regex>'` runs only the tests whose function name or
  description match.
- `tests/screen_test.c` unit-tests the terminal screen, snapshots, input
  encoding, and the chat compositor (`make screen-test`).
- `tests/test_040_dockerish.sh` checks the Docker-style command surface against
  `backmux-test` (`make test-040`).
- `tests/test_version_makefile.sh` and `tests/test_release_installer.sh` cover
  version resolution and `install.sh`.
- `make test` runs all of the above.
- Some suite tests drive real terminal programs: tmux, whiptail, vim, micro,
  and python3 (Debian/Ubuntu: `apt install tmux whiptail vim-tiny micro
  python3`). A test whose program is missing reports SKIP. Install them all,
  because a skipped test provides no validation. The Linux CI image includes
  these dependencies; hosted Linux test jobs install the terminal programs.

Conventions:

- A test that needs something absent (a root actor, `setsid`) calls
  `skip "<reason>"`. It is reported as SKIP, not as a PASS that tested
  nothing. `HOLD_REQUIRE_ROOT_TESTS=1` turns every skip into a failure;
  `scripts/test_root.sh` sets it.
- Assert the specific observable: exact exit code, file mode and owner,
  exact output, not just "exit 0". Add a negative test for every refusal
  path.
- Security-sensitive changes (store layout and modes, sudo routing, the
  signal path, console sockets, message-text sanitizing) need a test that fails
  if the invariant regresses. See `test_system_store_*`,
  `test_*_sudo_provenance`, `test_signal_*`, and `test_msg_*` for the pattern.

## Architecture

Source is layered under `src/`, with public headers in `include/backmux/`:

`core`/`screen` → `platform` → `store` → `term` → `console` → `access` →
`runtime` → `cli`/`main`

A lower layer never includes a higher layer's header; `make lint` enforces
this. Internal symbols use the `hold_` prefix.
