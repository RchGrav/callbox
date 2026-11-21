# CI parity contract

[Docs index](index.md) | [Contributing](../CONTRIBUTING.md)

Callbox's tests run in lanes split by privilege model. A root run can hide
bugs that only ordinary users hit, and a plain user run skips the root
tests, so each lane covers what the others cannot.

## Local lanes

### Normal lane: `scripts/ci.sh`

```sh
bash scripts/ci.sh     # same as: make ci
```

Run it as a normal user. It refuses UID 0, because a root run can mask bugs
that only exist for ordinary users. It runs, in order:

1. the version-sync check (`scripts/check_version_sync.sh`);
2. static and dynamic builds with `-Werror`;
3. `make test` and `make test-040`;
4. `make test` under ASan/UBSan, if the compiler supports it;
5. cppcheck, if installed;
6. the layer-dependency lint (`scripts/lint_layers.sh`).

A step the local toolchain cannot run is reported as SKIPPED, never passed.

### Privilege-delegation lane: `scripts/test_root.sh`

```sh
bash scripts/test_root.sh
```

Run it as a normal user with passwordless sudo; it refuses UID 0. The suite
elevates only the tests that model root behavior. This keeps the
`SUDO_UID`/`SUDO_USER` provenance those tests check, and keeps the
normal-user assertions meaningful. It sets `HOLD_REQUIRE_ROOT_TESTS=1`, so a
skipped root test fails the lane. It builds in a private temp copy, so the
source tree is never changed.

Do not replace this lane with `sudo make test`. A whole-suite root run sees
no invoking user, so it cannot test provenance.

`scripts/linux.sh` runs both local lanes inside the CI image:
`scripts/linux.sh` for the normal lane, `scripts/linux.sh root` for this one.

## GitHub CI lanes (`.github/workflows/ci.yml`)

| Job | What it runs |
| --- | --- |
| `build-and-test` (gcc, clang) | Static and dynamic `-Werror` builds, then `make test`, as the non-root runner. |
| `privilege-delegation-test` | `bash scripts/test_root.sh`. |
| `root-runner` | `make test` as root with the `SUDO_*` variables removed. The suite then acts as a throwaway user per test. |
| `fifo-console` | `make test` with `CALLBOX_CONSOLE_TRANSPORT=fifo`, then `tests/test_callbox.sh` with Unix sockets denied through an `LD_PRELOAD` shim, so the FIFO fallback runs for real. |
| `macos-runtime` (arm64, x86_64) | `.github/scripts/test_macos_ci.sh`: a `-Werror` clang build, launch and screen smoke tests, `screen-test`, the version and installer tests, and installer dry runs. It does not run `tests/test_callbox.sh`. |
| `sanitizers` | `make test` under ASan/UBSan with gcc. |
| `static-analysis` | cppcheck. |
| `layer-dependencies` | `bash scripts/lint_layers.sh`. |

The build, sanitizer, cppcheck, and lint jobs run the same commands as
`scripts/ci.sh` but do not call it. The version-sync check runs only
locally. The root-runner, FIFO, and macOS lanes run only on GitHub.

## Release gate (`.github/workflows/release.yml`)

The publish job depends on two test jobs, so a tag produces no release
assets unless both are green:

1. `test`: `make test` as the non-root runner;
2. `privilege-delegation-test`: `scripts/test_root.sh`.

The release workflow does not run the other `ci.yml` lanes.

## Static analysis parity

cppcheck versions differ between local machines and GitHub runners. Keep the
cppcheck command identical in `scripts/ci.sh` and `.github/workflows/ci.yml`,
so a local pass means the same as a CI pass. Add any targeted suppression in
both places, with its reason in the commit message.
