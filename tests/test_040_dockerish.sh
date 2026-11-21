#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./callbox-dynamic}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

TEST_ROOT="$(mktemp -d /tmp/callbox-040.XXXXXX)"
cleanup() {
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" end web --all >/dev/null 2>&1 || true
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" end port-smoke >/dev/null 2>&1 || true
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

export HOME="$TEST_ROOT/home"
export HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
export HOLD_TEST_INVOKING_HOME="$TEST_ROOT/home"
mkdir -p "$HOME"

# Where calls land: root uses the system store (runs/ and logs/ under its
# base), everyone else the user store (records and logs side by side).
if [ "$(id -u)" -eq 0 ]; then
  REC_DIR="$HOLD_TEST_SYSTEM_STATE_DIR/runs"
  LOG_DIR="$HOLD_TEST_SYSTEM_STATE_DIR/logs"
else
  REC_DIR="$HOME/.local/state/callbox"
  LOG_DIR="$HOME/.local/state/callbox"
fi

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

callbox() {
  "$HOLD_REAL_BIN" "$@"
}

extract_full64() {
  sed -n '/^[0-9a-f]\{64\}$/p' | head -n1
}

# Bare foreground: stream the process output, print no id and no startup note.
out=$(callbox -- /bin/sh -c 'echo smoke-out; echo smoke-err >&2' 2>&1)
printf '%s\n' "$out" | grep -qx 'smoke-out' || fail "foreground stdout missing"
printf '%s\n' "$out" | grep -qx 'smoke-err' || fail "foreground stderr missing"
if printf '%s\n' "$out" | grep -q 'callbox  started'; then
  fail "foreground call printed a startup note"
fi
if printf '%s\n' "$out" | grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$'; then
  fail "foreground call printed an id line"
fi
full=$(basename "$(ls "$LOG_DIR/"*.log | head -n1)" .log)
[ "${#full}" -eq 64 ] || fail "full 64-hex call id missing from store"
display=${full:0:12}
log="$LOG_DIR/$full.log"
grep -qx 'smoke-out' "$log" || fail "stdout raw log line missing"
test ! -e "$log.idx" || fail "a log index sidecar was written"
callbox logs --plain "$display" | grep -qx 'smoke-out' || fail "logs --plain did not print raw log"
callbox logs -p "$display" | grep -qx 'smoke-out' || fail "logs -p did not print raw log"

# Named detached call, then redial by name reuses the same call id and log.
named=$(callbox -d --name named-smoke -- /bin/sh -c 'echo named; sleep 0.1' 2>&1 | extract_full64 | cut -c1-12)
[ "${#named}" -eq 12 ] || fail "named call id missing"
sleep 0.3
redial_id=$(callbox -d named-smoke 2>&1 | extract_full64 | cut -c1-12) || fail "redial by call name failed"
[ "$redial_id" = "$named" ] || fail "redial did not reuse existing call id"
sleep 0.3
[ "$(callbox logs --plain named-smoke | grep -c '^named$')" -eq 2 ] ||
  fail "redial did not append to existing raw log"

# rename gives a call a meaningful name.
callbox rename named-smoke renamed-smoke >/dev/null 2>&1 || fail "rename failed"
callbox ps -a | grep -q 'renamed-smoke' || fail "rename did not update the call name"

if callbox -p 8080:80 -- /bin/true >/tmp/callbox-pub.out 2>/tmp/callbox-pub.err; then
  fail "publish unexpectedly succeeded"
fi
grep -q 'does not publish or forward ports' /tmp/callbox-pub.err ||
  fail "publish rejection message missing"
if callbox -v "$TEST_ROOT:/work" -- /bin/true >/tmp/callbox-vol.out 2>/tmp/callbox-vol.err; then
  fail "volume unexpectedly succeeded"
fi
grep -q 'does not mount or remap volumes' /tmp/callbox-vol.err ||
  fail "volume rejection message missing"

server=$(callbox -d --name port-smoke -- python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); time.sleep(5)' 2>&1 | extract_full64 | cut -c1-12)
[ "${#server}" -eq 12 ] || fail "server id missing"
for _ in $(seq 1 30); do
  psout=$(callbox ps 2>/dev/null || true)
  if printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp'; then
    break
  fi
  sleep 0.1
done
printf '%s\n' "$psout" | grep -q '^CALL ID[[:space:]]\+COMMAND[[:space:]]\+CREATED[[:space:]]\+STATUS[[:space:]]\+PORTS[[:space:]]\+NAMES' ||
  fail "ps header is not Docker-shaped"
# Port observation reads /proc: Linux-only (macOS shows an honest empty PORTS).
if [ "$(uname -s)" = Linux ]; then
  printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp' ||
    fail "observed listening port missing from ps"
fi
callbox end port-smoke >/dev/null 2>&1 || true

callbox attach -h 2>&1 | grep -q 'usage: callbox attach <target>' || fail "attach help missing"

rmout=$(callbox -d --rm -- /bin/sh -c 'echo rm; sleep 0.1' 2>&1)
rmfull=$(printf '%s\n' "$rmout" | extract_full64)
[ "${#rmfull}" -eq 64 ] || fail "--rm full id missing"
for _ in $(seq 1 50); do
  [ ! -e "$REC_DIR/$rmfull.json" ] && [ ! -e "$LOG_DIR/$rmfull.log" ] && break
  sleep 0.1
done
[ ! -e "$REC_DIR/$rmfull.json" ] || fail "--rm record remained"
[ ! -e "$LOG_DIR/$rmfull.log" ] || fail "--rm log remained"

echo "PASS: callbox surface smoke"
