# tests/test_terminal.sh - terminal state: a terminal's record follows what
# it is doing (its program, the shell's directory, the foreground workload,
# its children), and save keeps it. Sourced by test_backmux.sh, which
# registers these with run_test. Why a file of its own: the 500-line rule
# (AGENTS.md) - new tests go in files by area.

# A terminal's record follows what it is doing, not only how it was launched:
# the shell's directory, the foreground workload with its exact argv and
# directory, idle once that ends, a pipeline as one, and the last observation
# once the terminal ends. Paths with spaces, as people's folders have.
# ts_is RECORD CWD FOREGROUND WORKLOAD_CWD [WORKLOAD ARGV...]: the record's
# terminal says exactly that (argv compared as an array, not joined).
ts_is() {
  python3 - "$@" <<'PY'
import json, sys
path, cwd, fg, wcwd, *argv = sys.argv[1:]
t = json.load(open(path)).get("terminal") or {}
w = t.get("workload") or {}
sys.exit(0 if [t.get("cwd", ""), t.get("foreground", ""), w.get("cwd", ""), w.get("argv", [])] == [cwd, fg, wcwd, argv] else 1)
PY
}
ts_field() { # record key: one field of the record's terminal, as JSON
  python3 -c 'import json,sys; print(json.dumps((json.load(open(sys.argv[1])).get("terminal") or {}).get(sys.argv[2])))' "$1" "$2"
}
ts_until() { # as ts_is, after a list (which refreshes), until it holds
  local i
  for i in $(seq 1 40); do
    "$HOLD_BIN" list >/dev/null 2>&1
    ts_is "$@" && return 0
    sleep 0.1
  done
  echo "terminal: $(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get("terminal"))[:400])' "$1") (want: ${*:2})" >&2
  return 1
}
ts_start() { # name dir: a bash terminal started in dir, at its prompt; prints its record's path
  local id
  id=$(cd "$2" && "$HOLD_BIN" -dt --name "$1" -- bash --norc) || return 1
  "$HOLD_BIN" wait "$1" --match '$' --timeout 5000 >/dev/null || return 1
  record_path "$id"
}
test_terminal_state_follows_the_shell() {
  command -v bash >/dev/null 2>&1 || skip "no bash"
  local path a="$TEST_ROOT/ts A" b="$TEST_ROOT/ts B" c="$TEST_ROOT/ts C"
  mkdir -p "$a" "$b" "$c"
  # the kernel reports a process's real directory (macOS: /var is /private/var)
  a=$(cd "$a" && pwd -P) b=$(cd "$b" && pwd -P) c=$(cd "$c" && pwd -P)
  path=$(ts_start ts "$a") || return 1
  ts_until "$path" "$a" idle "" || return 1
  "$HOLD_BIN" send ts "cd '$b' && (cd '$c' && exec sleep 301)" --enter >/dev/null || return 1
  ts_until "$path" "$b" workload "$c" sleep 301 || return 1
  out_has '^workload:  sleep 301$' "$HOLD_BIN" info ts || return 1
  # a look that cannot read the foreground keeps the workload last read
  HOLD_TEST_FOREGROUND_UNREADABLE=1 "$HOLD_BIN" list >/dev/null 2>&1 || return 1
  ts_is "$path" "$b" workload "$c" sleep 301 && [ "$(ts_field "$path" foreground_unread)" = true ] ||
    { echo "an unreadable look lost the workload: $(ts_field "$path" workload)" >&2; return 1; }
  out_has '^workload:  sleep 301 (as last read; it could not be read now)$' env HOLD_TEST_FOREGROUND_UNREADABLE=1 "$HOLD_BIN" info ts ||
    return 1
  ts_until "$path" "$b" workload "$c" sleep 301 && [ "$(ts_field "$path" foreground_unread)" = null ] ||
    { echo "a readable look did not clear unread" >&2; return 1; }
  "$HOLD_BIN" send ts -k C-c >/dev/null || return 1
  ts_until "$path" "$b" idle "" || return 1
  "$HOLD_BIN" send ts 'sleep 302 | sleep 303' --enter >/dev/null || return 1
  ts_until "$path" "$b" pipeline "$b" sleep 302 || return 1
  out_has 'part of a pipeline' "$HOLD_BIN" info ts || return 1
  "$HOLD_BIN" end ts >/dev/null 2>&1 || true
  ts_is "$path" "$b" pipeline "$b" sleep 302 || { echo "the end lost the last observation" >&2; return 1; }
  out_has 'the last observation before it ended' "$HOLD_BIN" info ts || return 1
}

# The workload's argv is kept whole: an argument with spaces, an empty one,
# and one of 20000 characters (longer than any fixed command-line buffer).
test_terminal_workload_argv_is_exact() {
  command -v bash >/dev/null 2>&1 || skip "no bash"
  local path dir long
  dir=$(cd "$TEST_ROOT" && pwd -P)
  long=$(printf 'x%.0s' $(seq 1 20000))
  path=$(ts_start tsargv "$dir") || return 1
  "$HOLD_BIN" send tsargv 'L=$(printf "x%.0s" $(seq 1 20000))' --enter >/dev/null || return 1
  "$HOLD_BIN" send tsargv "sh -c 'sleep 300; :' 'a b' '' \"\$L\"" --enter >/dev/null || return 1
  ts_until "$path" "$dir" workload "$dir" sh -c 'sleep 300; :' 'a b' '' "$long" || return 1
  "$HOLD_BIN" end tsargv >/dev/null 2>&1 || true
}

# The program is read from the live process: a shell that replaces itself
# (exec) is what runs now, a shell or not. Background children count as
# children while the shell stays idle (what exec will need to know).
test_terminal_live_program_and_children() {
  command -v bash >/dev/null 2>&1 || skip "no bash"
  local path dir
  dir=$(cd "$TEST_ROOT" && pwd -P)
  path=$(ts_start tslive "$dir") || return 1
  "$HOLD_BIN" send tslive 'sleep 300 &' --enter >/dev/null || return 1
  ts_until "$path" "$dir" idle "" || return 1
  [ "$(ts_field "$path" children)" -ge 1 ] || { echo "children: $(ts_field "$path" children)" >&2; return 1; }
  "$HOLD_BIN" send tslive 'kill %1; exec sh' --enter >/dev/null || return 1
  local i
  for i in $(seq 1 40); do
    "$HOLD_BIN" list >/dev/null 2>&1
    [ "$(ts_field "$path" argv)" = '["sh"]' ] && break
    sleep 0.1
  done
  [ "$(ts_field "$path" argv)" = '["sh"]' ] && [ "$(ts_field "$path" hosting_shell)" = true ] ||
    { echo "after exec sh: argv $(ts_field "$path" argv), hosting $(ts_field "$path" hosting_shell)" >&2; return 1; }
  "$HOLD_BIN" send tslive 'exec sleep 300' --enter >/dev/null || return 1
  for i in $(seq 1 40); do
    "$HOLD_BIN" list >/dev/null 2>&1
    [ "$(ts_field "$path" argv)" = '["sleep", "300"]' ] && break
    sleep 0.1
  done
  [ "$(ts_field "$path" argv)" = '["sleep", "300"]' ] && [ "$(ts_field "$path" hosting_shell)" = false ] ||
    { echo "after exec sleep: argv $(ts_field "$path" argv), hosting $(ts_field "$path" hosting_shell)" >&2; return 1; }
  "$HOLD_BIN" end tslive >/dev/null 2>&1 || true
}

# A save that could not write the terminal's state is not a save, and says so.
test_save_fails_when_terminal_state_cannot_be_written() {
  command -v bash >/dev/null 2>&1 || skip "no bash"
  local path dir rc
  dir=$(cd "$TEST_ROOT" && pwd -P)
  path=$(ts_start tssave "$dir") || return 1
  "$HOLD_BIN" send tssave 'cd /' --enter >/dev/null || return 1
  sleep 0.3
  rc=0; HOLD_TEST_TERM_WRITE_FAILS=1 "$HOLD_BIN" save tssave 2>"$TEST_ROOT/tssave.err" || rc=$?
  [ "$rc" -ne 0 ] && grep -q "cannot save tssave: its terminal's state could not be written" "$TEST_ROOT/tssave.err" ||
    { echo "save: rc=$rc" >&2; cat "$TEST_ROOT/tssave.err" >&2; return 1; }
  ! grep -q '"saved": true' "$path" || { echo "a failed save marked the process saved" >&2; return 1; }
  "$HOLD_BIN" save tssave 2>/dev/null || return 1
  grep -q '"saved": true' "$path" && ts_is "$path" / idle "" || { echo "the save after did not keep it" >&2; return 1; }
  "$HOLD_BIN" end tssave >/dev/null 2>&1 || true
}
