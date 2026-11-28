# tests/test_docker.sh - the Docker-style surface: commands that must act as
# Docker's do, for scripts written for docker. Sourced by test_backmux.sh,
# which registers these with run_test. Why a file of its own: the 500-line
# rule (AGENTS.md) - new tests go in files by area.

# backmux run reads docker run's grammar: the first operand is the program
# (never a backmux verb), -w is the working directory, arguments pass as
# typed, no terminal without -t, the program's exit status comes back, a
# detached run keeps it for wait, and run's own errors exit 125.
test_docker_run_grammar() {
  local rc out dir
  rc=0; "$HOLD_BIN" run list >/dev/null 2>"$TEST_ROOT/dr.err" || rc=$?
  [ "$rc" -eq 127 ] && grep -q "cannot start 'list'" "$TEST_ROOT/dr.err" || { echo "run list: rc=$rc" >&2; cat "$TEST_ROOT/dr.err" >&2; return 1; }
  dir=$(cd /tmp && pwd -P)
  [ "$("$HOLD_BIN" run --rm -w /tmp /bin/pwd)" = "$dir" ] || { echo "run -w /tmp" >&2; return 1; }
  [ "$("$HOLD_BIN" run --rm --workdir=/tmp /bin/pwd)" = "$dir" ] || { echo "run --workdir=/tmp" >&2; return 1; }
  mkdir -p "$TEST_ROOT/args/etcx"
  [ "$(cd "$TEST_ROOT/args" && "$HOLD_BIN" run --rm /bin/echo etcx)" = etcx ] || { echo "a path-like argument was rewritten" >&2; return 1; }
  rc=0; out=$("$HOLD_BIN" run --rm sh -c 'tty; exit 42' 2>/dev/null) || rc=$?
  [ "$rc" -eq 42 ] && [ "$out" = "not a tty" ] || { echo "run without -t: rc=$rc out=$out" >&2; return 1; }
  [ "$(TS_RUN_E=passed "$HOLD_BIN" run --rm -e TS_RUN_E sh -c 'echo "$TS_RUN_E"')" = passed ] || { echo "-e KEY did not pass it through" >&2; return 1; }
  out=$("$HOLD_BIN" run -d sh -c 'exit 42' 2>"$TEST_ROOT/dr.err") || return 1
  printf '%s\n' "$out" | grep -Eqx '[a-f][0-9a-f]{63}' && [ ! -s "$TEST_ROOT/dr.err" ] ||
    { echo "run -d printed: $out / $(cat "$TEST_ROOT/dr.err")" >&2; return 1; }
  [ "$("$HOLD_BIN" wait "$out" 2>/dev/null)" = 42 ] || { echo "run -d lost the exit status" >&2; return 1; }
  for args in "" "--bogus x" "-z x" "-p 80:80 x" "-w"; do
    rc=0; "$HOLD_BIN" run $args >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 125 ] || { echo "run $args: rc=$rc (want 125)" >&2; return 1; }
  done
}
