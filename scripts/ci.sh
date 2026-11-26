#!/usr/bin/env bash
# Run local build, regression, and analysis checks against the current tree:
#   version sync -> default-link -Werror build -> dynamic -Werror build ->
#   regression suite + Docker-shaped smoke -> ASan/UBSan build+test ->
#   cppcheck -> layer-dependency lint.
#
# The Makefile selects platform-specific linking and the tests report missing
# dependencies. Sanitizers and cppcheck are skipped with a notice when absent.
# This does not run every GitHub CI lane: root, privilege-delegation, FIFO,
# and macOS-specific jobs have separate entry points. scripts/linux.sh runs
# these local checks in the repository's Linux CI container image.
#
# Env:
#   CC                     compiler to use (default: cc)
#   HOLD_REQUIRE_ROOT_TESTS=1   require privilege-sensitive tests to run
set -uo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

if [ "$(id -u)" -eq 0 ]; then
  echo "scripts/ci.sh: run the normal CI lane as a non-root user, not via sudo/root." >&2
  echo "  Root-sensitive coverage belongs in scripts/test_root.sh, which starts as a non-root sudoer and elevates per test." >&2
  exit 1
fi

WERROR="-std=c11 -Wall -Wextra -Wpedantic -Werror -O2"
SAN_FLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined"
CC_BIN="${CC:-cc}"
export HOLD_TEST_TIMEOUT="${HOLD_TEST_TIMEOUT:-60}"

fails=0
ran=""
skipped=""
step()    { printf '\n=== %s ===\n' "$1"; }
mark_ok() { ran="$ran $1"; }
mark_no() { skipped="$skipped|$1 ($2)"; printf '  -- SKIPPED: %s (%s)\n' "$1" "$2"; }
must()    { if ! "$@"; then printf '  FAILED: %s\n' "$*" >&2; fails=$((fails + 1)); fi; }

step "version sync"
must bash scripts/check_version_sync.sh
mark_ok "version-sync"

step "default-link build, -Werror (CC=$CC_BIN)"
must make clean
must make CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "static-build"

step "dynamic, -Werror"
must make clean
must make backmux-dynamic CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "dynamic-build"

step "regression suite + surface smoke"
must make clean
must make test CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "suite"

step "ASan/UBSan"
if printf 'int main(void){return 0;}\n' | "$CC_BIN" -fsanitize=address,undefined -x c - -o /tmp/.hold_sanprobe 2>/dev/null; then
  rm -f /tmp/.hold_sanprobe
  must make clean
  # Brokers and waiters run with stderr on /dev/null, so a report would be
  # lost: halt on the first one and keep every report in a log, checked below.
  san_logs=$(mktemp -d)
  must env UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$san_logs/ubsan" \
    ASAN_OPTIONS="log_path=$san_logs/asan" \
    make test CC="$CC_BIN" CFLAGS="$SAN_FLAGS" STATIC_LDFLAGS='' LDFLAGS='-fsanitize=address,undefined' TEST_LDFLAGS='-fsanitize=address,undefined'
  if ls "$san_logs"/* >/dev/null 2>&1; then
    cat "$san_logs"/* >&2
    printf '  FAILED: sanitizer reports above (some from background helpers)\n' >&2
    fails=$((fails + 1))
  fi
  rm -rf "$san_logs"
  mark_ok "asan-ubsan"
else
  mark_no "asan-ubsan" "$CC_BIN has no -fsanitize support"
fi

step "cppcheck static analysis"
if command -v cppcheck >/dev/null 2>&1; then
  must cppcheck --enable=warning,performance,portability --error-exitcode=1 --max-configs=1 \
    --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
    --std=c11 -Iinclude src/
  mark_ok "cppcheck"
else
  mark_no "cppcheck" "not installed; run scripts/linux.sh for parity"
fi

step "layer dependency lint"
must bash scripts/lint_layers.sh
mark_ok "layer-lint"

make clean >/dev/null 2>&1 || true

printf '\n================ ci.sh summary ================\n'
printf 'ran:%s\n' "$ran"
[ -n "$skipped" ] && printf 'skipped:%s\n' "$(printf '%s' "$skipped" | tr '|' '\n  ')"
if [ "$fails" -ne 0 ]; then
  printf 'RESULT: FAIL (%d step(s) failed)\n' "$fails"
  exit 1
fi
printf 'RESULT: PASS\n'
