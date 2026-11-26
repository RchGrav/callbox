#!/usr/bin/env bash
# Privilege-delegation lane: run the regression suite as a NON-root user that can
# elevate via passwordless sudo, and let the suite elevate per-test.
#
# The suite's as_user/as_root/as_sudo_from_user helpers exercise user, root,
# and sudo invocations separately, including SUDO_UID/SUDO_USER provenance.
# Builds and cleanup remain owned by the non-root test runner.
# HOLD_REQUIRE_ROOT_TESTS=1 makes skipped privilege-sensitive tests fail.
#
# Shared verbatim by the GitHub CI lane (runs as the non-root 'runner') and by
# scripts/linux.sh (runs this as the container's non-root 'ci' sudoer).
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
  echo "test_root.sh: run as a non-root user with passwordless sudo." >&2
  echo "  This lane tests sudo invocations with the invoking user's" >&2
  echo "  SUDO_UID/SUDO_USER. In the container use" >&2
  echo "  'scripts/linux.sh root' (it runs this as the non-root 'ci' sudoer)." >&2
  exit 1
fi
if ! command -v sudo >/dev/null 2>&1 || ! sudo -n true >/dev/null 2>&1; then
  echo "test_root.sh: need passwordless sudo — the elevated tests must be able to run," >&2
  echo "  and HOLD_REQUIRE_ROOT_TESTS=1 makes a missing root actor a hard failure." >&2
  exit 1
fi

src="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
werror="${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -Werror -O2}"

# Build + run in a private temp copy so the host/source tree is never mutated.
# Everything here runs as this one non-root user, so the EXIT-trap cleanup owns
# what it removes.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src"
# Anchor excludes to repo-root paths: a bare --exclude=backmux would also drop
# the include/backmux/ header directory.
tar --exclude='./.git' --exclude='./backmux' --exclude='./backmux-dynamic' --exclude='./backmux-test' \
    --exclude='./obj' --exclude='./obj-test' \
    --exclude='./backmux.dSYM' --exclude='./backmux-dynamic.dSYM' \
    -C "$src" -cf - . | tar -xf - -C "$tmp/src"

cd "$tmp/src"
umask 022
make clean
HOLD_REQUIRE_ROOT_TESTS=1 make test CFLAGS="$werror"
