#!/usr/bin/env bash
set -Eeuo pipefail

# The test build (make callbox-test, or make test), never the release
# binary: only a -DHOLD_TESTING build honors HOLD_TEST_SYSTEM_STATE_DIR, so
# the sudo tests would otherwise write into the machine's real system store.
HOLD_REAL_BIN="${HOLD_BIN:-./callbox-test}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

FAILS=0
SUITE_ROOT="$(mktemp -d)"
chmod 755 "$SUITE_ROOT"
TEST_ROOT=""
HOME=""
HOLD_TEST_SYSTEM_STATE_DIR=""
ROOT_HOME=""
ACTOR_HOME=""
TEST_USER=""
TEST_UID=""
TEST_GID=""
USER_ACTOR_NEEDS_SUDO=0
ROOT_ACTOR_AVAILABLE=0
SUDO_BIN="$(command -v sudo || true)"
USER_CREATED=0
ROOT_SAFE_HOLD_DIR=""
HOLD_TEST_TIMEOUT="${HOLD_TEST_TIMEOUT:-25}"

# util-linux script(1) and BSD/macOS script(1) disagree on flags; detect once.
SCRIPT_FLAVOR=""
if command -v script >/dev/null 2>&1; then
  if script -qfec true /dev/null </dev/null >/dev/null 2>&1; then
    SCRIPT_FLAVOR=linux
  else
    SCRIPT_FLAVOR=bsd
  fi
fi

pty_run() {
  if [ "$SCRIPT_FLAVOR" = linux ]; then
    script -qfec "$1" /dev/null
  else
    script -q /dev/null /bin/sh -c "$1"
  fi
}

PASSES=0
SKIPS=0
pass() { echo "PASS: $1"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }
skip_note() { echo "SKIP: $1"; SKIPS=$((SKIPS + 1)); }
# Call `skip "<reason>"` from inside a test to report SKIP instead of a vacuous
# PASS. It returns sentinel 77, which run_test maps to SKIP (or to FAIL when
# HOLD_REQUIRE_ROOT_TESTS=1, so CI cannot silently no-op the root lane).
skip() { [ -n "${1:-}" ] && echo "  (skipped: $1)" >&2; return 77; }
# Tests of the Unix-socket console's own mechanics (socket files, raw socket
# clients) do not apply when the suite runs with CALLBOX_CONSOLE_TRANSPORT=fifo;
# the FIFO console has its own tests below.
socket_console_only() {
  [ "${CALLBOX_CONSOLE_TRANSPORT:-}" != fifo ] || { skip "socket console only"; return; }
  [ -z "${HOLD_TEST_SOCKETS_DENIED:-}" ] || { skip "Unix sockets are denied in this run"; return; }
}
# An optional program (vim, tmux, ...) is missing: always a plain skip, even
# under HOLD_REQUIRE_ROOT_TESTS=1, which exists to catch skipped root tests.
skip_tool() { echo "  (skipped: $1)" >&2; return 78; }
# A scratch directory the acting user owns: when the suite runs as root, callbox
# (and the programs it holds) run as an unprivileged test user who cannot
# write into the root-owned $TEST_ROOT.
actor_dir() {
  local d="$TEST_ROOT/actor"
  mkdir -p "$d" || return 1
  if [ "${USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then chown "$TEST_UID:$TEST_GID" "$d" || return 1; fi
  printf '%s' "$d"
}

# Refuse to run vacuously: a missing core tool (e.g. ps from procps) must be a
# hard, visible failure, never a quietly-green test.
require_tools() {
  local missing="" t
  for t in ps stat sed awk grep find mktemp head tr cut id chmod kill; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  if [ -n "$missing" ]; then
    echo "FATAL: required test tools missing:$missing" >&2
    echo "  install them (e.g. 'procps' provides ps) and re-run; the suite refuses to pass vacuously." >&2
    exit 1
  fi
  case "$HOLD_TEST_TIMEOUT" in
    ''|*[!0-9]*|0)
      echo "FATAL: HOLD_TEST_TIMEOUT must be a positive integer number of seconds" >&2
      exit 1
      ;;
  esac
}

suite_cleanup() {
  set +e
  if [ "$USER_CREATED" -eq 1 ] && [ -n "$TEST_USER" ]; then
    userdel "$TEST_USER" >/dev/null 2>&1 || true
  fi
  if [ -n "$ROOT_SAFE_HOLD_DIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then
      rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    elif [ -n "$SUDO_BIN" ]; then
      "$SUDO_BIN" -n rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    fi
  fi
  remove_tree "$SUITE_ROOT"
}
trap suite_cleanup EXIT

remove_tree() {
  local path="$1"
  [ -n "$path" ] || return 0
  [ -e "$path" ] || return 0

  # Fast path: almost every test tree is user-owned and removable as-is. Avoid
  # recursive chmod/chown unless cleanup actually hits root-owned leftovers.
  rm -rf "$path" 2>/dev/null || true
  [ ! -e "$path" ] || [ -L "$path" ] || {
    if [ "$(id -u)" -eq 0 ]; then
      chmod -R u+rwX "$path" 2>/dev/null || true
      rm -rf "$path" || true
    elif [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -n "$SUDO_BIN" ]; then
      # Root-owned artifacts are expected only in the synthetic system store
      # and root-home fixtures. Normalize those directories instead of walking
      # the whole temp tree on every cleanup.
      while IFS= read -r root_owned_dir; do
        "$SUDO_BIN" -n chmod -R u+rwX "$root_owned_dir" 2>/dev/null || true
        "$SUDO_BIN" -n chown -R "$(id -u):$(id -g)" "$root_owned_dir" 2>/dev/null || true
      done < <(find "$path" -maxdepth 3 -type d \( -name system -o -name root-home \) -print 2>/dev/null || true)
      rm -rf "$path" 2>/dev/null || "$SUDO_BIN" -n rm -rf "$path" || true
    else
      rm -rf "$path" || true
    fi
  }
  return 0
}

setup_suite_actors() {
  if [ "$(id -u)" -eq 0 ]; then
    USER_ACTOR_NEEDS_SUDO=1
    ROOT_ACTOR_AVAILABLE=1
    TEST_USER="holdtest_$$"
    ACTOR_HOME="$SUITE_ROOT/user-home"
    mkdir -p "$ACTOR_HOME" || return 1
    if ! id "$TEST_USER" >/dev/null 2>&1; then
      useradd -M -d "$ACTOR_HOME" -s /bin/sh "$TEST_USER" || return 1
      USER_CREATED=1
    fi
    TEST_UID="$(id -u "$TEST_USER")"
    TEST_GID="$(id -g "$TEST_USER")"
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
    # The test user must be able to execute the binary under test, but the
    # checkout may sit in a home it cannot traverse (/home/runner is 0750 on
    # GitHub runners): run a world-readable copy instead.
    if ! sudo -n -u "$TEST_USER" test -x "$HOLD_REAL_BIN" 2>/dev/null &&
       ! su -s /bin/sh "$TEST_USER" -c "test -x '$HOLD_REAL_BIN'" 2>/dev/null; then
      cp "$HOLD_REAL_BIN" "$SUITE_ROOT/callbox-under-test" || return 1
      chmod 755 "$SUITE_ROOT/callbox-under-test" || return 1
      HOLD_REAL_BIN="$SUITE_ROOT/callbox-under-test"
    fi
  else
    USER_ACTOR_NEEDS_SUDO=0
    TEST_USER="$(id -un)"
    TEST_UID="$(id -u)"
    TEST_GID="$(id -g)"
    if [ -n "$SUDO_BIN" ] && "$SUDO_BIN" -n true >/dev/null 2>&1; then
      ROOT_ACTOR_AVAILABLE=1
    else
      ROOT_ACTOR_AVAILABLE=0
    fi
  fi
  export HOLD_REAL_BIN SUDO_BIN TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE
}

as_user() {
  local env_args
  env_args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    CALLBOX_CONSOLE_TRANSPORT \
    CALLBOX_ID \
    CALLBOX_NAME \
    CALLBOX_PARENT \
    TERM TMUX TMUX_PANE STY TERM_PROGRAM; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${env_args[@]}" "$@"
  else
    env "${env_args[@]}" "$@"
  fi
}

as_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    CALLBOX_CONSOLE_TRANSPORT; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

as_sudo_from_user() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "SUDO_UID=$TEST_UID"
    "SUDO_GID=$TEST_GID"
    "SUDO_USER=$TEST_USER"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    CALLBOX_CONSOLE_TRANSPORT; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

# (The terminal variables pass through so the TERM test sees what it set:
# under the root lane this wrapper goes through sudo -u, which resets the
# environment to its own short list.)
write_user_actor_wrapper() {
  cat > "$TEST_ROOT/user-callbox" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
env_args=(
  "HOME=$HOLD_ACTOR_HOME"
  "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
  "HOLD_TEST_INVOKING_HOME=$HOLD_ACTOR_HOME"
  "PATH=$PATH"
)
for name in \
  HOLD_BOOT_ID_PATH \
  HOLD_TEST_HOME_ROOT \
  HOLD_TEST_FAIL_RECORD_WRITE \
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
  HOLD_FAKE_SUDO_ARGV \
  HOLD_FAKE_SUDO_RC \
  CALLBOX_CONSOLE_TRANSPORT \
  CALLBOX_ID \
  CALLBOX_NAME \
  CALLBOX_PARENT \
  TERM TMUX TMUX_PANE STY TERM_PROGRAM; do
  if [ "${!name+x}" = x ]; then
    env_args+=("$name=${!name}")
  fi
done
if [ "${HOLD_USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then
  exec "$HOLD_ACTOR_SUDO_BIN" -n -u "$HOLD_ACTOR_USER" env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
fi
exec env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
SH
  chmod +x "$TEST_ROOT/user-callbox" || return 1
  HOLD_BIN="$TEST_ROOT/user-callbox"
  export HOLD_BIN
}

new_env() {
  TEST_ROOT="$(mktemp -d "$SUITE_ROOT/test.XXXXXX")" || return 1
  chmod 755 "$TEST_ROOT" || return 1
  HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
  HOLD_BOOT_ID_PATH="$TEST_ROOT/boot_id"
  printf 'boot-main\n' >"$HOLD_BOOT_ID_PATH" || return 1
  ROOT_HOME="$TEST_ROOT/root-home"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR" "$ROOT_HOME" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" || return 1
  chmod 700 "$ROOT_HOME" || return 1

  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  else
    ACTOR_HOME="$TEST_ROOT/home"
    mkdir -p "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  fi

  HOME="$ACTOR_HOME"
  export HOME TEST_ROOT HOLD_TEST_SYSTEM_STATE_DIR HOLD_BOOT_ID_PATH ROOT_HOME ACTOR_HOME
  export HOLD_ACTOR_HOME="$ACTOR_HOME"
  export HOLD_ACTOR_USER="$TEST_USER"
  export HOLD_ACTOR_SUDO_BIN="$SUDO_BIN"
  export HOLD_USER_ACTOR_NEEDS_SUDO="$USER_ACTOR_NEEDS_SUDO"
  write_user_actor_wrapper
}

cleanup_env() {
  local store ids id sys_store
  store="$HOME/.local/state/callbox"
  if [ -d "$store" ]; then
    ids=$(find "$store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
    done
  fi
  sys_store="$HOLD_TEST_SYSTEM_STATE_DIR/runs"
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -d "$sys_store" ]; then
    ids=$(find "$sys_store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      as_root "$HOLD_REAL_BIN" kill "system:$id" >/dev/null 2>&1 || true
    done
  fi
  remove_tree "$TEST_ROOT"
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ] && [ -n "$ACTOR_HOME" ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
  fi
}

ensure_user_fixture_store() {
  mkdir -p "$HOME/.local/state/callbox" || return 1
  chmod 700 "$HOME" "$HOME/.local" "$HOME/.local/state" "$HOME/.local/state/callbox" 2>/dev/null || true
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$HOME/.local" || return 1
  fi
}

file_mode() {
  stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"
}

resolve_path() {
  local path="$1" dir base
  if command -v realpath >/dev/null 2>&1; then
    realpath "$path"
    return
  fi
  dir="$(dirname "$path")"
  base="$(basename "$path")"
  (cd "$dir" && printf '%s/%s\n' "$(pwd -P)" "$base")
}

root_file_exists() {
  as_root test -f "$1"
}

root_path_absent() {
  as_root sh -c '[ ! -e "$1" ]' sh "$1"
}

root_file_mode() {
  as_root stat -c '%a' "$1" 2>/dev/null || as_root stat -f '%Lp' "$1"
}

root_file_owner() {
  as_root stat -c '%u:%g' "$1" 2>/dev/null || as_root stat -f '%u:%g' "$1"
}

sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    printf '%s\n' 'missing sha256sum or shasum for SHA-256 test helper' >&2
    return 127
  fi
}


root_grep() {
  local pattern="$1" path="$2"
  as_root grep -F -q -- "$pattern" "$path"
}

terminate_pid_tree() {
  local pid="$1" signal_name="${2:-TERM}" child
  [ -n "$pid" ] || return 0
  for child in $(ps -eo pid=,ppid= 2>/dev/null | awk -v p="$pid" '$2 == p { print $1 }'); do
    terminate_pid_tree "$child" "$signal_name"
  done
  kill "-$signal_name" "$pid" 2>/dev/null || true
}

print_timeout_diagnostics() {
  local desc="$1" fn="$2"
  {
    echo "TIMEOUT: $fn ($desc) exceeded ${HOLD_TEST_TIMEOUT}s"
    echo "TEST_ROOT: ${TEST_ROOT:-}"
    echo "--- relevant ps output ---"
    ps -eo pid=,ppid=,pgid=,stat=,etime=,args= 2>/dev/null |
      awk -v root="${TEST_ROOT:-}" -v suite="$SUITE_ROOT" '
        NR == 1 || $0 ~ /callbox/ || (root != "" && index($0, root)) || index($0, suite) || $0 ~ /sleep|bash|sh/ {
          print
          shown++
          if (shown >= 120) exit
        }
      ' || true
    echo "--- bounded find listing for TEST_ROOT ---"
    if [ -n "${TEST_ROOT:-}" ] && [ -d "$TEST_ROOT" ]; then
      find "$TEST_ROOT" -maxdepth 4 -print 2>/dev/null | sort | head -n 200 || true
    else
      echo "(TEST_ROOT unavailable)"
    fi
  } >&2
}

run_test() {
  local desc="$1" fn="$2" rc="" pid deadline
  # HOLD_TEST_ONLY=<regex>: run only tests whose function or description match.
  if [ -n "${HOLD_TEST_ONLY:-}" ] && ! [[ "$fn $desc" =~ $HOLD_TEST_ONLY ]]; then return 0; fi
  echo "RUN: $desc"
  new_env || { fail "$desc"; return; }
  export HOME TEST_ROOT HOLD_BIN HOLD_REAL_BIN HOLD_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
  export TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE SUDO_BIN
  export HOLD_ACTOR_HOME HOLD_ACTOR_USER HOLD_ACTOR_SUDO_BIN HOLD_USER_ACTOR_NEEDS_SUDO
  set +e
  ( set -Eeuo pipefail; "$fn" ) &
  pid=$!
  deadline=$((SECONDS + HOLD_TEST_TIMEOUT))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
      print_timeout_diagnostics "$desc" "$fn"
      terminate_pid_tree "$pid" TERM
      sleep 0.5
      terminate_pid_tree "$pid" KILL
      wait "$pid" 2>/dev/null || true
      rc=124
      break
    fi
    sleep 0.1
  done
  if [ -z "$rc" ]; then
    wait "$pid"
    rc=$?
  fi
  set -e
  if [ "$rc" -eq 0 ]; then
    pass "$desc"
  elif [ "$rc" -eq 78 ]; then
    skip_note "$desc"
  elif [ "$rc" -eq 77 ]; then
    if [ "${HOLD_REQUIRE_ROOT_TESTS:-0}" -eq 1 ]; then
      fail "$desc (skipped, but HOLD_REQUIRE_ROOT_TESTS=1)"
    else
      skip_note "$desc"
    fi
  else
    fail "$desc"
  fi
  cleanup_env
}

setup_suite_actors
require_tools

extract_id() {
  sed -n -e '/^[0-9a-f]\{12\}$/p' -e '/^[0-9a-f]\{64\}$/p' -e 's/^callbox: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1 | cut -c1-12
}

find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3" path count
  [ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || return 1
  if [ -e "$store/$id$suffix" ]; then
    printf '%s\n' "$store/$id$suffix"
    return 0
  fi
  count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
  [ "$count" -eq 1 ] || return 1
  path=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null)
  [ -n "$path" ] || return 1
  printf '%s\n' "$path"
}

record_path() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}"
  find_prefixed_artifact "$store" "$id" ".json"
}

log_path() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}"
  find_prefixed_artifact "$store" "$id" ".log"
}

record_exists() {
  record_path "$1" "${2:-$HOME/.local/state/callbox}" >/dev/null 2>&1
}

log_exists() {
  log_path "$1" "${2:-$HOME/.local/state/callbox}" >/dev/null 2>&1
}

root_find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3"
  as_root sh -c '
store=$1
id=$2
suffix=$3
[ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || exit 1
if [ -e "$store/$id$suffix" ]; then
  printf "%s\n" "$store/$id$suffix"
  exit 0
fi
count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
[ "$count" -eq 1 ] || exit 1
find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null
' sh "$store" "$id" "$suffix"
}

root_record_path() {
  root_find_prefixed_artifact "$2" "$1" ".json"
}

root_log_path() {
  root_find_prefixed_artifact "$2" "$1" ".log"
}

root_record_exists() {
  root_record_path "$1" "$2" >/dev/null 2>&1
}

root_log_exists() {
  root_log_path "$1" "$2" >/dev/null 2>&1
}

record_pgid() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"pgid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_sid() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"sid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_name() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"name": "\([^"]*\)".*/\1/p' "$record" | head -n1
}

# Poll until a call's record reports the exited state, so purge/redial (which
# require an ended call) do not race the supervisor's finish write.
record_ended_soon() {
  local id="$1" store="${2:-$HOME/.local/state/callbox}" record tries
  for tries in $(seq 1 60); do
    record=$(record_path "$id" "$store") || return 1
    grep -q '"state": "exited"' "$record" && return 0
    # A plain -d call records no exit (nothing stays behind to): callbox's
    # observed state says when it has ended.
    if [ -z "${2:-}" ] && ! grep -q '"wait": true\|"console_sock"' "$record" &&
      "$HOLD_BIN" inspect "$id" 2>/dev/null | grep -q '^  "state": "exited"'; then return 0; fi
    sleep 0.05
  done
  return 1
}


pid_dead_enough() {
  local p="$1" st
  if ! kill -0 "$p" 2>/dev/null; then
    return 0
  fi
  st=$(ps -o stat= -p "$p" 2>/dev/null | tr -d ' ' | cut -c1 || true)
  [ "$st" = "Z" ] || ! kill -0 "$p" 2>/dev/null
}

pgid_terminated() {
  local g="$1" tries stats
  for tries in $(seq 1 40); do
    if ! kill -0 "-$g" 2>/dev/null; then
      return 0
    fi
    stats=$(ps -o stat= -g "$g" 2>/dev/null | tr -d " " || true)
    if [ -n "$stats" ] && ! printf "%s\n" "$stats" | grep -qv "^Z"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

path_absent_soon() {
  local path="$1" tries
  for tries in $(seq 1 40); do
    [ ! -e "$path" ] && return 0
    sleep 0.05
  done
  return 1
}

test_lifecycle() {
  local out id lines
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  # Detached bare form prints exactly the 64-hex call id, nothing else.
  printf '%s\n' "$out" | grep -Eqx '[0-9a-f]{64}' || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$id" | grep -Eq '^[0-9a-f]{12}$' || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up "
  "$HOLD_BIN" end "$id" >/dev/null
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" list -a | grep -Eq "^$id[[:space:]].*Exited"
  "$HOLD_BIN" purge >/dev/null
  lines=$("$HOLD_BIN" list -a | wc -l)
  [ "$lines" -eq 1 ]
}


test_kill_subcommand() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" kill "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_group_kill_children() {
  local out id pgid children
  out=$("$HOLD_BIN" -d bash -c 'sleep 600 & sleep 601 & wait' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  sleep 0.2
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  sleep 0.2
  for p in $children; do
    pid_dead_enough "$p" || return 1
  done
  return 0
}

test_exec_failure_no_record() {
  local rc count
  set +e
  "$HOLD_BIN" nonexistent_binary_xyz >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 127 ] || { echo "rc=$rc (want 127, command not found)" >&2; return 1; }
  if [ -d "$HOME/.local/state/callbox" ]; then
    count=$(find "$HOME/.local/state/callbox" -maxdepth 1 -type f -name '*.json' | wc -l)
  else
    count=0
  fi
  [ "$count" -eq 0 ] || return 1
  count=$(find "$HOME/.local/state/callbox" -maxdepth 1 -type f 2>/dev/null | wc -l)
  [ "$count" -eq 0 ]
}

test_fast_exit_record_exited() {
  local out id record
  out=$("$HOLD_BIN" -dw /bin/sh -c 'exit 7' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record=$(record_path "$id") || return 1
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"ended_at": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \\(7\\)"
}

test_exec_replacement_remains_controllable() {
  local out id pgid
  out=$("$HOLD_BIN" -d bash -c 'exec sleep 300' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_corrupt_record_handling() {
  ensure_user_fixture_store || return 1
  printf 'garbage\n' > "$HOME/.local/state/callbox/badbad00cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad00cafe' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad00cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/callbox/badbad00cafe0000000000000000000000000000000000000000000000000000.json" ]
}

test_deep_json_record_rejected_not_crashed() {
  ensure_user_fixture_store || return 1
  local json i
  json='{"junk":'
  for i in $(seq 1 80); do json="${json}["; done
  json="${json}0"
  for i in $(seq 1 80); do json="${json}]"; done
  json="${json},\"version\":1,\"id\":\"abc12445cafe\",\"pid\":12345,\"pgid\":12345,\"sid\":12345,\"start_unix_ns\":0,\"argv\":[\"x\"],\"cmdline_display\":\"x\",\"uid\":0,\"gid\":0,\"proc_starttime_ticks\":0,\"exe_dev\":0,\"exe_ino\":0}"
  printf '%s\n' "$json" > "$HOME/.local/state/callbox/abc12445cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12445cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12445cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_record_rejected() {
  ensure_user_fixture_store || return 1
  cat > "$TEST_ROOT/record-target.json" <<'JSON'
{"version":1,"id":"abc12545cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":12345,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  ln -s "$TEST_ROOT/record-target.json" "$HOME/.local/state/callbox/abc12545cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12545cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12545cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_log_rejected() {
  local out id log rc
  out=$("$HOLD_BIN" -d /bin/echo original-log-line 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.2
  [ -f "$log" ] || return 1
  printf 'symlink-secret\n' > "$TEST_ROOT/log-target" || return 1
  rm -f "$log" || return 1
  ln -s "$TEST_ROOT/log-target" "$log" || return 1
  set +e
  "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>"$TEST_ROOT/dump.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  ! grep -q 'symlink-secret' "$TEST_ROOT/dump.out" || return 1
  grep -q 'failed to open log' "$TEST_ROOT/dump.err"
}

test_invalid_pgid_record() {
  ensure_user_fixture_store || return 1
  cat > "$HOME/.local/state/callbox/abc12345cafe0000000000000000000000000000000000000000000000000000.json" <<'JSON'
{"version":1,"id":"abc12345cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":0,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12345cafe' "$TEST_ROOT/list.out"
}

test_orphan_log_cleanup() {
  ensure_user_fixture_store || return 1
  : > "$HOME/.local/state/callbox/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" || return 1
  : > "$HOME/.local/state/callbox/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" || return 1
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/callbox/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" ] && [ ! -e "$HOME/.local/state/callbox/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" ]
}

test_id_sanitization() {
  local rc
  for bad in '../../etc/passwd' 'AABBCC' 'hello!' ''; do
    set +e
    "$HOLD_BIN" stop "$bad" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 5 ] || return 1
  done
}

test_print_signal_output() {
  local out id pgid got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ]
  got=$("$HOLD_BIN" kill --print "$id") || return 1
  [ "$got" = "kill -KILL -- -$pgid" ]
}

test_signal_refuses_tampered_live_group_identity() {
  local out id rec real_pgid real_sid decoy_out decoy_id decoy_pgid decoy_sid rc
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  real_pgid=$(record_pgid "$id")
  real_sid=$(record_sid "$id")
  [ -n "$id" ] && [ -n "$real_pgid" ] && [ -n "$real_sid" ] || return 1

  decoy_out=$("$HOLD_BIN" -d sleep 300 2>&1) || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  decoy_id=$(printf '%s\n' "$decoy_out" | extract_id)
  decoy_pgid=$(record_pgid "$decoy_id")
  decoy_sid=$(record_sid "$decoy_id")
  [ -n "$decoy_id" ] && [ -n "$decoy_pgid" ] && [ -n "$decoy_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    [ -n "$decoy_id" ] && "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }
  [ "$decoy_pgid" != "$real_pgid" ] && [ "$decoy_sid" != "$real_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }

  rec=$(record_path "$id") || return 1
  cp "$rec" "$rec.bak" || return 1
  sed -i.tmp \
    -e "s/\"pgid\":[[:space:]]*[0-9][0-9]*/\"pgid\":$decoy_pgid/" \
    -e "s/\"sid\":[[:space:]]*[0-9][0-9]*/\"sid\":$decoy_sid/" \
    "$rec" || return 1
  rm -f "$rec.tmp"
  set +e
  "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/tampered-live-print.err"
  rc=$?
  set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
  [ "$rc" -eq 2 ] || { echo "--print on tampered live pgid/sid: rc=$rc (want 2 refused)" >&2; return 1; }
  grep -Eq 'process group/session differs|cannot be signaled' "$TEST_ROOT/tampered-live-print.err" || {
    cat "$TEST_ROOT/tampered-live-print.err" >&2
    return 1
  }
}

test_stop_multiple_ids() {
  local out1 out2 id1 id2 pgid1 pgid2 rc
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid1=$(record_pgid "$id1")
  pgid2=$(record_pgid "$id2")
  [ -n "$id1" ] && [ -n "$id2" ] && [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  set +e
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_argument_edges() {
  local rc out
  set +e
  printf '' | "$HOLD_BIN" >"$TEST_ROOT/bare-callbox.out" 2>"$TEST_ROOT/bare-callbox.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/bare-callbox.out" "$TEST_ROOT/bare-callbox.err" >&2; return 1; }
  grep -q 'PLACE A CALL' "$TEST_ROOT/bare-callbox.out" || { cat "$TEST_ROOT/bare-callbox.out" >&2; return 1; }
  set +e
  "$HOLD_BIN" purge -all >/dev/null 2>"$TEST_ROOT/prune-flag.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  grep -q "unknown flag '-all'" "$TEST_ROOT/prune-flag.err" || return 1
  set +e
  "$HOLD_BIN" end >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  "$HOLD_BIN" --help >/dev/null || return 1
  "$HOLD_BIN" help >/dev/null || return 1
  "$HOLD_BIN" help --help >/dev/null || return 1
  "$HOLD_BIN" help targets | grep -q 'call id, leading id prefix, call name, or the start of a name' || return 1
  "$HOLD_BIN" help scripting | grep -q 'Exit codes:' || return 1
  "$HOLD_BIN" end -h | grep -q 'usage: callbox end' || return 1
  out=$("$HOLD_BIN" --version) || return 1
  printf '%s\n' "$out" | grep -Eq '^(dev(-[0-9a-f]{7,40}(-dirty)?)?|[0-9a-f]{7,40}|v?[0-9]+\.[0-9]+\.[0-9]+.*)$'
  set +e
  "$HOLD_BIN" -l >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  set +e
  "$HOLD_BIN" --list >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  "$HOLD_BIN" -- sleep 1 >/dev/null
}

test_special_chars_args() {
  local out id json
  out=$("$HOLD_BIN" -d echo "hello world" "it's" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  [ -f "$json" ] || return 1
  grep -Fq '"hello world"' "$json"
  grep -Fq '"it'"'"'s"' "$json"
}

test_log_capture() {
  local out id log
  out=$("$HOLD_BIN" -d bash -c 'echo out; echo err >&2; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.4
  [ -f "$log" ] || return 1
  [ ! -e "$log.idx" ] || { echo "a log index sidecar was written" >&2; return 1; }
  ! grep -q '"log"' "$log" || { cat "$log" >&2; return 1; }
  grep -q '^out$' "$log" && grep -q '^err$' "$log" || return 1
  [ "$("$HOLD_BIN" logs "$id" | wc -l | tr -d ' ')" -eq 2 ] || return 1
  [ "$("$HOLD_BIN" logs "$id" -n 1 | wc -l | tr -d ' ')" -eq 1 ] || return 1
  [ -z "$("$HOLD_BIN" logs "$id" -n 0)" ] || return 1
  set +e; "$HOLD_BIN" logs "$id" --filter out >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "removed --filter flag: rc=$rc (want 1, a usage error)" >&2; return 1; }
}

# logs -n N prints exactly the newest N lines; -f follows to the end and
# drains the last output written before exit.
test_logs_tail_and_follow() {
  "$HOLD_BIN" -d --name lines -- /bin/sh -c 'i=1; while [ $i -le 50 ]; do echo "line $i"; i=$((i+1)); done; printf "no-newline-tail"' >/dev/null || return 1
  "$HOLD_BIN" wait lines --timeout 5000 >/dev/null || return 1
  [ "$("$HOLD_BIN" logs lines -n 3)" = "$(printf 'line 49\nline 50\nno-newline-tail')" ] || { "$HOLD_BIN" logs lines -n 3 | od -c | tail -3 >&2; return 1; }
  [ "$("$HOLD_BIN" logs lines --tail=1)" = "no-newline-tail" ] || return 1
  [ "$("$HOLD_BIN" logs lines -n 1000 | wc -l | tr -d ' ')" -eq 50 ] || return 1
  "$HOLD_BIN" -d --name follower -- /bin/sh -c 'echo first; sleep 1; echo last' >/dev/null || return 1
  out=$("$HOLD_BIN" logs follower -f) || return 1
  [ "$out" = "$(printf 'first\nlast')" ] || { printf '%s\n' "$out" >&2; return 1; }
}

test_start_follow_short_form() {
  local out
  # -f keeps the foreground call streaming; foreground prints no id of its own.
  out=$("$HOLD_BIN" -f bash -c 'echo follow-short' 2>&1) || return 1
  printf '%s\n' "$out" | grep -q 'follow-short'
}


test_tail_verb_existing_id() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'sleep 0.3; echo from_tail_id; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'from_tail_id'
}

test_persistent_stale_records() {
  local out id store bootfile oldboot list_out stale_id stale_log
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d bash -c 'echo stale-line; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  store="$HOME/.local/state/callbox"
  stale_log=$(log_path "$id" "$store") || return 1
  [ -f "$stale_log" ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list >/dev/null || return 1
  printf 'boot-b\n' >"$bootfile" || return 1
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale"
  set +e
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null 2>"$TEST_ROOT/stop.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" kill "$id" >/dev/null 2>"$TEST_ROOT/kill.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/print.err"
  [ "$?" -eq 2 ] || return 1
  set -e
  grep -q 'stale' "$TEST_ROOT/stop.err"
  grep -q 'stale' "$TEST_ROOT/kill.err"
  grep -q 'stale' "$TEST_ROOT/print.err"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/tail.out"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/dump.out"
  record_exists "$id" "$store" && log_exists "$id" "$store"
}


test_boot_unavailable_does_not_force_stale() {
  local out id bootfile list_out rc pgid got
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  rm -f "$bootfile"
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale" || return 1
  set +e
  got=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" 2>"$TEST_ROOT/missing-boot-print.err")
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "stop --print with missing boot source: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/missing-boot-print.err" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { printf 'got: %s\nwant: kill -TERM -- -%s\n' "$got" "$pgid" >&2; return 1; }
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null
}

test_leader_zombie_group_still_running() {
  local id list_out
  id=$("$HOLD_BIN" -d bash -c 'sleep 60 & exit 0' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  sleep 0.3
  list_out=$("$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
}

test_tail_finished_log_prints_existing_output() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'echo finished-tail-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'finished-tail-line'
}

test_console_does_not_require_external_attach_tool() {
  local fake_path out id store record sock
  fake_path="$TEST_ROOT/no-tools"
  mkdir -p "$fake_path" || return 1
  out=$(as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" --console /bin/sh -c 'read line; echo "native:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  # the program ends after the line: attach exits 8 ("the call ended")
  { printf 'ping\n' | as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" console "$id" >"$TEST_ROOT/console-native.out" 2>"$TEST_ROOT/console-native.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/console-native.out" "$TEST_ROOT/console-native.err" >&2
    return 1
  }
  grep -q 'native:ping' "$TEST_ROOT/console-native.out" || { cat "$TEST_ROOT/console-native.out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_reports_non_console_run() {
  local out id rc
  out=$("$HOLD_BIN" -d sleep 30 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rc=0; "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-none.out" 2>"$TEST_ROOT/console-none.err" || rc=$?
  # 10: the call exists but has no console (the wrong kind of call)
  [ "$rc" -eq 10 ] || { echo "rc=$rc (want 10)" >&2; return 1; }
  grep -q "has no console" "$TEST_ROOT/console-none.err" || { cat "$TEST_ROOT/console-none.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_console_round_trip_and_log_tee() {
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "got:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list >"$TEST_ROOT/console-list.out" || return 1
  grep -Eq "^$id[[:space:]].*Up " "$TEST_ROOT/console-list.out" || {
    cat "$TEST_ROOT/console-list.out" >&2
    return 1
  }
  # the program ends after the line: attach exits 8 ("the call ended")
  { printf 'ping\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console.out" 2>"$TEST_ROOT/console.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/console.out" "$TEST_ROOT/console.err" >&2
    return 1
  }
  grep -q 'got:ping' "$TEST_ROOT/console.out" || { cat "$TEST_ROOT/console.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'got:ping' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_exit_code_is_recorded() {
  local out id store record rc
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "console-exit:$line"; exit 7' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1

  set +e
  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-exit.out" 2>"$TEST_ROOT/console-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 8 ] || {
    echo "console attach rc=$rc (want 8: the call ended while attached)" >&2
    cat "$TEST_ROOT/console-exit.out" "$TEST_ROOT/console-exit.err" >&2
    return 1
  }
  grep -q 'console-exit:done' "$TEST_ROOT/console-exit.out" || { cat "$TEST_ROOT/console-exit.out" >&2; return 1; }

  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  ! grep -q '"ExitCode"' "$record" || { echo "legacy Docker key in native record" >&2; cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(7\)" || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
}

test_end_console_call_signals_target() {
  # 'callbox end' on a console call must end the HELD group, not the broker. The
  # target ignores SIGHUP so an incidental PTY hangup cannot masquerade as the
  # group signal: under the pre-anchoring bug the record named the broker, the
  # group survived, and the record fabricated Exited (0). No red test caught this
  # because the best-effort 'stop' cleanups elsewhere (2015-2037, 2068) silently
  # leaked the target; this test is the pin.
  local out id store record pgid sock got ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  # Target-anchoring: pid==pgid==sid names the held sh/sleep, not a callbox broker.
  # Under the bug the recorded group is the broker's and holds no sleep.
  ps -o args= -g "$pgid" >"$TEST_ROOT/end-signal-group.txt" 2>/dev/null || true
  grep -q 'sleep 45' "$TEST_ROOT/end-signal-group.txt" || {
    echo "record pgid $pgid is not the held group:" >&2
    cat "$TEST_ROOT/end-signal-group.txt" >&2
    return 1
  }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The honest --print surface names the real group.
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ] || {
    echo "stop --print: [$got] want [kill -TERM -- -$pgid]" >&2
    return 1
  }
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  # Poll the record; never grep list before this (the zombie window renders
  # Exited (0) transiently until the broker's mark lands with 143).
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(143\)" || { "$HOLD_BIN" ps -a >&2; return 1; }
}

test_end_console_call_escalates_to_kill() {
  # SPEC 'end: TERM, then KILL' holds for consoles too. The leader traps both TERM
  # and HUP and keeps a child alive across TERM, so only the KILL escalation can
  # bring the group down; the record must reach exit_code 137 (128+SIGKILL).
  local out id store record pgid ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" TERM HUP; while :; do sleep 30; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end escalation" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 137' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 137:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 9' "$record" || { cat "$record" >&2; return 1; }
}

test_console_broker_term_forwards_to_target() {
  # A raw SIGTERM to the broker pid itself (not routed through 'callbox end') must
  # forward to the held group and clean up the socket. The record no longer names
  # the broker, so without the forwarder a bare kill would orphan the target.
  local out id store record pgid sock broker ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The broker is the parent of the target group leader (it forked the target).
  broker=$(ps -o ppid= -p "$pgid" 2>/dev/null | tr -d ' ')
  [ -n "$broker" ] && [ "$broker" -gt 1 ] || { echo "could not find broker for pgid $pgid" >&2; return 1; }
  kill -TERM "$broker" || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived broker TERM" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143 after broker TERM:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
}

test_console_socket_lives_in_store_dir() {
  socket_console_only || return $?
  # The console socket must live inside the store's console directory, never in
  # /tmp, even though the harness's store path is longer than the AF_UNIX
  # sun_path limit (~104). This exercises the relative-bind path.
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do [ "$line" = noop ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  case "$sock" in
    # macOS TMPDIR round-trips through the /private symlink; accept both spellings.
    "$store/console/$id"*.sock|"/private$store/console/$id"*.sock) ;;
    *)
    echo "console socket not in store console dir (got: $sock)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
      ;;
  esac
  if [ ! -S "$sock" ]; then
    echo "no socket present at $sock" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The socket must be owner-only (0600): it is an exec channel into the run.
  sock_mode=$(file_mode "$sock") || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  if [ "$sock_mode" != 600 ]; then
    echo "console socket mode=$sock_mode (want 600)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The relative path must also be connectable when the store path is long.
  # (8: the program ends after the line, while attached)
  { printf 'noop\n' | "$HOLD_BIN" console "$id" >/dev/null 2>"$TEST_ROOT/console-store.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/console-store.err" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  return 0
}

test_console_target_runs_in_caller_cwd() {
  # Relative-bind temporarily chdir()s into the console dir to bind the socket;
  # it must restore the cwd so the launched process still runs in the caller's
  # working directory, not the console directory.
  local out id store logf workdir i ok
  workdir="$TEST_ROOT/console-cwd"
  mkdir -p "$workdir" || return 1
  : > "$workdir/sentinel-file" || return 1
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$workdir" || return 1
  fi

  out=$( cd "$workdir" && "$HOLD_BIN" --console /bin/sh -c \
    'if [ -e sentinel-file ]; then echo CWD_OK; else echo CWD_BAD; fi; sleep 30' 2>&1 ) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  logf=$(log_path "$id" "$store") || return 1
  ok=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if grep -q CWD_OK "$logf" 2>/dev/null; then ok=1; break; fi
    if grep -q CWD_BAD "$logf" 2>/dev/null; then ok=0; break; fi
    sleep 0.2
  done
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  if [ "$ok" -ne 1 ]; then
    echo "target did not run in caller cwd; log:" >&2
    cat "$logf" >&2 2>/dev/null || true
    return 1
  fi
  return 0
}

build_console_protocol_helper() {
  local helper="$TEST_ROOT/console_protocol_helper"
  [ -x "$helper" ] && { printf '%s\n' "$helper"; return 0; }
  cat >"$TEST_ROOT/console_protocol_helper.c" <<'EOF'
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}

static int write_frame(int fd, unsigned char type, const char *payload) {
  size_t len = payload ? strlen(payload) : 0;
  unsigned char header[3];
  if (len > UINT16_MAX) return -1;
  header[0] = type;
  header[1] = (unsigned char)((len >> 8) & 0xff);
  header[2] = (unsigned char)(len & 0xff);
  if (write_all(fd, header, sizeof(header)) != 0) return -1;
  return len == 0 ? 0 : write_all(fd, payload, len);
}

int main(int argc, char **argv) {
  if (argc != 4) return 64;
  const char *sock_path = argv[1];
  const char *input = argv[2];
  const char *expect = argv[3];

  /* Connect via a short name relative to the socket's directory, mirroring
   * callbox's own client, so a long store path stays within sun_path. */
  const char *slash = strrchr(sock_path, '/');
  if (!slash || !slash[1]) return 3;
  char dir[4096];
  size_t dlen = (size_t)(slash - sock_path);
  if (dlen >= sizeof(dir)) return 3;
  memcpy(dir, sock_path, dlen);
  dir[dlen] = '\0';
  const char *name = slash + 1;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 2;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(name) >= sizeof(addr.sun_path)) return 3;
  strcpy(addr.sun_path, name);
  if (chdir(dir) != 0) return 4;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 4;
  if (write_all(fd, "holdv1\0\0", 8) != 0) return 5;
  if (write_frame(fd, 'D', input) != 0) return 6;

  char seen[8192];
  size_t seen_len = 0;
  memset(seen, 0, sizeof(seen));
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sr < 0 && errno == EINTR) continue;
    if (sr <= 0) return 7;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 8;
    if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return 9;
    size_t copy = (size_t)n;
    if (copy > sizeof(seen) - seen_len - 1) copy = sizeof(seen) - seen_len - 1;
    memcpy(seen + seen_len, buf, copy);
    seen_len += copy;
    seen[seen_len] = '\0';
    if (strstr(seen, expect)) {
      if (strcmp(expect, "attach denied") == 0) {
        close(fd);
        return 0;
      }
      break;
    }
  }

  if (write_frame(fd, 'X', NULL) != 0) return 10;
  close(fd);
  return 0;
}
EOF
  "${CC:-cc}" -std=c99 -Wall -Wextra -Werror "$TEST_ROOT/console_protocol_helper.c" -o "$helper"
  printf '%s\n' "$helper"
}

test_console_rejects_unrelated_peer_uid_before_replay() {
  socket_console_only || return $?
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  [ -n "$SUDO_BIN" ] || skip "sudo unavailable"
  id nobody >/dev/null 2>&1 || skip "no nobody user"
  local attacker_uid
  attacker_uid=$(id -u nobody)
  [ "$attacker_uid" != "0" ] && [ "$attacker_uid" != "$TEST_UID" ] || skip "nobody is not an unrelated user"
  "$SUDO_BIN" -n -u nobody true >/dev/null 2>&1 || skip "cannot run as nobody"

  local out id record sock helper rc log
  out=$("$HOLD_BIN" --console /bin/sh -c 'echo replay-secret; while read line; do echo "got:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -S "$sock" ] || return 1
  log=$(log_path "$id") || return 1

  # Deliberately relax filesystem traversal and socket mode in this temp store
  # so the kernel accepts the unrelated connection attempt; the broker's peer
  # credential check must still reject it before replaying output or forwarding
  # input.
  chmod 0711 "$ACTOR_HOME" "$ACTOR_HOME/.local" "$ACTOR_HOME/.local/state" \
    "$HOME/.local/state/callbox" "$HOME/.local/state/callbox/console" || return 1
  chmod 0666 "$sock" || return 1

  helper=$(build_console_protocol_helper) || return 1
  mkdir -p "$TEST_ROOT/nobody-home" || return 1
  chmod 755 "$TEST_ROOT/nobody-home" || return 1
  set +e
  "$SUDO_BIN" -n -u nobody env HOME="$TEST_ROOT/nobody-home" "$helper" "$sock" 'steal
' 'attach denied' >"$TEST_ROOT/unauth-console.out" 2>"$TEST_ROOT/unauth-console.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/unauth-console.out" "$TEST_ROOT/unauth-console.err" >&2; return 1; }
  grep -q 'attach denied' "$TEST_ROOT/unauth-console.out" || { cat "$TEST_ROOT/unauth-console.out" >&2; return 1; }
  ! grep -q 'replay-secret' "$TEST_ROOT/unauth-console.out" || { echo "unauthorized attach received replay" >&2; return 1; }
  sleep 0.2
  ! grep -q 'got:steal' "$log" || { cat "$log" >&2; return 1; }

  # the program ends after the line: attach exits 8 ("the call ended")
  { printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/authorized-console.out" 2>"$TEST_ROOT/authorized-console.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/authorized-console.out" "$TEST_ROOT/authorized-console.err" >&2
    return 1
  }
  grep -q 'got:done' "$TEST_ROOT/authorized-console.out" || { cat "$TEST_ROOT/authorized-console.out" >&2; return 1; }
}

test_console_can_reattach_after_detach() {
  socket_console_only || return $?
  local out id store record sock helper
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do echo "seen:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }

  helper=$(build_console_protocol_helper) || return 1
  as_user "$helper" "$sock" 'first
' 'seen:first' >"$TEST_ROOT/console-first.out" 2>"$TEST_ROOT/console-first.err" || {
    cat "$TEST_ROOT/console-first.out" "$TEST_ROOT/console-first.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-first.out" || { cat "$TEST_ROOT/console-first.out" >&2; return 1; }

  # the program ends after the line: attach exits 8 ("the call ended")
  { printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-second.out" 2>"$TEST_ROOT/console-second.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/console-second.out" "$TEST_ROOT/console-second.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  grep -q 'seen:done' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'seen:first' "$logf" || { cat "$logf" >&2; return 1; }
  grep -q 'seen:done' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_save_marks_record_and_is_idempotent() {
  local id name out
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  [ -n "$name" ] || { echo "run has no generated name" >&2; return 1; }
  # First save: stdout empty, stderr confirms, record flagged.
  out=$("$HOLD_BIN" save "$id" 2>"$TEST_ROOT/save1.err") || { echo "save rc nonzero" >&2; cat "$TEST_ROOT/save1.err" >&2; return 1; }
  [ -z "$out" ] || { echo "save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "callbox: saved $id ($name)" "$TEST_ROOT/save1.err" || { cat "$TEST_ROOT/save1.err" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "record not marked saved" >&2; return 1; }
  # Second save is idempotent: still exit 0, same note, no stdout.
  out=$("$HOLD_BIN" save "$name" 2>"$TEST_ROOT/save2.err") || { echo "idempotent save rc nonzero" >&2; return 1; }
  [ -z "$out" ] || { echo "idempotent save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "callbox: saved $id ($name)" "$TEST_ROOT/save2.err" || { cat "$TEST_ROOT/save2.err" >&2; return 1; }
}

test_purge_sweep_skips_saved() {
  local saved_id plain_id
  saved_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  plain_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$saved_id" ] && [ -n "$plain_id" ] || return 1
  "$HOLD_BIN" save "$saved_id" >/dev/null 2>&1 || return 1
  record_ended_soon "$saved_id" || return 1
  record_ended_soon "$plain_id" || return 1
  "$HOLD_BIN" purge >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "sweeping purge removed a saved call" >&2; return 1; }
  ! record_exists "$plain_id" || { echo "sweeping purge left an unsaved ended call" >&2; return 1; }
  # -a (include stale) must also skip the saved call.
  "$HOLD_BIN" purge -a >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "purge -a removed a saved call" >&2; return 1; }
}

test_purge_targeted_refuses_saved_exact_wording() {
  local id name pfx rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  # Let the reaper's exit stamp land before saving, so save is the last writer
  # and the saved flag cannot be lost to the finish-write.
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  # Refusal by name: line 1 identifies by name, line 2 echoes the typed token.
  set +e; "$HOLD_BIN" purge "$name" >"$TEST_ROOT/refuse.out" 2>"$TEST_ROOT/refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "targeted refusal rc=$rc (want 2)" >&2; cat "$TEST_ROOT/refuse.err" >&2; return 1; }
  [ ! -s "$TEST_ROOT/refuse.out" ] || { echo "refusal wrote to stdout" >&2; return 1; }
  printf "callbox: '%s' is saved — purging a saved call requires --force\n  callbox purge %s --force\n" "$name" "$name" >"$TEST_ROOT/refuse.want"
  diff -u "$TEST_ROOT/refuse.want" "$TEST_ROOT/refuse.err" || { echo "refusal wording mismatch (by name)" >&2; return 1; }
  # Refusal by id prefix: line 2 echoes exactly the prefix the user typed.
  pfx=$(printf '%s' "$id" | cut -c1-6)
  set +e; "$HOLD_BIN" purge "$pfx" 2>"$TEST_ROOT/refuse2.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "prefix refusal rc=$rc (want 2)" >&2; return 1; }
  printf "callbox: '%s' is saved — purging a saved call requires --force\n  callbox purge %s --force\n" "$name" "$pfx" >"$TEST_ROOT/refuse2.want"
  diff -u "$TEST_ROOT/refuse2.want" "$TEST_ROOT/refuse2.err" || { echo "refusal wording mismatch (by prefix)" >&2; return 1; }
  record_exists "$id" || { echo "refused purge still removed the call" >&2; return 1; }
}

test_purge_force_removes_saved_ended() {
  local id name
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  # The copy-paste command from the refusal must succeed on an ended saved call.
  "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1 || { echo "force purge of saved ended call failed" >&2; return 1; }
  ! record_exists "$id" || { echo "force did not remove the saved call" >&2; return 1; }
}

test_purge_force_removes_live_saved() {
  local id name pgid
  id=$("$HOLD_BIN" -d sleep 300 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  pgid=$(record_pgid "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  # --force must end the live call and remove it despite the saved flag.
  # Retry briefly: under load the TERM->KILL escalation can outlive one
  # purge invocation by a scheduler beat; a real regression still fails
  # every attempt for the full window.
  local tries=0
  while :; do
    "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1
    if ! record_exists "$id" && pgid_terminated "$pgid"; then
      return 0
    fi
    tries=$((tries + 1))
    [ "$tries" -lt 20 ] || break
    sleep 0.1
  done
  ! record_exists "$id" || { echo "force did not remove the live saved call" >&2; return 1; }
  pgid_terminated "$pgid" || { echo "force purge left the process group alive" >&2; return 1; }
  return 1
}

test_rename_of_saved_call_keeps_protection() {
  local id newname rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" rename "$id" keptsafe >/dev/null 2>&1 || { echo "rename failed" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "rename cleared the saved flag" >&2; return 1; }
  set +e; "$HOLD_BIN" purge keptsafe 2>"$TEST_ROOT/rename-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed saved call not protected: rc=$rc" >&2; return 1; }
  grep -qF "callbox: 'keptsafe' is saved" "$TEST_ROOT/rename-refuse.err" || { cat "$TEST_ROOT/rename-refuse.err" >&2; return 1; }
  "$HOLD_BIN" purge keptsafe --force >/dev/null 2>&1 || true
}

test_rename_saves_an_unsaved_call() {
  local id rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Naming a call declares intent to keep it: rename implies save.
  "$HOLD_BIN" rename "$id" nowkept 2>"$TEST_ROOT/rename-save.err" || { echo "rename failed" >&2; return 1; }
  grep -qF "(saved)" "$TEST_ROOT/rename-save.err" || { cat "$TEST_ROOT/rename-save.err" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "rename did not save the call" >&2; return 1; }
  set +e; "$HOLD_BIN" purge nowkept 2>"$TEST_ROOT/rename-save-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed call not protected: rc=$rc" >&2; return 1; }
  "$HOLD_BIN" purge nowkept --force >/dev/null 2>&1 || true
}

test_redial_honors_recorded_foreground_mode() {
  local out
  # A brief tail on the recipe so the foreground stream is deterministically
  # observable before the child exits (siblings below do the same); the test
  # pins redial honoring the foreground MODE, not zero-latency capture.
  "$HOLD_BIN" --name fgredial -- /bin/sh -c 'echo fg-redial-hit; sleep 0.2' >/dev/null 2>&1 || return 1
  # Foreground recipe: redial streams the output and prints no id of its own.
  out=$("$HOLD_BIN" fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -q 'fg-redial-hit' || { echo "foreground redial did not stream output: [$out]" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "foreground redial printed a detached id" >&2; return 1; }
  # An explicit -d on the redial overrides the recipe and detaches.
  out=$("$HOLD_BIN" -d fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "-d override did not detach: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_detached_mode() {
  local id out
  id=$("$HOLD_BIN" -d --name dredial /bin/sh -c 'echo d-redial; sleep 0.1' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Detached recipe: bare redial detaches and prints the 64-hex id.
  out=$("$HOLD_BIN" dredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "detached redial did not print a bare id: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_console_mode() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  local sm id out
  pty_run "$HOLD_BIN -it --name itredial /bin/sh -c 'echo it-live-hit; sleep 0.4'" >/dev/null 2>&1 || true
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"tty": \(true\).*/\1/p' | head -n1)
  [ "$sm" = "true" ] || { echo "itredial recipe tty=$sm (want true)" >&2; return 1; }
  id=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"id": "\([0-9a-f]*\)".*/\1/p' | head -n1)
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Console recipe: bare redial replays on a PTY (attaches) rather than detaching.
  out=$(pty_run "$HOLD_BIN itredial" 2>&1) || true
  printf '%s\n' "$out" | grep -q 'it-live-hit' || { printf '%s\n' "$out" >&2; echo "console redial did not replay on a PTY" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "console redial detached instead of attaching" >&2; return 1; }
  record_ended_soon "$id" || return 1
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"tty": \(true\).*/\1/p' | head -n1)
  [ "$sm" = "true" ] || { echo "console redial rewrote recipe tty=$sm (want true)" >&2; return 1; }
}

test_prune_by_id() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d true 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/callbox"
  "$HOLD_BIN" prune "$id1" >/dev/null || return 1
  ! record_exists "$id1" "$store" && ! log_exists "$id1" "$store" || return 1
  record_exists "$id2" "$store" && log_exists "$id2" "$store"
}

test_prune_all_keeps_running() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/callbox"
  "$HOLD_BIN" prune all >/dev/null || return 1
  record_exists "$id1" "$store" || return 1
  ! record_exists "$id2" "$store" || return 1
  ! log_exists "$id2" "$store" || return 1
}

test_transactional_record_write_failure() {
  local rc pids
  set +e
  HOLD_TEST_FAIL_RECORD_WRITE=1 "$HOLD_BIN" -d bash -c 'exec -a hold_txn_test_sleep sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  # Detect a leaked process via ps (a required tool) rather than pgrep with
  # `|| true`, which would pass vacuously if pgrep were absent.
  pids=$(ps -eo pid=,args= | awk '/hold_txn_test_sleep/ && !/awk/ {print $1}')
  [ -z "$pids" ]
}


make_fake_sudo() {
  mkdir -p "$TEST_ROOT/fakebin" || return 1
  cat > "$TEST_ROOT/fakebin/sudo" <<'SH'
#!/usr/bin/env bash
: "${HOLD_FAKE_SUDO_ARGV:?}"
printf '%s\n' "$@" > "$HOLD_FAKE_SUDO_ARGV"
exit "${HOLD_FAKE_SUDO_RC:-77}"
SH
  chmod 755 "$TEST_ROOT/fakebin" "$TEST_ROOT/fakebin/sudo" || return 1
  export HOLD_FAKE_SUDO_ARGV="$TEST_ROOT/sudo.argv"
  : > "$HOLD_FAKE_SUDO_ARGV" || return 1
  chmod 0666 "$HOLD_FAKE_SUDO_ARGV" || return 1
  export HOLD_FAKE_SUDO_RC=77
  export PATH="$TEST_ROOT/fakebin:$PATH"
}

write_public_index_fixture() {
  local id="$1" state="${2:-running}" started="${3:-2026-06-15T18:42:11Z}" alias="${4:-}"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  local alias_json=""
  if [ -n "$alias" ]; then
    alias_json="\"alias\":\"$alias\","
  fi
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<JSON
{"id":"$id","root_managed":true,"requires_elevation":true,${alias_json}"state_hint":"$state","started_at":"$started","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
}


test_cap_flags_are_rejected() {
  # The grants-era capability flags were deleted; like -p/-v they are
  # rejected honestly rather than accepted and ignored.
  set +e
  "$HOLD_BIN" --cap-add NET_BIND_SERVICE -- true >/dev/null 2>"$TEST_ROOT/cap-add.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "cap-add: rc=$rc (want 1)" >&2; cat "$TEST_ROOT/cap-add.err" >&2; return 1; }
  grep -q 'does not manage capabilities' "$TEST_ROOT/cap-add.err" || { cat "$TEST_ROOT/cap-add.err" >&2; return 1; }
  set +e
  "$HOLD_BIN" --cap-drop=ALL -- true >/dev/null 2>"$TEST_ROOT/cap-drop.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "cap-drop: rc=$rc (want 1)" >&2; cat "$TEST_ROOT/cap-drop.err" >&2; return 1; }
  grep -q 'does not manage capabilities' "$TEST_ROOT/cap-drop.err" || { cat "$TEST_ROOT/cap-drop.err" >&2; return 1; }
}

test_docker_rm_removes_run_artifacts_after_exit() {
  local out id store json log
  store="$HOME/.local/state/callbox"
  out=$("$HOLD_BIN" -d --rm -- /bin/sh -c 'echo rm-gone; sleep 0.3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  log=$(log_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$json" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$log" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  sleep 0.1
  [ -f "$json" ] || { echo "--rm removed record before process exit" >&2; return 1; }
  path_absent_soon "$json" || { echo "--rm did not remove record $json" >&2; cat "$json" >&2 2>/dev/null || true; return 1; }
  path_absent_soon "$log" || { echo "--rm did not remove log $log" >&2; return 1; }
}

# --rm, Docker-style: kill/end remove the call before they return (the name is
# free at once), and wait reports the exit code even when removal won the race.
test_rm_removed_on_kill_end_and_wait_reports_code() {
  local f out
  for f in -d -dt; do
    "$HOLD_BIN" $f --rm --name rmk -- sleep 30 >/dev/null || return 1
    "$HOLD_BIN" kill rmk >/dev/null || return 1
    ! "$HOLD_BIN" list | grep -q ' rmk$' || { echo "$f: --rm call still listed after kill" >&2; return 1; }
    "$HOLD_BIN" $f --rm --name rmk -- sleep 30 >/dev/null || { echo "$f: name not free after kill" >&2; return 1; }
    "$HOLD_BIN" end rmk >/dev/null || return 1
    ! "$HOLD_BIN" list | grep -q ' rmk$' || { echo "$f: --rm call still listed after end" >&2; return 1; }
    # wait while it runs: the code, then the name is free
    "$HOLD_BIN" $f --rm --name rmw -- /bin/sh -c 'sleep 0.4; exit 3' >/dev/null || return 1
    out=$("$HOLD_BIN" wait rmw) || return 1
    [ "$out" = 3 ] || { echo "$f: wait printed '$out', want 3" >&2; return 1; }
    "$HOLD_BIN" $f --rm --name rmw -- true >/dev/null || { echo "$f: name not free after wait" >&2; return 1; }
    # wait after it was already removed: the exit it left behind
    "$HOLD_BIN" $f --rm --name rmg -- /bin/sh -c 'exit 7' >/dev/null || return 1
    sleep 1
    out=$("$HOLD_BIN" wait rmg) || { echo "$f: wait on a removed --rm call failed" >&2; return 1; }
    [ "$out" = 7 ] || { echo "$f: wait after removal printed '$out', want 7" >&2; return 1; }
  done
}

# A call's helpers (reaper, log pump, --rm watcher) leave the caller's session:
# an agent harness that SIGKILLs the caller's process group the moment the
# command returns must not lose the call's output, exit code, or --rm.
test_call_survives_caller_group_kill() {
  python3 - "$HOLD_BIN" <<'PY' || return 1
import os, signal, subprocess, sys
callbox = sys.argv[1]
pid = os.fork()
if pid == 0:
    os.setsid()  # a fresh group, like a harness running one tool call
    subprocess.run([callbox, "-dw", "--name", "grpk", "--", "/bin/sh", "-c", "sleep 0.4; echo late-line; exit 5"],
                   stdout=subprocess.DEVNULL)
    subprocess.run([callbox, "-d", "--rm", "--name", "grpr", "--", "/bin/sh", "-c", "sleep 0.4; exit 2"],
                   stdout=subprocess.DEVNULL)
    os.killpg(0, signal.SIGKILL)
os.waitpid(pid, 0)
PY
  [ "$("$HOLD_BIN" wait grpk --timeout 10000)" = 5 ] || { echo "exit code lost when the caller's group was killed" >&2; return 1; }
  "$HOLD_BIN" logs grpk | grep -qx late-line || { echo "output lost when the caller's group was killed" >&2; return 1; }
  [ "$("$HOLD_BIN" wait grpr --timeout 10000)" = 2 ] || { echo "--rm exit lost when the caller's group was killed" >&2; return 1; }
  sleep 0.5
  ! "$HOLD_BIN" list | grep -q ' grpr$' || { echo "--rm call left behind when the caller's group was killed" >&2; return 1; }
}

test_env_flag_is_recipe_data_only() {
  local out id store fake record log
  store="$HOME/.local/state/callbox"
  fake="$TEST_ROOT/fake-home"
  mkdir -p "$fake" || return 1
  out=$("$HOLD_BIN" -d -e HOME="$fake" -e HOLD_MARKER=recipe -- /bin/sh -c 'echo "home:$HOME marker:$HOLD_MARKER"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ -f "$record" ] || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ ! -d "$fake/.local/state/callbox" ] || { echo "-e HOME leaked into callbox's own environment" >&2; return 1; }
  record_ended_soon "$id" || return 1
  log=$(log_path "$id" "$store") || return 1
  grep -q "home:$fake marker:recipe" "$log" || { cat "$log" >&2; return 1; }
  out=$("$HOLD_BIN" -d -e PATH=/nonexistent -- sleep 0.1 2>&1) || { echo "-e PATH broke invoking-PATH command resolution" >&2; printf '%s\n' "$out" >&2; return 1; }
}


test_raw_start_does_not_steal_trailing_system() {
  local out id log
  out=$("$HOLD_BIN" -d sh -c 'printf "arg:%s\n" "$1"' sh --system 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  "$HOLD_BIN" logs --plain "$id" >"$TEST_ROOT/raw-start.log" || return 1
  grep -q '^arg:--system$' "$TEST_ROOT/raw-start.log"
}

test_public_root_index_list_is_redacted() {
  write_public_index_fixture abc12345cafe0000000000000000000000000000000000000000000000000000 running 2026-06-15T18:42:11Z || return 1
  # Public root-index rows appear with -a in redacted form: the USER and COMMAND
  # cells both read the literal "hidden" (exists-but-not-yours-to-see, not "-"),
  # and the STATUS reflects the projected state.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  # Projected rows speak the table's Docker phrasing: "Up <age>", not the raw hint.
  grep -Eq '^abc12345cafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]Up ' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_public_root_index_list_reads_projected_state() {
  local id=deadbeefcafe0000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{
  "id": "deadbeefcafe0000000000000000000000000000000000000000000000000000",
  "root_managed": true,
  "state_hint": "exited",
  "created_at": "2026-06-15T18:42:10Z",
  "started_at": "2026-06-15T18:42:11Z",
  "ended_at": "2026-06-15T18:42:12Z",
  "exit_code": 7,
  "running": false,
  "argv": ["secret"],
  "cmdline_display": "secret command"
}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  # A recorded exit reads like any call's ("Exited (7)"); USER and COMMAND
  # stay redacted to the literal "hidden". Ended public rows need -a.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^deadbeefcafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]Exited \(7\)([[:space:]]|$)' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `callbox list` is the ledger: a user's own calls, live and past, by default, with
# a USER column naming the caller; -l/--live narrows it to the running ones.
test_list_default_is_full_ledger_live_narrows() {
  local live_out ended_out live_id ended_id me
  me="$TEST_USER" # the acting user (as root, the suite acts as a test user)
  live_out=$("$HOLD_BIN" -d --name ledger_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -dw --name ledger_past /bin/sh -c 'exit 4' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  # Default view: the USER column, then both the live and the past call, each
  # attributed to the calling user.
  "$HOLD_BIN" list >"$TEST_ROOT/ledger.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]]+$me[[:space:]].*Up " "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$ended_id[[:space:]]+$me[[:space:]].*Exited \\(4\\)" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  # -l (and its --live long form) drop the past call, keep the live one.
  local flag
  for flag in -l --live; do
    "$HOLD_BIN" list "$flag" >"$TEST_ROOT/live.out" 2>&1 || return 1
    grep -Eq "^$live_id[[:space:]].*Up " "$TEST_ROOT/live.out" || { cat "$TEST_ROOT/live.out" >&2; return 1; }
    ! grep -q "^$ended_id" "$TEST_ROOT/live.out" || { echo "$flag still showed the past call" >&2; cat "$TEST_ROOT/live.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `callbox list <name>` narrows the ledger to the call whose NAME matches. Regression
# for the filter matching the deleted profile-era alias field, which nothing set,
# so a named lookup of a real call printed an empty table.
test_list_name_filter_matches_call_name() {
  local out webby_id other_id
  out=$("$HOLD_BIN" -d --name webby sleep 300 2>&1) || return 1
  webby_id=$(printf '%s\n' "$out" | extract_id); [ -n "$webby_id" ] || return 1
  out=$("$HOLD_BIN" -d --name other_call sleep 300 2>&1) || return 1
  other_id=$(printf '%s\n' "$out" | extract_id); [ -n "$other_id" ] || return 1
  "$HOLD_BIN" list webby >"$TEST_ROOT/named.out" 2>&1 || return 1
  grep -Eq "^$webby_id[[:space:]].*[[:space:]]webby$" "$TEST_ROOT/named.out" || { cat "$TEST_ROOT/named.out" >&2; return 1; }
  ! grep -q 'other_call' "$TEST_ROOT/named.out" || { echo "name filter leaked another call" >&2; cat "$TEST_ROOT/named.out" >&2; return 1; }
  "$HOLD_BIN" stop "$webby_id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$other_id" >/dev/null 2>&1 || true
}

# list -a renders the observed ports root projected into a global call's public
# entry, in the same redacted row (USER and COMMAND both "hidden").
test_list_all_renders_global_ports_projection() {
  local id=facefeed12340000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{"id":"facefeed12340000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_web","state_hint":"running","created_at":"2026-06-15T18:42:11Z","observed_ports":"127.0.0.1:8080/tcp, [::]:9090/tcp","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>&1 || return 1
  # The redacted row shows the generated name and the projected ports; USER and
  # COMMAND read "hidden" and the command line never appears.
  grep -Eq "^${id:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*127\\.0\\.0\\.1:8080/tcp.*global_web$" "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `callbox list -s/--system` (and the equivalent `--system list`) is exactly the
# redacted global view: no personal calls leak into it.
test_system_list_is_redacted_global_only() {
  local out personal_id gid=beadfeed56780000000000000000000000000000000000000000000000000000 form
  out=$("$HOLD_BIN" -d --name my_personal sleep 300 2>&1) || return 1
  personal_id=$(printf '%s\n' "$out" | extract_id); [ -n "$personal_id" ] || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"beadfeed56780000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_only","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # -s, --system, and the pre-command --system spelling all mean the same view.
  for form in "list -s" "list --system" "--system list"; do
    # shellcheck disable=SC2086
    "$HOLD_BIN" $form >"$TEST_ROOT/system-list.out" 2>&1 || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*global_only$" "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q "^$personal_id" "$TEST_ROOT/system-list.out" || { echo "form '$form' leaked a personal call" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q 'my_personal' "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$personal_id" >/dev/null 2>&1 || true
}

# ps is Docker's machine-wide running view: running calls from both scopes, the
# global ones redacted, no USER column; -a adds ended; non-Docker flags reject.
test_ps_is_docker_machine_wide_running_view() {
  local live_out live_id ended_out ended_id gid=c1cadafeed990000000000000000000000000000000000000000000000000000 rc
  live_out=$("$HOLD_BIN" -d --name ps_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -dw --name ps_past /bin/sh -c 'exit 2' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"c1cadafeed990000000000000000000000000000000000000000000000000000","root_managed":true,"name":"ps_global","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # ps: Docker's header (no USER column), running personal + global, past hidden.
  "$HOLD_BIN" ps >"$TEST_ROOT/ps.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]].*Up .*ps_live$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]].*ps_global$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q "^$ended_id" "$TEST_ROOT/ps.out" || { echo "ps default showed an ended call" >&2; cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/ps.out"
  # ps -a includes the ended personal call.
  "$HOLD_BIN" ps -a >"$TEST_ROOT/ps-a.out" 2>&1 || return 1
  grep -Eq "^$ended_id[[:space:]].*Exited \\(2\\)" "$TEST_ROOT/ps-a.out" || { cat "$TEST_ROOT/ps-a.out" >&2; return 1; }
  # ps speaks only Docker: -l and -s are rejected as unknown flags.
  local badflag
  for badflag in -l -s -u; do
    set +e; "$HOLD_BIN" ps "$badflag" >/dev/null 2>"$TEST_ROOT/ps-bad.err"; rc=$?; set -e
    [ "$rc" -eq 1 ] || { echo "ps $badflag: rc=$rc (want 1)" >&2; return 1; }
    grep -q "unknown flag '$badflag'" "$TEST_ROOT/ps-bad.err" || { cat "$TEST_ROOT/ps-bad.err" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `callbox purge -s` as a normal user re-execs through sudo (so sudo can prompt),
# passing exactly `<self> purge --system` (plus any state flags).
test_purge_system_reexecs_through_sudo() {
  make_fake_sudo || return 1
  export HOLD_FAKE_SUDO_RC=0
  # The re-exec replaces the process with (fake) sudo, which records its argv,
  # one entry per line: the self binary path, then purge, then --system.
  set +e
  "$HOLD_BIN" purge -s >/dev/null 2>&1
  set -e
  grep -qx 'purge' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing 'purge'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -qx -- '--system' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing '--system'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -q 'callbox' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing the self binary path" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  ! grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv carried an unrequested --all" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  # -a is forwarded as a state widener when requested.
  : > "$HOLD_FAKE_SUDO_ARGV"
  set +e
  "$HOLD_BIN" purge -s -a >/dev/null 2>&1
  set -e
  grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing forwarded '--all'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
}

# As root, `list` is the global working view; `list -a` also walks every user's
# personal store and adds a USER column so ownership is clear.
test_root_list_all_labels_owner_and_walks_homes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local homes="$TEST_ROOT/homes" uid owner
  uid=$(id -u); owner=$(id -un)
  # A fabricated personal store under a test home root, holding one call owned by
  # the invoking user (so its USER resolves via /etc/passwd). The record shape
  # mirrors the minimal valid record, with a live-looking pgid so it is a row.
  mkdir -p "$homes/somebody/.local/state/callbox" || return 1
  cat > "$homes/somebody/.local/state/callbox/aa11bb22cc330000000000000000000000000000000000000000000000000000.json" <<JSON
{"version":1,"id":"aa11bb22cc330000000000000000000000000000000000000000000000000000","pid":2,"pgid":2,"sid":2,"start_unix_ns":0,"argv":["peer-cmd"],"cmdline_display":"peer-cmd","uid":$uid,"gid":$uid,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0,"name":"peer_call"}
JSON
  chmod -R 755 "$homes" || return 1
  # A global call in the system store (root's own view).
  local out gid_call
  out=$(as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" -d --name global_root sleep 300 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  gid_call=$(printf '%s\n' "$out" | extract_id); [ -n "$gid_call" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Plain root list: the global call only, no personal rows.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list >"$TEST_ROOT/root-list.out" 2>&1 || return 1
  grep -Eq "^$gid_call[[:space:]].*global_root$" "$TEST_ROOT/root-list.out" || { cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  ! grep -q 'peer_call' "$TEST_ROOT/root-list.out" || { echo "plain root list walked personal stores" >&2; cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  # Root list -a: USER column (second, after CALL ID); the peer call attributed
  # to its record owner, the global call to its recorded invoking user. Under
  # sudo that is the invoking account; run directly as root it would be "root".
  # In both cases the label equals `id -un` in this harness, so assert that.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list -a >"$TEST_ROOT/root-list-a.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^aa11bb22cc33[[:space:]]+$owner[[:space:]].*peer_call$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^$gid_call[[:space:]]+$owner[[:space:]].*global_root$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" stop "system:$gid_call" >/dev/null 2>&1 || true
}

test_user_local_wins_over_public_root_collision() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  # A root-public entry that collides on the call's display prefix: the local
  # record must win so stop resolves it without a sudo re-exec.
  local pub_id="${id}0000000000000000000000000000000000000000000000000000"
  write_public_index_fixture "$pub_id" running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  [ ! -s "$HOLD_FAKE_SUDO_ARGV" ] || return 1
  pgid_terminated "$pgid" || return 1
  record_exists "$pub_id" "$HOLD_TEST_SYSTEM_STATE_DIR/public"
}

test_explicit_user_target() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" stop "user:$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_tail_ctrl_c_detaches_from_tail_and_keeps_run() {
  command -v setsid >/dev/null 2>&1 || skip "setsid not available"
  local out id pgid tail_pid rc
  out=$("$HOLD_BIN" -d bash -c 'while :; do echo tail-still-running; sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  setsid "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  sleep 0.3
  kill -INT "-$tail_pid" 2>/dev/null || kill -INT "$tail_pid" 2>/dev/null || true
  set +e
  wait "$tail_pid"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  kill -0 "-$pgid" 2>/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


# A plain call launched in the foreground (here -w) is followed, and the keys
# mean what they mean everywhere you are on a call: Ctrl-C goes to the
# program; Ctrl-P Ctrl-Q leaves it running and says how to follow it again.
# Driven from a -dt call, so the keys come from a real terminal.
test_foreground_keys_interrupt_or_detach() {
  "$HOLD_BIN" -dt --name kdrv -- "$HOLD_REAL_BIN" -w --name trapper -- sh -c 'trap "echo got-INT; exit 6" INT; echo armed; while :; do sleep 0.2; done' >/dev/null || return 1
  "$HOLD_BIN" wait kdrv --match armed --timeout 8000 >/dev/null || { "$HOLD_BIN" peek kdrv >&2; return 1; }
  "$HOLD_BIN" send kdrv -k C-c || return 1
  "$HOLD_BIN" wait trapper --exit --timeout 5000 >/dev/null || return 1
  out_has '"exit_code": 6' "$HOLD_BIN" inspect trapper || { echo "Ctrl-C did not reach the program" >&2; "$HOLD_BIN" peek kdrv >&2; return 1; }
  "$HOLD_BIN" -dt --name kdrv2 -- "$HOLD_REAL_BIN" -w --name keeper -- sh -c 'echo keeper-up; while :; do sleep 0.3; done' >/dev/null || return 1
  "$HOLD_BIN" wait kdrv2 --match keeper-up --timeout 8000 >/dev/null || return 1
  "$HOLD_BIN" send kdrv2 -k C-p -k C-q || return 1
  "$HOLD_BIN" wait kdrv2 --match 'keeper keeps running; follow it again: callbox --tail keeper' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek kdrv2 >&2; return 1; }
  out_has 'Up ' "$HOLD_BIN" list keeper || { echo "detaching stopped the call" >&2; return 1; }
  "$HOLD_BIN" end keeper kdrv kdrv2 >/dev/null 2>&1 || true
}

# Without a terminal, a SIGINT sent to callbox goes to the program, as
# docker run's sig-proxy does.
test_foreground_sigint_reaches_the_program() {
  command -v setsid >/dev/null 2>&1 || skip "setsid not available"
  local fg_pid rc
  setsid "$HOLD_BIN" -w --name proxied -- sh -c 'trap "exit 6" INT; while :; do sleep 0.2; done' </dev/null >/dev/null 2>&1 &
  fg_pid=$!
  "$HOLD_BIN" wait proxied --idle 300 --timeout 5000 >/dev/null 2>&1 || sleep 1
  sleep 0.5
  kill -INT "$fg_pid" 2>/dev/null || true
  set +e; wait "$fg_pid"; rc=$?; set -e
  [ "$rc" -eq 6 ] || { echo "SIGINT to callbox: rc=$rc (want the program's 6)" >&2; return 1; }
}

# A program placed with no session flags runs in user mode: attached on a
# PTY. From a script (no terminal) it still runs on its PTY, its output
# arrives as a stream (no screen repaint), and callbox exits as it did.
test_bare_launch_runs_on_a_pty() {
  local out rc=0
  out=$("$HOLD_BIN" --name bare1 -- sh -c 'tty; echo bare-out; exit 4' </dev/null) || rc=$?
  [ "$rc" -eq 4 ] || { echo "bare launch: rc=$rc (want 4)" >&2; return 1; }
  printf '%s\n' "$out" | grep -q '^/dev/' || { echo "not on a PTY: [$out]" >&2; return 1; }
  printf '%s\n' "$out" | grep -q 'bare-out' || { echo "output lost: [$out]" >&2; return 1; }
  ! printf '%s' "$out" | grep -q "$(printf '\033')\[2J" || { echo "a script got a screen repaint" >&2; return 1; }
  out_has '"tty": true' "$HOLD_BIN" inspect bare1 || return 1
}

# macOS boot ids: kern.bootsessionuuid, exact (macos-uuid-<UUID>/<sec>).
# A call recorded before it (macos-<sec>.<usec>, from kern.boottime, which
# macOS shifts when it adjusts the clock) is matched by boot time within
# 60 s, so it stays running across the upgrade and still takes a msg.
test_macos_boot_ids_uuid_and_older_records() {
  local bf="$HOLD_BOOT_ID_PATH"
  printf 'macos-1791613741.073254\n' > "$bf"
  "$HOLD_BIN" -dt --name olderboot -- sh -c 'echo ob-ready; while IFS= read -r l; do echo "got:$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait olderboot --match ob-ready --timeout 8000 >/dev/null || return 1
  printf 'macos-1791613741.139128\n' > "$bf"
  out_has '"state": "running"' "$HOLD_BIN" inspect olderboot || { echo "boot-time drift made an older call stale" >&2; return 1; }
  printf 'macos-uuid-AAAA-1111/1791613742\n' > "$bf"
  out_has '"state": "running"' "$HOLD_BIN" inspect olderboot || { echo "an older call went stale under the UUID id" >&2; return 1; }
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg olderboot 'still here' >/dev/null || { echo "msg refused a live older call" >&2; return 1; }
  "$HOLD_BIN" wait olderboot --regex 'got:.*still here' --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" -d --name uuidboot -- sleep 30 >/dev/null || return 1
  printf 'macos-uuid-AAAA-1111/1791613799\n' > "$bf"
  out_has '"state": "running"' "$HOLD_BIN" inspect uuidboot || { echo "boot-time drift counted against a UUID id" >&2; return 1; }
  printf 'macos-uuid-BBBB-2222/1791613742\n' > "$bf"
  out_has '"state": "stale"' "$HOLD_BIN" inspect uuidboot || { echo "another boot's UUID did not read as stale" >&2; return 1; }
  printf 'macos-uuid-BBBB-2222/1791700000\n' > "$bf"
  out_has '"state": "stale"' "$HOLD_BIN" inspect olderboot || { echo "a reboot did not make an older call stale" >&2; return 1; }
  printf 'macos-uuid-AAAA-1111/1791613742\n' > "$bf"
  "$HOLD_BIN" end olderboot uuidboot >/dev/null 2>&1 || true
}

# User mode at a terminal (a switchless launch): a message for the call goes
# to its chat window and is never typed into the program; send still types
# into it; redial brings the program back the same way.
test_user_mode_messages_go_to_chat() {
  "$HOLD_BIN" -dt --size 120 30 --name udrv -- "$HOLD_REAL_BIN" --name umode -- sh -c 'echo umode-ready; while IFS= read -r l; do echo "typed:$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait udrv --match umode-ready --timeout 8000 >/dev/null || { "$HOLD_BIN" peek udrv >&2; return 1; }
  out_has '"chat": true' "$HOLD_BIN" inspect umode || { echo "not in user mode" >&2; return 1; }
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg umode 'for the chat' || return 1
  "$HOLD_BIN" wait udrv --match 'chat: 1 new' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek udrv >&2; return 1; }
  if "$HOLD_BIN" peek umode | grep -q 'typed:'; then echo "the message was typed into the program" >&2; return 1; fi
  "$HOLD_BIN" send umode --text 'by send' -k Enter >/dev/null || return 1
  "$HOLD_BIN" wait umode --match 'typed:by send' --timeout 5000 >/dev/null || { echo "send did not reach the program" >&2; return 1; }
  "$HOLD_BIN" end umode udrv >/dev/null 2>&1 || true
  "$HOLD_BIN" -dt --size 120 30 --name udrv2 -- "$HOLD_REAL_BIN" umode >/dev/null || return 1
  "$HOLD_BIN" wait udrv2 --match umode-ready --timeout 8000 >/dev/null || { "$HOLD_BIN" peek udrv2 >&2; return 1; }
  out_has '"chat": true' "$HOLD_BIN" inspect umode || { echo "redial did not come back in user mode" >&2; return 1; }
  "$HOLD_BIN" end umode udrv2 >/dev/null 2>&1 || true
}

# -it is not user mode: attached on a PTY with no chat window, and a message
# for it is typed into the program, as for any call (an agent run with -it
# must get its messages).
test_it_launch_messages_are_typed() {
  "$HOLD_BIN" -dt --size 120 30 --name idrv -- "$HOLD_REAL_BIN" -it --name itcall -- sh -c 'echo it-ready; while IFS= read -r l; do echo "typed:$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait idrv --match it-ready --timeout 8000 >/dev/null || { "$HOLD_BIN" peek idrv >&2; return 1; }
  if "$HOLD_BIN" inspect itcall | grep -q '"chat": true'; then echo "-it got the chat window" >&2; return 1; fi
  out_has '"tty": true' "$HOLD_BIN" inspect itcall || return 1
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg itcall 'for the program' || return 1
  "$HOLD_BIN" wait itcall --match 'typed:[msg from outside callbox] for the program' --timeout 5000 >/dev/null ||
    { echo "the message was not typed into the -it program" >&2; "$HOLD_BIN" peek itcall >&2; return 1; }
  "$HOLD_BIN" end itcall idrv >/dev/null 2>&1 || true
}

# -i: the program's input is a pipe its broker holds, never your terminal.
# Piped input streams in and ends with EOF; the exit code comes back.
test_pipe_input_streams_and_ends() {
  local out rc=0
  out=$(printf 'b\na\nc\n' | "$HOLD_BIN" -i --name sorter -- sort) || rc=$?
  [ "$rc" -eq 0 ] || return 1
  [ "$(printf '%s' "$out" | tr '\n' ' ')" = "a b c" ] || { echo "sorted: [$out]" >&2; return 1; }
  out_has '"interactive": true' "$HOLD_BIN" inspect sorter || return 1
  if out_has '"tty": true' "$HOLD_BIN" inspect sorter; then echo "-i put the program on a PTY" >&2; return 1; fi
  rc=0
  printf 'x\n' | "$HOLD_BIN" -i -- sh -c 'read l; [ -t 0 ] && exit 9; exit 3' || rc=$?
  [ "$rc" -eq 3 ] || { echo "-i exit code: rc=$rc (want 3; 9 = stdin was a terminal)" >&2; return 1; }
}

# -i at a terminal: callbox edits each line (backspace works), Ctrl-P Ctrl-Q
# detaches leaving it running, callbox attach reconnects input and output,
# Ctrl-D is end of input, and Ctrl-C reaches a program on pipes as SIGINT.
test_pipe_input_keys_detach_and_attach() {
  "$HOLD_BIN" -dt --name pdrv -- "$HOLD_REAL_BIN" -i --name piper -- sh -c 'echo pipe-ready; while IFS= read -r l; do echo "got:$l"; done; echo eof-seen' >/dev/null || return 1
  "$HOLD_BIN" wait pdrv --match pipe-ready --timeout 8000 >/dev/null || { "$HOLD_BIN" peek pdrv >&2; return 1; }
  "$HOLD_BIN" send pdrv abc -k BSpace -k Enter || return 1
  "$HOLD_BIN" wait pdrv --match 'got:ab' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek pdrv >&2; return 1; }
  "$HOLD_BIN" send pdrv -k C-p -k C-q || return 1
  "$HOLD_BIN" wait pdrv --match 'detached from piper' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek pdrv >&2; return 1; }
  out_has 'Up ' "$HOLD_BIN" list piper || { echo "detaching stopped the call" >&2; return 1; }
  "$HOLD_BIN" -dt --name pdrv2 -- "$HOLD_REAL_BIN" attach piper >/dev/null || return 1
  sleep 0.5
  "$HOLD_BIN" send pdrv2 again -k Enter || return 1
  "$HOLD_BIN" wait pdrv2 --match 'got:again' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek pdrv2 >&2; return 1; }
  "$HOLD_BIN" send pdrv2 -k C-d || return 1
  "$HOLD_BIN" wait piper --exit --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs piper | grep -q 'eof-seen' || { echo "Ctrl-D was not end of input" >&2; return 1; }
  "$HOLD_BIN" -dt --name pdrv3 -- "$HOLD_REAL_BIN" -i --name pint -- sh -c 'trap "exit 5" INT; echo int-armed; while :; do sleep 0.2; done' >/dev/null || return 1
  "$HOLD_BIN" wait pdrv3 --match int-armed --timeout 8000 >/dev/null || return 1
  "$HOLD_BIN" send pdrv3 -k C-c || return 1
  "$HOLD_BIN" wait pint --exit --timeout 5000 >/dev/null || return 1
  out_has '"exit_code": 5' "$HOLD_BIN" inspect pint || { echo "Ctrl-C did not reach the program on pipes" >&2; return 1; }
  "$HOLD_BIN" end pdrv pdrv2 pdrv3 >/dev/null 2>&1 || true
}

# -di: in the background with input open; callbox attach types into it later.
test_pipe_input_detached_then_attached() {
  "$HOLD_BIN" -di --name later -- sh -c 'echo later-ready; while IFS= read -r l; do echo "got:$l"; done' >/dev/null || return 1
  "$HOLD_BIN" -dt --name ldrv -- "$HOLD_REAL_BIN" attach later >/dev/null || return 1
  "$HOLD_BIN" wait ldrv --match later-ready --timeout 8000 >/dev/null || { "$HOLD_BIN" peek ldrv >&2; return 1; }
  "$HOLD_BIN" send ldrv typed-later -k Enter || return 1
  "$HOLD_BIN" wait ldrv --match 'got:typed-later' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek ldrv >&2; return 1; }
  "$HOLD_BIN" end later ldrv >/dev/null 2>&1 || true
}


# -i never lets a program that does not read its input lock up its broker:
# output keeps flowing (a program that prints more than it reads), and peek
# answers while the program ignores a large piped input.
test_pipe_input_never_blocks_the_broker() {
  local n
  # (the feeder may die of SIGPIPE once the program has read all it will: not ours to judge)
  n=$(set +o pipefail; seq 1 20000 | "$HOLD_BIN" -i --name amp -- sh -c 'while read l; do echo $l $l $l $l $l $l $l $l; done' | wc -l)
  [ "$n" -eq 20000 ] || { echo "amplifier: $n of 20000 lines" >&2; return 1; }
  ( head -c 300000 /dev/zero | tr '\0' x | fold -w100 | "$HOLD_BIN" -i --name deaf -- sh -c 'echo deaf-up; sleep 3' >/dev/null 2>&1 ) &
  local i
  for i in $(seq 1 50); do "$HOLD_BIN" inspect deaf >/dev/null 2>&1 && break; sleep 0.1; done
  "$HOLD_BIN" wait deaf --match deaf-up --timeout 5000 >/dev/null || return 1
  out_has 'deaf-up' "$HOLD_BIN" peek deaf || { echo "peek did not answer while the program ignored its input" >&2; return 1; }
  wait
}

# -i: a program that stops reading (or closes stdin) early still reports its
# own exit code, and its output after that still arrives.
test_pipe_input_early_close_keeps_exit_code() {
  local rc=0
  # callbox's own status (PIPESTATUS[1]): the feeder dies of SIGPIPE once the
  # program has stopped reading, which is the point of the test.
  set +e
  seq 1 200000 | "$HOLD_BIN" -i -- head -2 >/dev/null; rc=${PIPESTATUS[1]}
  set -e
  [ "$rc" -eq 0 ] || { echo "head -2: rc=$rc" >&2; return 1; }
  set +e
  yes | "$HOLD_BIN" -i -- sh -c 'head -2 >/dev/null; exit 9'; rc=${PIPESTATUS[1]}
  set -e
  [ "$rc" -eq 9 ] || { echo "exit 9: rc=$rc" >&2; return 1; }
  rc=0
  local out
  out=$( (sleep 0.5; echo a; sleep 0.5; echo b) | "$HOLD_BIN" -i -- sh -c 'exec 0<&-; echo closed; sleep 1.5; echo still; exit 6') || rc=$?
  [ "$rc" -eq 6 ] || { echo "closed stdin: rc=$rc" >&2; return 1; }
  printf '%s\n' "$out" | grep -q still || { echo "output after closing stdin was lost: [$out]" >&2; return 1; }
}

# msg and send --enter complete a line for a program on pipes (a PTY turns
# Enter's carriage return into a newline; a pipe needs the newline itself).
test_pipe_input_enter_is_a_newline() {
  "$HOLD_BIN" -di --name liner -- sh -c 'while IFS= read -r l; do echo "got:[$l]"; done' >/dev/null || return 1
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg liner 'hello agent' || return 1
  "$HOLD_BIN" send liner sent-text --enter || return 1
  "$HOLD_BIN" wait liner --match 'got:[sent-text]' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek liner >&2; return 1; }
  "$HOLD_BIN" logs liner | grep -q 'hello agent\]' || { echo "msg did not end its line" >&2; "$HOLD_BIN" logs liner >&2; return 1; }
  "$HOLD_BIN" end liner >/dev/null 2>&1 || true
}

# Started with stdin closed, callbox never reads its own console as input;
# from a script, end of input reaches a PTY program (it finishes).
test_closed_or_empty_stdin_ends_input() {
  local out rc=0
  out=$("$HOLD_BIN" --name noin -- sh -c 'echo line1; read x; echo "read:[$x]"' <&- | tr -d '\r') || rc=$?
  [ "$rc" -eq 0 ] || { echo "stdin closed: rc=$rc" >&2; return 1; }
  [ "$(printf '%s\n' "$out" | grep -c line1)" -eq 1 ] || { echo "output came back as input: [$out]" >&2; return 1; }
  printf '%s\n' "$out" | grep -q 'read:\[\]' || { echo "the program saw no end of input: [$out]" >&2; return 1; }
  rc=0; "$HOLD_BIN" -i -- sh -c 'cat >/dev/null; exit 4' <&- || rc=$?
  [ "$rc" -eq 4 ] || { echo "-i stdin closed: rc=$rc" >&2; return 1; }
  rc=0; "$HOLD_BIN" cat </dev/null >/dev/null || rc=$?
  [ "$rc" -eq 0 ] || { echo "callbox cat </dev/null: rc=$rc" >&2; return 1; }
}

# A SIGINT sent to an attached client goes to the program (as docker run's
# sig-proxy does), for a PTY call and a call on pipes.
test_sigint_to_attached_client_reaches_program() {
  local m p rc
  for m in -t -i; do
    rc=0
    "$HOLD_BIN" $m --name "sig${m#-}" -- sh -c 'trap "exit 5" INT; echo armed; while :; do sleep 0.2; done' </dev/null >/dev/null 2>&1 &
    p=$!
    "$HOLD_BIN" wait "sig${m#-}" --match armed --timeout 5000 >/dev/null 2>&1 || sleep 1
    kill -INT "$p"
    wait "$p" || rc=$?
    [ "$rc" -eq 5 ] || { echo "$m: SIGINT to the client: rc=$rc (want 5)" >&2; return 1; }
  done
}

# A program that exits but leaves a background child behind has ended, with
# its own exit code - the recorded exit, not the live process group, decides.
test_exit_stamp_beats_a_leftover_child() {
  local rc=0
  "$HOLD_BIN" -i -- sh -c 'sleep 3 & echo hi; exit 2' </dev/null >/dev/null || rc=$?
  [ "$rc" -eq 2 ] || { echo "-i with a background child: rc=$rc (want 2)" >&2; return 1; }
}


# Run a command, sending it SIGTERM after N seconds if it is still going:
# macOS has no timeout(1). Its stdin is passed on (a background job's would
# be /dev/null). Returns the command's status.
term_after() {
  local secs=$1 pid killer rc=0
  shift
  "$@" <&0 &
  pid=$!
  ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null ) &
  killer=$!
  wait "$pid" || rc=$?
  kill "$killer" 2>/dev/null || true
  wait "$killer" 2>/dev/null || true
  return "$rc"
}

# A fast redial prints its own run's output once - never a protocol reply,
# never the earlier runs' output - and its record is this run's (the exit
# stamp of a program that ends at once must not put the old run back).
test_fast_redial_prints_its_own_run() {
  local i out rc
  "$HOLD_BIN" --name quick -- echo first </dev/null >/dev/null || return 1
  for i in 1 2 3 4; do
    rc=0
    out=$("$HOLD_BIN" quick </dev/null 2>&1 | tr -d '\r') || rc=$?
    [ "$rc" -eq 0 ] || { echo "redial $i: rc=$rc out=[$out]" >&2; return 1; }
    [ "$out" = "first" ] || { echo "redial $i printed [$out]" >&2; return 1; }
  done
  out_has '"log_start": 28' "$HOLD_BIN" inspect quick || { echo "the record is not the last run's" >&2; "$HOLD_BIN" inspect quick | grep log_start >&2; return 1; }
}

# `callbox foo | head`: callbox ends as a writer to a closed pipe does (141)
# and says the call keeps running.
test_closed_reader_ends_with_141() {
  local st
  set +e +o pipefail
  "$HOLD_BIN" --name gush -- seq 1 1000000 </dev/null 2>"$TEST_ROOT/gush.err" | head -2 >/dev/null
  st=${PIPESTATUS[0]}
  set -e -o pipefail
  [ "$st" -eq 141 ] || { echo "rc=$st (want 141)" >&2; cat "$TEST_ROOT/gush.err" >&2; return 1; }
  grep -q 'gush keeps running' "$TEST_ROOT/gush.err" || { cat "$TEST_ROOT/gush.err" >&2; return 1; }
  "$HOLD_BIN" end gush >/dev/null 2>&1 || true
}

# A SIGTERM to a launch (timeout 2 callbox foo) is passed on to its program,
# which is not left behind.
test_sigterm_to_a_launch_reaches_program() {
  set +e
  term_after 2 "$HOLD_BIN" --name termed -- sh -c 'trap "exit 7" TERM; while :; do sleep 0.2; done' </dev/null >/dev/null 2>&1
  set -e
  "$HOLD_BIN" wait termed --exit --timeout 5000 >/dev/null 2>&1 || true
  out_has 'Exited (7)' "$HOLD_BIN" list -a termed || { echo "the program did not get the SIGTERM" >&2; "$HOLD_BIN" list -a >&2; return 1; }
}

# A piped attach to a -di call does not end its input: the next attach can
# feed it again.
test_piped_attach_keeps_di_input_open() {
  "$HOLD_BIN" -di --name feed -- sh -c 'while IFS= read -r l; do echo "got:$l"; done; echo EOF-seen' >/dev/null || return 1
  set +e
  printf 'one\n' | term_after 2 "$HOLD_BIN" attach feed >/dev/null 2>&1
  printf 'two\n' | term_after 2 "$HOLD_BIN" attach feed >/dev/null 2>&1
  set -e
  "$HOLD_BIN" wait feed --match 'got:two' --timeout 5000 >/dev/null || { "$HOLD_BIN" logs feed >&2; return 1; }
  if "$HOLD_BIN" logs feed | grep -q EOF-seen; then echo "an attach ended the call's input" >&2; return 1; fi
  "$HOLD_BIN" end feed >/dev/null 2>&1 || true
}

# wait --match finds text that wrapped at the right edge.
test_wait_match_spans_a_soft_wrap() {
  "$HOLD_BIN" -dt --name narrow --size 20 5 -- sh -c 'printf "0123456789abcdefNEEDLEword\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait narrow --match NEEDLEword --timeout 3000 >/dev/null || { "$HOLD_BIN" peek narrow >&2; return 1; }
  "$HOLD_BIN" end narrow >/dev/null 2>&1 || true
}


# `callbox -w cmd | head`: quiet 141, as `logs | head` is, with a note
# that the call keeps running.
test_follow_closed_reader_ends_with_141() {
  local st
  set +e +o pipefail
  "$HOLD_BIN" -w --name wgush -- seq 1 1000000 </dev/null 2>"$TEST_ROOT/wgush.err" | head -1 >/dev/null
  st=${PIPESTATUS[0]}
  set -e -o pipefail
  [ "$st" -eq 141 ] || { echo "rc=$st (want 141)" >&2; cat "$TEST_ROOT/wgush.err" >&2; return 1; }
  if grep -q 'failed while tailing' "$TEST_ROOT/wgush.err"; then cat "$TEST_ROOT/wgush.err" >&2; return 1; fi
  "$HOLD_BIN" end wgush >/dev/null 2>&1 || true
}

# A foreground redial shows its own run, not every earlier run's output.
test_foreground_redial_shows_its_run_only() {
  local n
  "$HOLD_BIN" -w --name thrice -- seq 1 3 </dev/null >/dev/null || return 1
  n=$("$HOLD_BIN" thrice </dev/null | wc -l | tr -d ' ')
  [ "$n" -eq 3 ] || { echo "redial printed $n lines (want 3)" >&2; return 1; }
  n=$("$HOLD_BIN" thrice </dev/null | wc -l | tr -d ' ')
  [ "$n" -eq 3 ] || { echo "second redial printed $n lines (want 3)" >&2; return 1; }
}

# With no terminal on the client's side, the screen answers the program's
# terminal queries: a program waiting for its cursor position gets it.
test_stream_client_gets_query_replies() {
  local out
  out=$("$HOLD_BIN" --name dsr -- sh -c 'sleep 0.5; stty -icanon -echo min 0 time 30; printf "\033[6n"; dd bs=1 count=6 2>/dev/null | od -c' </dev/null 2>&1 | tr -d '\r')
  printf '%s' "$out" | grep -q '033   \[   1   ;   1' || { echo "no reply to the query: [$out]" >&2; return 1; }
}

# Purges at once each report only what they removed.
test_concurrent_purges_count_once() {
  local i total
  for i in $(seq 1 12); do "$HOLD_BIN" -d --name "pc$i" true >/dev/null; done
  "$HOLD_BIN" wait pc12 --exit --timeout 5000 >/dev/null 2>&1 || sleep 1
  sleep 0.5
  for i in 1 2 3 4; do "$HOLD_BIN" purge >"$TEST_ROOT/purge$i.out" 2>&1 & done
  wait
  total=$(cat "$TEST_ROOT"/purge[1-4].out | grep -c '  pc[0-9]*$' || true)
  [ "$total" -eq 12 ] || { echo "purges reported $total removals of 12" >&2; cat "$TEST_ROOT"/purge[1-4].out >&2; return 1; }
}


# An id prefix that matches several calls lists them and exits 6, for
# every verb - not "no call matches" (5), which would be false.
test_ambiguous_id_prefix_lists_and_exits_6() {
  local i c v rc
  for i in $(seq 1 20); do "$HOLD_BIN" -d true >/dev/null; done
  c=$(ls "$HOME/.local/state/callbox/"*.json | xargs -n1 basename | cut -c1 | sort | uniq -d | head -1)
  [ -n "$c" ] || { skip "no two ids share a first digit"; return; }
  for v in inspect logs end peek wait save purge; do
    rc=0; "$HOLD_BIN" "$v" "$c" >/dev/null 2>"$TEST_ROOT/amb.err" || rc=$?
    [ "$rc" -eq 6 ] || { echo "$v $c: rc=$rc (want 6)" >&2; cat "$TEST_ROOT/amb.err" >&2; return 1; }
    grep -q "more than one call matches '$c'" "$TEST_ROOT/amb.err" || { cat "$TEST_ROOT/amb.err" >&2; return 1; }
  done
  rc=0; "$HOLD_BIN" "$c" </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 6 ] || { echo "redial $c: rc=$rc (want 6)" >&2; return 1; }
}

# The start of a name is a target when one call fits; several are listed.
test_name_prefix_targets() {
  local rc
  "$HOLD_BIN" -d --name helper -- sh -c 'echo from-helper' >/dev/null
  "$HOLD_BIN" -d --name helium -- sh -c 'echo from-helium' >/dev/null
  "$HOLD_BIN" wait helper --exit --timeout 5000 >/dev/null 2>&1 || sleep 1
  out_has 'from-helper' "$HOLD_BIN" logs helpe || return 1
  rc=0; "$HOLD_BIN" logs hel >/dev/null 2>"$TEST_ROOT/np.err" || rc=$?
  [ "$rc" -eq 6 ] || { echo "logs hel: rc=$rc (want 6)" >&2; return 1; }
  grep -q helium "$TEST_ROOT/np.err" && grep -q helper "$TEST_ROOT/np.err" || { cat "$TEST_ROOT/np.err" >&2; return 1; }
}

# A top-level word that is no verb, call, or program may be the start of
# one verb or call name; a program on PATH still wins; several fits list.
test_shortened_top_level_words() {
  local rc bin
  "$HOLD_BIN" --name quokka -- sh -c 'echo quokka-ran' </dev/null >/dev/null
  out_has 'CALL ID' "$HOLD_BIN" li || { echo "li is not list" >&2; return 1; }
  out_has 'quokka-ran' "$HOLD_BIN" lo quokka || { echo "lo is not logs" >&2; return 1; }
  rc=0; "$HOLD_BIN" p >/dev/null 2>"$TEST_ROOT/sh.err" || rc=$?
  [ "$rc" -eq 6 ] || { echo "p: rc=$rc (want 6)" >&2; return 1; }
  grep -q 'peek' "$TEST_ROOT/sh.err" && grep -q 'purge' "$TEST_ROOT/sh.err" || { cat "$TEST_ROOT/sh.err" >&2; return 1; }
  # the start of a call's name redials it
  out_has 'quokka-ran' "$HOLD_BIN" quok </dev/null || { echo "quok did not redial quokka" >&2; return 1; }
  # a program on PATH beats the start of a verb
  bin="$TEST_ROOT/pathbin"; mkdir -p "$bin"
  printf '#!/bin/sh\necho i-am-li\n' > "$bin/li"; chmod +x "$bin/li"
  out_has 'i-am-li' env PATH="$bin:$PATH" "$HOLD_BIN" li </dev/null || { echo "li on PATH did not run" >&2; return 1; }
}

# Redialing a running call goes back to it: a console call is attached, a
# plain one has its log followed; -d prints its id. Nothing starts twice.
test_redial_running_call_goes_back_to_it() {
  local id out
  "$HOLD_BIN" -dt --name upcon -- sh -c 'echo con-up; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait upcon --match con-up --timeout 5000 >/dev/null || return 1
  out=$(term_after 3 "$HOLD_BIN" upcon </dev/null 2>&1 | tr -d '\r') || true
  printf '%s' "$out" | grep -q con-up || { echo "redial of a running console call did not attach: [$out]" >&2; return 1; }
  "$HOLD_BIN" -d --name uplog -- sh -c 'echo log-up; sleep 1; echo log-done' >/dev/null || return 1
  out=$("$HOLD_BIN" uplog </dev/null 2>&1) || true
  printf '%s' "$out" | grep -q log-done || { echo "redial of a running call did not follow its log: [$out]" >&2; return 1; }
  # (following starts at the log's end, as tail -f does; the log shows runs)
  [ "$("$HOLD_BIN" logs uplog | grep -c log-up)" -eq 1 ] || { echo "it ran twice" >&2; "$HOLD_BIN" logs uplog >&2; return 1; }
  id=$("$HOLD_BIN" -d upcon 2>/dev/null) || { echo "-d of a running call failed" >&2; return 1; }
  [ "${#id}" -eq 64 ] || { echo "-d printed [$id]" >&2; return 1; }
  "$HOLD_BIN" end upcon >/dev/null 2>&1 || true
}


# A command that cannot start exits as a shell's would, and leaves nothing.
test_cannot_start_exits_127_and_126() {
  local rc
  rc=0; "$HOLD_BIN" -d no-such-command-xyz >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 127 ] || { echo "not found: rc=$rc (want 127)" >&2; return 1; }
  rc=0; "$HOLD_BIN" no-such-command-xyz </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 127 ] || { echo "foreground not found: rc=$rc (want 127)" >&2; return 1; }
  printf 'echo hi\n' > "$TEST_ROOT/not-exec"; chmod 644 "$TEST_ROOT/not-exec"
  rc=0; "$HOLD_BIN" -d "$TEST_ROOT/not-exec" >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 126 ] || { echo "not executable: rc=$rc (want 126)" >&2; return 1; }
  if "$HOLD_BIN" list -a | grep -q 'no-such-command\|not-exec'; then echo "a failed start left a record" >&2; return 1; fi
}

# The same file is "permission denied" (126) by bare name on PATH as by its
# path, and under --restart, -d too: the first worker's exec is the launch.
test_cannot_start_126_by_path_and_restart() {
  local rc out
  mkdir -p "$TEST_ROOT/pbin"
  printf 'echo hi\n' > "$TEST_ROOT/pbin/not-exec-xyz"; chmod 644 "$TEST_ROOT/pbin/not-exec-xyz"
  rc=0; PATH="$TEST_ROOT/pbin:$PATH" "$HOLD_BIN" -d not-exec-xyz >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 126 ] || { echo "PATH, not executable: rc=$rc (want 126)" >&2; return 1; }
  rc=0; "$HOLD_BIN" --restart on-failure:0 "$TEST_ROOT/pbin/not-exec-xyz" </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 126 ] || { echo "--restart foreground: rc=$rc (want 126)" >&2; return 1; }
  rc=0; out=$("$HOLD_BIN" -d --restart on-failure:0 "$TEST_ROOT/pbin/not-exec-xyz" 2>/dev/null) || rc=$?
  [ "$rc" -eq 126 ] && [ -z "$out" ] || { echo "--restart -d: rc=$rc out=[$out] (want 126, no id)" >&2; return 1; }
  rc=0; "$HOLD_BIN" -d --restart always no-such-command-xyz >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 127 ] || { echo "--restart, not found: rc=$rc (want 127)" >&2; return 1; }
  # A PATH directory this user cannot enter says EACCES for any name; a name
  # in none of PATH is still not found (the root lane's PATH has /home/runner).
  mkdir -p "$TEST_ROOT/locked"; chmod 000 "$TEST_ROOT/locked"
  rc=0; PATH="$TEST_ROOT/locked:$PATH" "$HOLD_BIN" -d no-such-command-xyz >/dev/null 2>&1 || rc=$?
  chmod 700 "$TEST_ROOT/locked"
  [ "$rc" -eq 127 ] || { echo "behind a locked PATH dir, not found: rc=$rc (want 127)" >&2; return 1; }
  if "$HOLD_BIN" list -a | grep -q 'not-exec\|no-such-command'; then echo "a failed start left a record" >&2; return 1; fi
}

# Until callbox reports the named call ended (its observed state).
name_ended_soon() {
  local i
  for i in $(seq 1 100); do
    out_has '^  "state": "exited"' "$HOLD_BIN" inspect "$1" && return 0
    sleep 0.05
  done
  echo "the call $1 did not end" >&2
  return 1
}

# What the person set on a call (save, name) is the call's: a redial keeps it,
# so a plain purge still leaves a saved call alone.
test_redial_keeps_saved_and_name() {
  "$HOLD_BIN" -d --name keepme -- sh -c 'exit 0' >/dev/null || return 1
  name_ended_soon keepme || return 1
  "$HOLD_BIN" save keepme 2>/dev/null || return 1
  "$HOLD_BIN" -d keepme >/dev/null || return 1
  name_ended_soon keepme || return 1
  out_has '"saved": true' "$HOLD_BIN" inspect keepme || { echo "redial dropped saved" >&2; "$HOLD_BIN" inspect keepme >&2; return 1; }
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  out_has keepme "$HOLD_BIN" list -a || { echo "plain purge removed a saved, redialed call" >&2; return 1; }
}

# A burst of redials of one ended call starts it once: the rest find it
# running (6), and ending it leaves nothing running.
test_concurrent_redials_start_once() {
  local i starts rc launched
  "$HOLD_BIN" -d --name burst -- sh -c 'echo x >> "$0"; sleep 30' "$TEST_ROOT/starts" >/dev/null || return 1
  "$HOLD_BIN" end burst >/dev/null 2>&1 || return 1
  name_ended_soon burst || return 1
  # The root lane runs this body as root and the calls as a test user: the
  # count file must stay writable by the program.
  : > "$TEST_ROOT/starts"; chmod 666 "$TEST_ROOT/starts"
  # A loser that sees the call running before it takes the claim reports it
  # the -d way (the id, "already running", exit 0); one that loses the claim
  # itself exits 6. Either way it says "already running"; one launch does not.
  for i in $(seq 1 16); do
    ( rc=0; "$HOLD_BIN" -d burst >/dev/null 2>"$TEST_ROOT/burst.$i.err" || rc=$?; echo "$rc" > "$TEST_ROOT/burst.$i" ) &
  done
  wait
  sleep 0.5
  starts=$(wc -l < "$TEST_ROOT/starts")
  [ "$starts" -eq 1 ] || { echo "16 redials started $starts programs" >&2; cat "$TEST_ROOT"/burst.? "$TEST_ROOT"/burst.?? >&2; return 1; }
  launched=0
  for i in $(seq 1 16); do
    rc=$(cat "$TEST_ROOT/burst.$i")
    case "$rc" in 0|6) ;; *) echo "redial $i exited $rc" >&2; cat "$TEST_ROOT/burst.$i.err" >&2; return 1 ;; esac
    grep -q 'already running' "$TEST_ROOT/burst.$i.err" && continue
    [ "$rc" -eq 0 ] || { echo "redial $i exited $rc without saying why" >&2; cat "$TEST_ROOT/burst.$i.err" >&2; return 1; }
    launched=$((launched + 1))
  done
  [ "$launched" -eq 1 ] || { echo "want one launching redial, got $launched" >&2; return 1; }
  "$HOLD_BIN" end burst >/dev/null || return 1
  name_ended_soon burst || return 1
  if ls -a "$HOME/.local/state/callbox/" | grep -q '\.reserve$'; then echo "a redial claim was left behind" >&2; return 1; fi
}

# While a redial is launching a call (it holds the call's claim), purge
# leaves the call alone; a dead launcher's unheld claim goes with the record.
test_purge_stands_back_from_a_redial() {
  local id store rc
  command -v flock >/dev/null 2>&1 || skip "no flock(1) to hold a redial's claim"
  store="$HOME/.local/state/callbox"
  id=$("$HOLD_BIN" -d --name inflight -- sh -c 'exit 0') || return 1
  name_ended_soon inflight || return 1
  flock "$store/.$id.reserve" sleep 3 &
  sleep 0.3
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  out_has inflight "$HOLD_BIN" list -a || { echo "a sweep purged a call mid-redial" >&2; return 1; }
  rc=0; "$HOLD_BIN" purge inflight >/dev/null 2>"$TEST_ROOT/inflight.err" || rc=$?
  [ "$rc" -eq 2 ] || { echo "targeted purge mid-redial: rc=$rc (want 2)" >&2; cat "$TEST_ROOT/inflight.err" >&2; return 1; }
  grep -q 'being redialed' "$TEST_ROOT/inflight.err" || { cat "$TEST_ROOT/inflight.err" >&2; return 1; }
  wait
  # the claim is no longer held: litter, removed with the record
  [ -e "$store/.$id.reserve" ] || : > "$store/.$id.reserve"
  "$HOLD_BIN" purge inflight >/dev/null || return 1
  [ ! -e "$store/.$id.reserve" ] || { echo "purge left the dead claim behind" >&2; return 1; }
}

# end/stop/kill on a known call that has already ended: nothing to do, which
# is success (help scripting: "known name with nothing to do"), and it says so.
test_end_on_ended_call_is_success() {
  local rc
  "$HOLD_BIN" -d --name done1 -- true >/dev/null || return 1
  name_ended_soon done1 || return 1
  for verb in end stop kill; do
    rc=0; "$HOLD_BIN" "$verb" done1 >/dev/null 2>"$TEST_ROOT/ended.err" || rc=$?
    [ "$rc" -eq 0 ] || { echo "$verb on an ended call: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/ended.err" >&2; return 1; }
    grep -q 'done1 already exited' "$TEST_ROOT/ended.err" || { cat "$TEST_ROOT/ended.err" >&2; return 1; }
    ! grep -q 'nothing to' "$TEST_ROOT/ended.err" || { cat "$TEST_ROOT/ended.err" >&2; return 1; }
  done
  rc=0; "$HOLD_BIN" end no-such-call-xyz >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 5 ] || { echo "end of an unknown name: rc=$rc (want 5)" >&2; return 1; }
}

# send --wait says the input was sent when the program then ends or never goes
# quiet, so a failed step is not read as undelivered keys.
test_send_wait_says_input_was_sent() {
  local rc
  "$HOLD_BIN" -dt --name sq -- sh -c 'echo sq-ready; read x' >/dev/null || return 1
  "$HOLD_BIN" wait sq --match sq-ready --timeout 5000 >/dev/null || return 1
  rc=0; "$HOLD_BIN" send sq -k Enter --wait 300 2>"$TEST_ROOT/sq.err" || rc=$?
  [ "$rc" -eq 8 ] || { echo "send --wait into an ending program: rc=$rc (want 8)" >&2; cat "$TEST_ROOT/sq.err" >&2; return 1; }
  grep -q 'input sent; sq then ended' "$TEST_ROOT/sq.err" || { cat "$TEST_ROOT/sq.err" >&2; return 1; }
  "$HOLD_BIN" -dt --name busy -- sh -c 'while :; do date +%N; sleep 0.05; done' >/dev/null || return 1
  rc=0; "$HOLD_BIN" send busy x --wait 300 --timeout 1000 2>"$TEST_ROOT/busy.err" || rc=$?
  [ "$rc" -eq 7 ] || { echo "send --wait into a busy program: rc=$rc (want 7)" >&2; cat "$TEST_ROOT/busy.err" >&2; return 1; }
  grep -q 'input sent; busy did not go quiet within 1000 ms' "$TEST_ROOT/busy.err" || { cat "$TEST_ROOT/busy.err" >&2; return 1; }
  "$HOLD_BIN" end busy >/dev/null 2>&1 || true
}

# Names: a verb is refused, a taken name exits 11.
test_call_names_verbs_and_taken() {
  local rc
  rc=0; "$HOLD_BIN" -d --name list true >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 1 ] || { echo "--name list: rc=$rc (want 1)" >&2; return 1; }
  "$HOLD_BIN" -d --name keeper sleep 30 >/dev/null || return 1
  rc=0; "$HOLD_BIN" rename keeper logs >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 1 ] || { echo "rename to logs: rc=$rc (want 1)" >&2; return 1; }
  rc=0; "$HOLD_BIN" -d --name keeper true >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 11 ] || { echo "taken name: rc=$rc (want 11)" >&2; return 1; }
  "$HOLD_BIN" end keeper >/dev/null 2>&1 || true
}

# attach says how it ended: 8 the call ended, 10 no console, 3 refused.
test_attach_exit_codes() {
  local rc
  "$HOLD_BIN" -dt --name brief -- sh -c 'sleep 1' >/dev/null || return 1
  rc=0; "$HOLD_BIN" attach brief </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 8 ] || { echo "ended while attached: rc=$rc (want 8)" >&2; return 1; }
  rc=0; "$HOLD_BIN" attach brief </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 8 ] || { echo "already ended: rc=$rc (want 8)" >&2; return 1; }
  "$HOLD_BIN" -d --name plainjob sleep 30 >/dev/null || return 1
  rc=0; "$HOLD_BIN" attach plainjob </dev/null >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 10 ] || { echo "no console: rc=$rc (want 10)" >&2; return 1; }
  "$HOLD_BIN" end plainjob >/dev/null 2>&1 || true
}

# msg prints the delivered message's id, which msg --received knows.
test_msg_prints_its_id() {
  local id
  "$HOLD_BIN" -dt --name inbox -- cat >/dev/null || return 1
  "$HOLD_BIN" wait inbox --idle 200 --timeout 3000 >/dev/null 2>&1 || true
  id=$("$HOLD_BIN" msg inbox "hello inbox") || return 1
  printf '%s' "$id" | grep -Eq '^[0-9a-f]{6}$' || { echo "msg printed [$id]" >&2; return 1; }
  out_has "id:   $id" "$HOLD_BIN" msg inbox --received "$id" || return 1
  "$HOLD_BIN" end inbox >/dev/null 2>&1 || true
}

# end a b c: the ones that resolve are ended; the worst code is returned.
test_end_acts_on_each_target() {
  local rc
  "$HOLD_BIN" -d --name first sleep 30 >/dev/null || return 1
  "$HOLD_BIN" -d --name third sleep 30 >/dev/null || return 1
  rc=0; "$HOLD_BIN" end first not-a-call third >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 5 ] || { echo "rc=$rc (want 5)" >&2; return 1; }
  record_ended_soon "$("$HOLD_BIN" inspect first | grep -o '"id": "[0-9a-f]*"' | head -1 | cut -d'"' -f4)" || { echo "first was not ended" >&2; return 1; }
  record_ended_soon "$("$HOLD_BIN" inspect third | grep -o '"id": "[0-9a-f]*"' | head -1 | cut -d'"' -f4)" || { echo "third was not ended" >&2; return 1; }
}

# Without --restart-delay, restarts back off (1 s, then 2 s, ...).
test_restart_backs_off_by_default() {
  local n
  "$HOLD_BIN" -d --name spinner --restart always -- sh -c 'echo spin; exit 1' >/dev/null || return 1
  sleep 2.5
  n=$("$HOLD_BIN" logs spinner | grep -c '^spin' || true)
  "$HOLD_BIN" end spinner >/dev/null 2>&1 || true
  # runs at 0, 1, 3 s: two or three by 2.5 s - never the hundreds a tight loop makes
  [ "$n" -ge 2 ] && [ "$n" -le 3 ] || { echo "$n runs in 2.5 s (want 2-3)" >&2; return 1; }
}


test_long_command_list_truncates_instead_of_skips() {
  local out id long_arg list_out
  long_arg=$(printf 'x%.0s' $(seq 1 140))
  out=$("$HOLD_BIN" -d /bin/echo "$long_arg" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]" || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*\.\.\."
}

test_normal_start_writes_user_local_state() {
  local out id mode
  out=$("$HOLD_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  log=$(log_path "$id") || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  mode=$(file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  python3 - "$json" <<'PY' || { cat "$json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
required_top = {'saved', 'version', 'id', 'run_id', 'pid', 'pgid', 'sid', 'start_unix_ns', 'created_unix_ns', 'argv', 'name', 'state', 'started_at', 'created_at', 'log_path', 'boot_id', 'proc_starttime_ticks', 'exe_dev', 'exe_ino'}
missing = required_top - set(obj)
if missing:
    raise SystemExit(f'missing native record fields: {sorted(missing)}')
for legacy in ('Id', 'State', 'Config', 'Path', 'Args', 'LogPath', 'SessionMode', 'CapAdd'):
    if legacy in obj:
        raise SystemExit(f'legacy Docker-shaped key present in native record: {legacy}')
if not obj.get('argv'):
    raise SystemExit('record argv missing/empty')
observed_cwd = (obj.get('observed') or {}).get('cwd')
if observed_cwd and (not observed_cwd.startswith('/') or (observed_cwd != '/' and observed_cwd.endswith('/'))):
    raise SystemExit(f'observed cwd is not a normalized absolute path: {observed_cwd!r}')
PY
}

test_native_record_is_readable_when_hand_authored() {
  # A minimal hand-authored native record (the one schema since 0.7) loads and
  # renders; unknown/legacy keys are ignored, never fatal.
  local id store record logdir logpath idxpath ps_out
  id=fe4a4b8fbc063e25cd4bcb6798791b1dd03caca62f6a0b4c1304f28940992c52
  store="$HOME/.local/state/callbox"
  mkdir -p "$store" || return 1
  logdir="$HOME/.local/state/callbox-logs/$id"
  mkdir -p "$logdir" || return 1
  logpath="$logdir/$id.log"
  idxpath="$logpath.idx"
  : >"$logpath" || return 1
  : >"$idxpath" || return 1
  record="$store/$id.json"
  cat >"$record" <<JSON
{
  "saved": false,
  "version": 1,
  "id": "$id",
  "run_id": "$id",
  "pid": 999999,
  "pgid": 999999,
  "sid": 999999,
  "start_unix_ns": 0,
  "created_unix_ns": 0,
  "argv": ["/usr/bin/python3", "-m", "http.server", "8080"],
  "name": "adjective_noun",
  "state": "exited",
  "exit_code": 7,
  "started_at": "2026-06-28T17:30:47Z",
  "created_at": "2026-06-28T17:30:47Z",
  "ended_at": "2026-06-28T17:30:48Z",
  "log_path": "$logpath",
  "log_idx_path": "$idxpath",
  "LegacyIgnoredKey": {"nested": true},
  "uid": $(id -u),
  "gid": $(id -g),
  "proc_starttime_ticks": 0,
  "exe_dev": 0,
  "exe_ino": 0
}
JSON
  chmod 600 "$record" || return 1
  # As root the suite acts as a test user, who must own what it reads.
  if [ "${USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then chown -R "$TEST_UID:$TEST_GID" "$HOME/.local" || return 1; fi
  ps_out=$("$HOLD_BIN" ps -a 2>"$TEST_ROOT/native-reader.err") || {
    cat "$TEST_ROOT/native-reader.err" >&2
    return 1
  }
  ! grep -q 'skipping corrupt record' "$TEST_ROOT/native-reader.err" || { cat "$TEST_ROOT/native-reader.err" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '^fe4a4b8fbc06' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '"/usr/bin/python3 -m http' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q 'adjective_noun' || { printf '%s\n' "$ps_out" >&2; return 1; }
}

test_root_start_writes_system_store_and_public_unknown() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id mode list_out
  out=$(as_root "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s
' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  public_json=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  ! record_exists "$id" "$ROOT_HOME/.local/state/callbox" || return 1
  mode=$(root_file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(root_file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$public_json") || return 1
  [ "$mode" = 644 ] || return 1
  grep -Eq '"state_hint": "(running|exited)"' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"pid": [1-9]' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"started_at": "' "$public_json" || { cat "$public_json" >&2; return 1; }
  matched=0
  for _ in $(seq 1 30); do
    as_root cat "$json" >"$TEST_ROOT/root-private-state.json" || return 1
    if python3 - "$TEST_ROOT/root-private-state.json" "$public_json" >"$TEST_ROOT/root-public-state-compare.err" 2>&1 <<'PY'
import json, sys
private = json.load(open(sys.argv[1], encoding='utf-8'))
public = json.load(open(sys.argv[2], encoding='utf-8'))
projection_keys = {'id', 'state_hint', 'started_at', 'created_at', 'pid', 'pgid', 'sid', 'running'}
missing_projection = projection_keys - set(public)
if missing_projection:
    raise SystemExit(f'public projection missing keys: {sorted(missing_projection)}')
# The projection must never leak what only root may see.
for forbidden in ('argv', 'env', 'cmdline_display', 'invoked_by_user', 'observed', 'Config'):
    if forbidden in public:
        raise SystemExit(f'public projection leaks private key: {forbidden}')
if public['id'] != private['id']:
    raise SystemExit(f'public/private id drift: {public["id"]!r} != {private["id"]!r}')
for key in ('pid', 'pgid', 'sid'):
    if public[key] != private[key]:
        raise SystemExit(f'public/private drift for {key}: {private[key]!r} != {public[key]!r}')
if public.get('state_hint') != private.get('state'):
    raise SystemExit(f'state drift: public {public.get("state_hint")!r} != private {private.get("state")!r}')
PY
    then
      matched=1
      break
    fi
    sleep 0.1
  done
  [ "$matched" = 1 ] || { cat "$TEST_ROOT/root-public-state-compare.err" "$TEST_ROOT/root-private-state.json" "$public_json" >&2; return 1; }
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s
' "$list_out" | grep -Eq "^$id[[:space:]].*(Up|Exited|running|exited)"
}

test_sudo_start_writes_system_store_with_invoking_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id json
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  ! record_exists "$id" "$ACTOR_HOME/.local/state/callbox" || return 1
  root_grep '"invoked_by_uid": '"$TEST_UID" "$json" || return 1
  root_grep '"invoked_by_gid": '"$TEST_GID" "$json" || return 1
  root_grep '"invoked_by_user": "'"$TEST_USER"'"' "$json" || return 1
  root_grep '"invoked_via_sudo": true' "$json"
}

make_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

make_sleeping_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
sleep 5
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

assert_home_system_start_is_user_local() {
  local out="$1" id json log owner
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$ACTOR_HOME/.local/state/callbox") || return 1
  log=$(root_log_path "$id" "$ACTOR_HOME/.local/state/callbox") || return 1
  root_file_exists "$json" || return 1
  root_file_exists "$log" || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  owner=$(root_file_owner "$json") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
}

test_sudo_system_start_of_home_executable_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out
  app=$(make_home_executable) || return 1
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d "$app" 2>&1) || return 1
  # Files materialize through a sudo child; poll briefly instead of asserting
  # one instant. A real placement bug fails every attempt in the window.
  local sudo_tries=0
  while :; do
    if assert_home_system_start_is_user_local "$out"; then
      return 0
    fi
    sudo_tries=$((sudo_tries + 1))
    [ "$sudo_tries" -lt 20 ] || break
    sleep 0.1
  done
  assert_home_system_start_is_user_local "$out"
}

# A sudo TTY launch from the user's home lands in the user's store, and
# every file root writes there stays the user's: the record after the exit
# stamp, the log, the saved screen (written by a root broker, possibly before
# the launch's own chown runs - the program ends at once), and the record
# after a sudo rename. The user can still peek the ended call.
test_sudo_home_tty_call_files_stay_users() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out id store json log screen owner f tries=0
  app="$ACTOR_HOME/bin/home-tty"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  printf '#!/bin/sh\necho home-tty-ran\n' >"$app" || return 1
  chmod 755 "$app" || return 1
  as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -dwt "$app" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$ACTOR_HOME/.local/state/callbox"
  # The waiter stamps the exit and the broker saves the screen; give both a moment.
  while :; do
    json=$(root_record_path "$id" "$store" 2>/dev/null) || json=""
    screen=$(root_find_prefixed_artifact "$store" "$id" ".screen" 2>/dev/null) || screen=""
    if [ -n "$json" ] && [ -n "$screen" ] && as_root grep -q '"exit_code"' "$json"; then break; fi
    tries=$((tries + 1))
    [ "$tries" -lt 50 ] || { echo "no exit stamp or saved screen for $id" >&2; return 1; }
    sleep 0.1
  done
  log=$(root_log_path "$id" "$store") || return 1
  for f in "$json" "$log" "$screen"; do
    owner=$(root_file_owner "$f") || return 1
    [ "$owner" = "$TEST_UID:$TEST_GID" ] || { echo "$f is owned by $owner, not the user" >&2; return 1; }
  done
  as_user "$HOLD_REAL_BIN" peek "$id" 2>&1 | grep -q 'home-tty-ran' || { echo "the user cannot peek the ended call" >&2; return 1; }
  as_sudo_from_user "$HOLD_REAL_BIN" rename "user:$id" hometty >/dev/null 2>&1 || { echo "sudo rename failed" >&2; return 1; }
  json=$(root_record_path "$id" "$store") || return 1
  owner=$(root_file_owner "$json") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || { echo "sudo rename left the record owned by $owner" >&2; return 1; }
  as_user "$HOLD_REAL_BIN" inspect "$id" >/dev/null 2>&1 || { echo "the user cannot read the renamed record" >&2; return 1; }
}

test_sudo_system_start_of_home_argv_paths_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local cfg out id
  cfg="$ACTOR_HOME/app.toml"
  as_user sh -c 'printf "%s\n" "config" >"$1"' sh "$cfg" || return 1

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config="$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_sudo_context_can_stop_unique_user_local_run() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_docker_shaped_cli_flags_and_rm() {
  local out id id2
  out=$("$HOLD_BIN" -d --name docker-web -e HOLD_DOCKER_ENV=ok -- /bin/sh -c 'echo "$HOLD_DOCKER_ENV"; sleep 0.1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Poll for the captured line: under load the logger can flush later than a
  # fixed 0.2s; a real capture regression still fails the whole window.
  local logs_tries=0
  while :; do
    "$HOLD_BIN" logs -n 1 --plain "$id" >"$TEST_ROOT/docker-logs-tail.out" 2>/dev/null || true
    grep -q '^ok$' "$TEST_ROOT/docker-logs-tail.out" && break
    logs_tries=$((logs_tries + 1))
    [ "$logs_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-logs-tail.out" >&2; return 1; }
    sleep 0.1
  done
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/docker-inspect.json" || return 1
  python3 - "$TEST_ROOT/docker-inspect.json" "$id" <<'PY' || { cat "$TEST_ROOT/docker-inspect.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
if not isinstance(obj, list) or len(obj) != 1:
    raise SystemExit('inspect must return a one-object JSON array')
record = obj[0]
if not str(record.get('id', '')).startswith(sys.argv[2]):
    raise SystemExit('inspect record id mismatch')
if record.get('name') != 'docker-web':
    raise SystemExit('inspect record did not include run name')
if 'ok\n' in json.dumps(record):
    raise SystemExit('inspect returned log text instead of structured record data')
PY
  # The 0.1s child may still be live when the log line lands; poll until
  # ps -a reports it Exited rather than sleeping a fixed beat.
  local ps_tries=0
  while :; do
    "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-ps.out" || return 1
    grep -Eq "^$id[[:space:]]+.*Exited.*docker-web$" "$TEST_ROOT/docker-ps.out" && break
    ps_tries=$((ps_tries + 1))
    [ "$ps_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }
    sleep 0.1
  done
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES" "$TEST_ROOT/docker-ps.out" || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }

  out=$("$HOLD_BIN" -d --name docker-direct /bin/sh -c 'echo direct; sleep 5' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" rm --force docker-direct >/dev/null || return 1
  set +e; "$HOLD_BIN" inspect "$id2" >"$TEST_ROOT/docker-rm-inspect.out" 2>&1; rc=$?; set -e
  [ "$rc" -ne 0 ] || { cat "$TEST_ROOT/docker-rm-inspect.out" >&2; return 1; }
}

# The ps table humanizes CREATED Docker-style, phrases STATUS as Up/Exited, and
# annotates a saved call inline on STATUS.
test_ps_table_humanized_status_and_saved() {
  local out id run_out exit_id ps_out
  out=$("$HOLD_BIN" -d --name ps-humanized -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  run_out=$("$HOLD_BIN" -dw --name ps-exiter -- /bin/sh -c 'exit 5' 2>&1) || return 1
  exit_id=$(printf '%s\n' "$run_out" | extract_id)
  [ -n "$exit_id" ] || return 1
  record_ended_soon "$exit_id" || return 1
  # Age both calls past a second so CREATED reads a whole-unit Docker age
  # ("1 second ago") rather than the sub-second "Less than a second ago".
  sleep 1.2

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  # Header is exact, in the CALL ID..NAMES order with no PROFILE column.
  printf '%s\n' "$ps_out" | grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  # CREATED is humanized: never a raw ISO stamp, always a Docker-style age.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*(About a minute|[0-9]+ (seconds?|minutes?|hours?|days?|weeks?|months?)) ago" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  ! printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*[0-9]{4}-[0-9]{2}-[0-9]{2}T" || return 1
  # STATUS phrasing: Up ... for the live call, Exited (5) ... ago for the ended one.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up [0-9A-Za-z]" || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # save marks the live call; ps annotates protection inline on STATUS.
  "$HOLD_BIN" save ps-humanized >/dev/null 2>&1 || return 1
  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # list carries the same Up/Exited/(saved) STATUS phrasing as ps, but its own
  # header adds the USER column in Docker's IMAGE slot (second, after CALL ID).
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$list_out" >&2; return 1; }

  "$HOLD_BIN" end ps-humanized >/dev/null 2>&1 || true
}

# Content-sized columns must not shear: every data cell begins exactly under its
# header, whether the command is short or long enough to be ellipsized.
test_ps_table_columns_do_not_shear() {
  local short_out long_out ps_out long_arg
  short_out=$("$HOLD_BIN" -d --name ps-short -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  long_arg=$(printf 'y%.0s' $(seq 1 120))
  long_out=$("$HOLD_BIN" -d --name ps-long -- /bin/sh -c "echo $long_arg; sleep 30" 2>&1) || return 1
  [ -n "$(printf '%s\n' "$short_out" | extract_id)" ] || return 1
  [ -n "$(printf '%s\n' "$long_out" | extract_id)" ] || return 1

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | awk '
    NR == 1 {
      for (label = 1; label <= 4; label++) { }
      cols["COMMAND"] = index($0, "COMMAND")
      cols["CREATED"] = index($0, "CREATED")
      cols["STATUS"]  = index($0, "STATUS")
      next
    }
    {
      for (name in cols) {
        pos = cols[name]
        # The gutter before the column is a space and the cell itself is not,
        # so the column starts exactly where its header does.
        if (substr($0, pos - 1, 1) != " " || substr($0, pos, 1) == " ") {
          printf("shear at %s on: %s\n", name, $0) > "/dev/stderr"
          exit 1
        }
      }
    }
  ' || return 1
  # The long command is present and ellipsized rather than dropped.
  printf '%s\n' "$ps_out" | grep -Eq 'ps-long$' || return 1
  printf '%s\n' "$ps_out" | grep -Eq '\.\.\."' || return 1

  "$HOLD_BIN" end ps-short ps-long >/dev/null 2>&1 || true
}

# callbox ports lists the bound listeners of a call's process group, one per line.
test_ports_lists_bound_listener() {
  local out id ports_out tries rc
  [ "$(uname -s)" = Linux ] || skip "process observation (ports) is Linux-only"
  out=$("$HOLD_BIN" -d --name ports-smoke -- python3 -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",0)); s.listen(); print(s.getsockname()[1]); time.sleep(30)' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  ports_out=""
  for tries in $(seq 1 30); do
    ports_out=$("$HOLD_BIN" ports ports-smoke 2>/dev/null || true)
    printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' && break
    sleep 0.1
  done
  printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' || { printf '%s\n' "$ports_out" >&2; return 1; }

  # No target is a usage error.
  set +e; "$HOLD_BIN" ports >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 1 ] || return 1

  "$HOLD_BIN" end ports-smoke >/dev/null 2>&1 || true
}

# callbox stats --no-stream prints one plain frame with the documented columns.
test_stats_single_shot_shape() {
  local out id stats_out
  out=$("$HOLD_BIN" -d --name stats-smoke -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  stats_out=$("$HOLD_BIN" stats --no-stream stats-smoke) || return 1
  printf '%s\n' "$stats_out" | grep -Eq "^CALL ID[[:space:]]+NAME[[:space:]]+CPU %[[:space:]]+MEM \(RSS\)[[:space:]]+PIDS$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }
  printf '%s\n' "$stats_out" | grep -Eq "^$id[[:space:]]+stats-smoke[[:space:]]+[0-9.]+%[[:space:]]+[0-9.]+(B|KiB|MiB|GiB)[[:space:]]+[0-9]+$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }

  "$HOLD_BIN" end stats-smoke >/dev/null 2>&1 || true
}

# inspect reports where a live call's fds actually point. A child that redirects
# its own stdout to a file must show that file under Stdio.Stdout.
test_inspect_reports_stdio_targets() {
  [ "$(uname -s)" = Linux ] || skip "process observation (stdio fd targets) is Linux-only"
  local redir out id
  redir="$(actor_dir)/inspect-stdio.out" || return 1
  out=$("$HOLD_BIN" -d --name stdio-smoke -- /bin/sh -c "exec >\"$redir\"; sleep 30" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  "$HOLD_BIN" inspect stdio-smoke >"$TEST_ROOT/inspect-stdio.json" || return 1
  python3 - "$TEST_ROOT/inspect-stdio.json" "$redir" <<'PY' || { cat "$TEST_ROOT/inspect-stdio.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
record = obj[0]
stdio = record.get('Stdio')
if not isinstance(stdio, dict):
    raise SystemExit('inspect did not report a Stdio object for a live call')
for key in ('Stdin', 'Stdout', 'Stderr'):
    if key not in stdio:
        raise SystemExit('Stdio missing key ' + key)
if stdio['Stdout'] != sys.argv[2]:
    raise SystemExit('Stdout fd target %r did not match the redirect %r' % (stdio['Stdout'], sys.argv[2]))
PY
  "$HOLD_BIN" end stdio-smoke >/dev/null 2>&1 || true
}

test_docker_bare_launch_foreground_follows_output_by_default() {
  local out id
  # Bare foreground streams the process output and prints no id of its own.
  # (on a PTY now - user mode - so lines end in \r\n)
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo bare-foreground-ok' 2>"$TEST_ROOT/docker-bare-foreground.err" | tr -d '\r') || {
    cat "$TEST_ROOT/docker-bare-foreground.err" >&2
    return 1
  }
  grep -q '^bare-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  # Detached prints exactly the bare 64-hex id and hides the process output.
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo bare-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-bare-detached.err") || {
    cat "$TEST_ROOT/docker-bare-detached.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^bare-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}


test_docker_run_foreground_follows_output_by_default() {
  local out id
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo docker-foreground-ok' 2>"$TEST_ROOT/docker-foreground.err" | tr -d '\r') || {
    cat "$TEST_ROOT/docker-foreground.err" >&2
    return 1
  }
  grep -q '^docker-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  # Docker parity: foreground run prints only the process output, no id line.
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo docker-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-detached.err") || {
    cat "$TEST_ROOT/docker-detached.err" >&2
    return 1
  }
  # Docker parity: detach prints the full 64-hex id alone.
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^docker-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_unsupported_options_fail_loudly() {
  local rc
  "$HOLD_BIN" --detach-keys ctrl-p,ctrl-q true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-default.err" || {
    cat "$TEST_ROOT/docker-detach-keys-default.err" >&2
    return 1
  }

  "$HOLD_BIN" --detach-keys ctrl-a true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-custom.err" || {
    cat "$TEST_ROOT/docker-detach-keys-custom.err" >&2
    return 1
  }

  set +e
  "$HOLD_BIN" --detach-keys ctrl-not-a-key true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "invalid --detach-keys: rc=$rc (want 1)" >&2; return 1; }
  grep -q "invalid --detach-keys sequence" "$TEST_ROOT/docker-detach-keys-invalid.err" || { cat "$TEST_ROOT/docker-detach-keys-invalid.err" >&2; return 1; }

  "$HOLD_BIN" prune all >/dev/null 2>&1 || true
  "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-unsupported-ps.out" || return 1
  ! grep -q '/true' "$TEST_ROOT/docker-unsupported-ps.out" || { cat "$TEST_ROOT/docker-unsupported-ps.out" >&2; return 1; }
}


test_docker_restart_policy_restarts_failures() {
  local out id count got
  out=$("$HOLD_BIN" -d --restart on-failure:2 --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/restart-count"; c=$(wc -l < "$HOME/restart-count"); c=$((c)); echo restart-attempt-$c; [ "$c" -lt 3 ] && exit 7 || sleep 1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 30); do
    count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
    [ "$count" -ge 3 ] && break
    sleep 0.1
  done
  count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
  [ "$count" -eq 3 ] || { echo "restart attempts: $count" >&2; "$HOLD_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  got=$("$HOLD_BIN" logs "$id" --plain -n 50) || return 1
  printf '%s\n' "$got" | grep -q 'restart-attempt-1' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-2' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-3' || { printf '%s\n' "$got" >&2; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/restart-run.json" || return 1
  grep -q '"restart": "on-failure:2"' "$TEST_ROOT/restart-run.json" || { cat "$TEST_ROOT/restart-run.json" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}

test_purge_never_follows_stored_paths() {
  local out id store json victim
  store="$HOME/.local/state/callbox"
  victim="$TEST_ROOT/victim.file"
  printf 'victim' >"$victim"
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo x' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  json=$(record_path "$id" "$store") || return 1
  local full log
  full=$(basename "$json" .json)
  log="$store/$full.log"
  [ -f "$log" ] || return 1
  # Point the record's log_path at an innocent file; purge must derive its
  # unlink targets from the layout and warn, never follow the stored string.
  sed -i.bak "s|\"log_path\": \"[^\"]*\"|\"log_path\": \"$victim\"|" "$json" && rm -f "$json.bak"
  "$HOLD_BIN" purge "$id" >/dev/null 2>"$TEST_ROOT/purge-derive.err" || { cat "$TEST_ROOT/purge-derive.err" >&2; return 1; }
  [ -f "$victim" ] || { echo "purge followed a stored path and deleted the victim" >&2; return 1; }
  [ ! -f "$log" ] || { echo "purge left the store-resident log behind" >&2; return 1; }
  grep -q 'outside the store; left untouched' "$TEST_ROOT/purge-derive.err" || { cat "$TEST_ROOT/purge-derive.err" >&2; return 1; }
}

test_usage_errors_go_to_stderr_and_help_names_both_views() {
  local rc out err
  set +e
  out=$("$HOLD_BIN" -d 2>"$TEST_ROOT/flags-only.err")
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "flags-only rc=$rc want 1" >&2; return 1; }
  [ -z "$out" ] || { echo "usage error leaked to stdout" >&2; printf '%s\n' "$out" >&2; return 1; }
  grep -q '^usage: callbox' "$TEST_ROOT/flags-only.err" || { cat "$TEST_ROOT/flags-only.err" >&2; return 1; }
  out=$("$HOLD_BIN" 2>/dev/null)
  printf '%s\n' "$out" | grep -q 'callbox ps ' || { echo "help missing a distinct callbox ps line" >&2; return 1; }
  printf '%s\n' "$out" | grep -q 'ps is an alias' && { echo "help still claims ps is an alias" >&2; return 1; }
  return 0
}

test_crash_leftover_tmp_does_not_block_rewrites() {
  local out id store
  store="$HOME/.local/state/callbox"
  out=$("$HOLD_BIN" -d -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  local json full
  json=$(record_path "$id" "$store") || return 1
  full=$(basename "$json" .json)
  # A crashed writer's legacy fixed-name tomb must not block future rewrites.
  : >"$store/.$full.tmp"
  "$HOLD_BIN" rename "$id" tombproof >/dev/null 2>&1 || { echo "rename blocked by leftover tmp" >&2; return 1; }
  grep -q '"name": "tombproof"' "$json" || { cat "$json" >&2; return 1; }
  "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
}

test_purge_sweep_respects_creation_reserve() {
  local store fid
  store="$HOME/.local/state/callbox"
  ensure_user_fixture_store || return 1
  fid=$(printf '1234567890abcdef%.0s' 1 2 3 4)
  echo orphan >"$store/$fid.log"
  : >"$store/.$fid.reserve"
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  [ -f "$store/$fid.log" ] || { echo "sweep ate a mid-creation log" >&2; return 1; }
  command -v python3 >/dev/null 2>&1 || { rm -f "$store/$fid.log" "$store/.$fid.reserve"; return 0; }
  # A stale reserve is a dead start's litter: the orphan collection proceeds.
  python3 -c 'import os,sys,time; os.utime(sys.argv[1], (time.time()-1200,)*2)' "$store/.$fid.reserve"
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  [ ! -f "$store/$fid.log" ] || { echo "sweep kept an orphan log after the reserve aged out" >&2; return 1; }
  [ ! -f "$store/.$fid.reserve" ] || { echo "stale reserve not collected" >&2; return 1; }
}

test_logs_plain_dumps_whole_log_and_tail_is_last_n() {
  local out id got
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'seq 1 200' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || return 1
  got=$("$HOLD_BIN" logs "$id" --plain | wc -l)
  [ "$got" -eq 200 ] || { echo "plain dump lines=$got want 200" >&2; return 1; }
  got=$("$HOLD_BIN" logs "$id" --plain -n 10)
  [ "$(printf '%s\n' "$got" | wc -l)" -eq 10 ] || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | head -n1 | grep -qx '191' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | tail -n1 | grep -qx '200' || { printf '%s\n' "$got" >&2; return 1; }
  # Docker's --tail composes with --follow: last N, then the live stream
  # (which for an ended call is nothing).
  got=$("$HOLD_BIN" logs "$id" --plain -f -n 5)
  [ "$(printf '%s\n' "$got" | wc -l)" -eq 5 ] || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | head -n1 | grep -qx '196' || { printf '%s\n' "$got" >&2; return 1; }
}

test_docker_foreground_propagates_exit_code() {
  local rc
  set +e
  "$HOLD_BIN" -- /bin/sh -c 'exit 7' >/dev/null 2>"$TEST_ROOT/fg-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "foreground rc=$rc, want 7" >&2; cat "$TEST_ROOT/fg-exit.err" >&2; return 1; }
  set +e
  "$HOLD_BIN" --rm -- /bin/sh -c 'exit 5' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "--rm foreground rc=$rc, want 5" >&2; return 1; }
  "$HOLD_BIN" -- true >/dev/null 2>&1 || { echo "clean foreground exit not 0" >&2; return 1; }
}

test_restart_final_status_recorded() {
  local out id record
  out=$("$HOLD_BIN" -d --restart on-failure:1 --restart-delay 0 -- /bin/sh -c 'exit 3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || { echo "restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"exit_code": 3' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -F "Exited (3)" >/dev/null || { "$HOLD_BIN" list -a >&2; return 1; }
}

test_end_restart_call_records_term_status() {
  local out id record
  out=$("$HOLD_BIN" -d --restart always --restart-delay 0 -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  "$HOLD_BIN" end "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || { echo "ended restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"term_signal": ' "$record" || { cat "$record" >&2; return 1; }
}

test_unwitnessed_death_renders_exited_unknown() {
  local out id record pid ppid tries
  out=$("$HOLD_BIN" -dw -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$record" | head -n1)
  [ -n "$pid" ] || { cat "$record" >&2; return 1; }
  ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
  [ -n "$ppid" ] || return 1
  # Kill the waiter first so no final frame is ever written, then the call.
  kill -9 "$ppid" 2>/dev/null || true
  kill -9 "-$pid" 2>/dev/null || true
  for tries in $(seq 1 40); do
    "$HOLD_BIN" list -a | grep -F "Exited (?)" >/dev/null && return 0
    sleep 0.05
  done
  "$HOLD_BIN" list -a >&2
  cat "$record" >&2
  return 1
}

test_docker_restart_validation_and_tty_gate() {
  local rc
  set +e
  "$HOLD_BIN" --restart nonsense true >/dev/null 2>"$TEST_ROOT/restart-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "invalid restart rc=$rc" >&2; return 1; }
  grep -q 'invalid --restart policy' "$TEST_ROOT/restart-invalid.err" || { cat "$TEST_ROOT/restart-invalid.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" --restart-delay 1 true >/dev/null 2>"$TEST_ROOT/restart-delay-alone.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "restart-delay without restart rc=$rc" >&2; return 1; }
  grep -q -- '--restart-delay requires --restart' "$TEST_ROOT/restart-delay-alone.err" || { cat "$TEST_ROOT/restart-delay-alone.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -t --restart always true >/dev/null 2>"$TEST_ROOT/restart-tty.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "restart tty rc=$rc" >&2; return 1; }
  grep -q -- '--restart with --tty/-t is not supported yet' "$TEST_ROOT/restart-tty.err" || { cat "$TEST_ROOT/restart-tty.err" >&2; return 1; }
}


test_start_existing_run_appends_retained_log() {
  local out id log before_count after_count restart_out
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo restart-append-marker' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  log=$(log_path "$id") || return 1
  for _ in $(seq 1 30); do
    before_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$before_count" -ge 1 ] && break
    sleep 0.1
  done
  [ "${before_count:-0}" -ge 1 ] || { cat "$log" >&2; return 1; }
  restart_out=$("$HOLD_BIN" -d "$id" 2>&1) || {
    printf '%s\n' "$restart_out" >&2
    return 1
  }
  printf '%s\n' "$restart_out" | grep -q "${id:0:12}" || { printf '%s\n' "$restart_out" >&2; return 1; }
  for _ in $(seq 1 30); do
    after_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$after_count" -ge 2 ] && break
    sleep 0.1
  done
  [ "${after_count:-0}" -ge 2 ] || { cat "$log" >&2; return 1; }
}


test_docker_publish_and_volume_rejected() {
  local rc
  set +e
  "$HOLD_BIN" -p 8080:3000 -- true >"$TEST_ROOT/docker-publish.out" 2>"$TEST_ROOT/docker-publish.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { cat "$TEST_ROOT/docker-publish.out" "$TEST_ROOT/docker-publish.err" >&2; return 1; }
  grep -q 'does not publish or forward ports' "$TEST_ROOT/docker-publish.err" || { cat "$TEST_ROOT/docker-publish.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -v "$TEST_ROOT:/work" -- true >"$TEST_ROOT/docker-volume.out" 2>"$TEST_ROOT/docker-volume.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { cat "$TEST_ROOT/docker-volume.out" "$TEST_ROOT/docker-volume.err" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/docker-volume.err" || { cat "$TEST_ROOT/docker-volume.err" >&2; return 1; }
}


test_docker_interactive_stdin_pipes_to_child() {
  local out id
  out=$(printf 'stdin-i-ok\n' | "$HOLD_BIN" -i -f -- /bin/cat 2>"$TEST_ROOT/docker-i.err") || {
    cat "$TEST_ROOT/docker-i.err" >&2
    return 1
  }
  grep -q '^stdin-i-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$(printf 'stdin-no-i\n' | "$HOLD_BIN" -f -- /bin/cat 2>"$TEST_ROOT/docker-no-i.err") || {
    cat "$TEST_ROOT/docker-no-i.err" >&2
    return 1
  }
  if grep -q '^stdin-no-i$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_tty_foreground_attaches_and_detaches() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-child.sh" <<'EOF'
#!/bin/sh
IFS= read line
printf 'tty:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-child.sh"
  set +e
  python3 - <<'PY' | pty_run "$HOLD_BIN -it $TEST_ROOT/docker-tty-child.sh" >"$TEST_ROOT/docker-tty.out" 2>"$TEST_ROOT/docker-tty.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"hello-tty\n")
out.flush()
time.sleep(0.5)
out.write(b"\x10\x11")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it rc=$rc" >&2; cat "$TEST_ROOT/docker-tty.out" "$TEST_ROOT/docker-tty.err" >&2; return 1; }
  grep -q 'tty:hello-tty' "$TEST_ROOT/docker-tty.out" || { cat "$TEST_ROOT/docker-tty.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  record=$(record_path "$id") || { find "$HOME/.local/state/callbox" -maxdepth 1 -type f -print >&2 || true; return 1; }
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-ps.out" || { cat "$TEST_ROOT/docker-tty-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_docker_tty_custom_detach_keys() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-custom-child.sh" <<'EOF'
#!/bin/sh
printf 'custom-ready\n'
IFS= read line
printf 'custom-line:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-custom-child.sh"
  set +e
  python3 - <<'PY' | pty_run "$HOLD_BIN -it --detach-keys ctrl-a $TEST_ROOT/docker-tty-custom-child.sh" >"$TEST_ROOT/docker-tty-custom.out" 2>"$TEST_ROOT/docker-tty-custom.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"\x01")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it custom detach rc=$rc" >&2; cat "$TEST_ROOT/docker-tty-custom.out" "$TEST_ROOT/docker-tty-custom.err" >&2; return 1; }
  grep -q 'custom-ready' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  ! grep -q 'custom-line:' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-custom-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-custom-ps.out" || { cat "$TEST_ROOT/docker-tty-custom-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}


# Built in a copy of the tree: a clean and rebuild in the checkout would
# delete the binary this suite is running and leave a test build where the
# release binary goes.
test_build_artifact_coexistence() {
  local src="$TEST_ROOT/tree" jobs
  mkdir "$src" || return 1
  cp -R Makefile src include third_party "$src/" || return 1
  mkdir -p "$src/.github/scripts" && cp .github/scripts/resolve_version.sh "$src/.github/scripts/" || return 1
  # Two object sets from scratch, so: in parallel, unoptimized, and without
  # the outer build's flags (a sanitizer run passes them down in MAKEFLAGS).
  # What is tested is the artifacts' names, not their code; a sanitizer
  # build on a two-core runner outlasted the per-test timeout.
  jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
  MAKEFLAGS= make -j"$jobs" -C "$src" callbox CFLAGS='-std=c11 -O0' LDFLAGS= STATIC_LDFLAGS= \
    EXTRA_CPPFLAGS=-DHOLD_TESTING VERSION=test >/dev/null || return 1
  [ -x "$src/callbox" ] || return 1
  MAKEFLAGS= make -j"$jobs" -C "$src" callbox-dynamic callbox-test CFLAGS='-std=c11 -O0' LDFLAGS= TEST_LDFLAGS= \
    EXTRA_CPPFLAGS=-DHOLD_TESTING VERSION=test >/dev/null || return 1
  [ -x "$src/callbox" ] && [ -x "$src/callbox-dynamic" ] && [ -x "$src/callbox-test" ]
}

test_concurrent_unique_ids() {
  local i id ids uniq
  ids=""
  for i in $(seq 1 20); do
    "$HOLD_BIN" -d sleep 60 >"$TEST_ROOT/start.$i.out" 2>"$TEST_ROOT/start.$i.err" &
  done
  wait
  for i in $(seq 1 20); do
    id=$(extract_id <"$TEST_ROOT/start.$i.out")
    [ -n "$id" ] || return 1
    ids="$ids\n$id"
  done
  uniq=$(printf '%b\n' "$ids" | sed '/^$/d' | sort -u | wc -l)
  [ "$uniq" -eq 20 ] || return 1
  for id in $(printf '%b\n' "$ids" | sed '/^$/d'); do
    "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
  done
  return 0
}

set -e
# ---------------------------------------------------------------------------
# Security-invariant enforcement tests (audit hardening). Each fails loudly if
# the corresponding privilege/privacy invariant regresses, rather than only
# exercising the happy path.
# ---------------------------------------------------------------------------

# Run the binary as the unprivileged user with spoofed sudo provenance injected
# AFTER any privilege drop, so even the root-runner can test the euid gate.
as_user_spoof_sudo() {
  local args
  args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
    "SUDO_UID=0" "SUDO_GID=0" "SUDO_USER=root"
  )
  [ "${HOLD_BOOT_ID_PATH+x}" = x ] && args+=("HOLD_BOOT_ID_PATH=$HOLD_BOOT_ID_PATH")
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${args[@]}" "$@"
  else
    env "${args[@]}" "$@"
  fi
}

root_safe_hold_copy() {
  local name="$1" dir path
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 1
  ROOT_SAFE_HOLD_DIR="/usr/local/lib/callbox-test-$$"
  dir="$ROOT_SAFE_HOLD_DIR/$(basename "$TEST_ROOT")"
  path="$dir/$name"
  as_root mkdir -p "$dir" || return 1
  as_root chown 0:0 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root chmod 755 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root cp "$HOLD_REAL_BIN" "$path" || return 1
  as_root chown 0:0 "$path" || return 1
  as_root chmod 755 "$path" || return 1
  printf '%s\n' "$path"
}

test_system_store_directory_modes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id d mode
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  for d in runs logs console; do
    mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/$d") || return 1
    [ "$mode" = 700 ] || { echo "$d/ mode=$mode (want 700 -- private state would be world-visible)" >&2; return 1; }
  done
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR") || return 1
  [ "$mode" = 755 ] || { echo "base mode=$mode (want 755)" >&2; return 1; }
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  [ "$mode" = 755 ]
}

test_system_store_tightens_preexisting_loose_dir() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id mode
  as_root mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  as_root chmod 0777 "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  [ "$mode" = 700 ] || { echo "pre-existing loose runs/ not tightened: mode=$mode" >&2; return 1; }
}

test_system_store_refuses_symlinked_critical_dirs() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local old_store name root state target rc mode
  old_store="$HOLD_TEST_SYSTEM_STATE_DIR"
  for name in base runs logs console public; do
    root="$TEST_ROOT/syslink-$name"
    state="$root/state"
    target="$root/target"
    as_root mkdir -p "$root" "$target" || return 1
    as_root chmod 0777 "$target" || return 1
    if [ "$name" = base ]; then
      as_root rm -rf "$state" || return 1
      as_root ln -s "$target" "$state" || return 1
    else
      as_root mkdir -p "$state" || return 1
      case "$name" in
        public) as_root mkdir -p "$state/runs" "$state/logs" "$state/console" || return 1 ;;
        console) as_root mkdir -p "$state/runs" "$state/logs" || return 1 ;;
        logs) as_root mkdir -p "$state/runs" || return 1 ;;
      esac
      as_root ln -s "$target" "$state/$name" || return 1
    fi
    HOLD_TEST_SYSTEM_STATE_DIR="$state"
    export HOLD_TEST_SYSTEM_STATE_DIR
    set +e
    as_root "$HOLD_REAL_BIN" -d true >/dev/null 2>"$TEST_ROOT/syslink-$name.err"
    rc=$?
    set -e
    HOLD_TEST_SYSTEM_STATE_DIR="$old_store"
    export HOLD_TEST_SYSTEM_STATE_DIR
    [ "$rc" -ne 0 ] || { echo "system store accepted symlinked $name directory" >&2; return 1; }
    mode=$(root_file_mode "$target") || return 1
    [ "$mode" = 777 ] || { echo "symlink target for $name was chmod-followed: mode=$mode" >&2; return 1; }
  done
}

test_system_store_artifacts_owned_by_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id owner record log
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  record=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  owner=$(root_file_owner "$record") || return 1
  [ "$owner" = "0:0" ] || { echo "record owner=$owner (want 0:0)" >&2; return 1; }
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "0:0" ] || { echo "log owner=$owner (want 0:0)" >&2; return 1; }
}

test_nonroot_ignores_spoofed_sudo_provenance() {
  local out id json
  out=$(as_user_spoof_sudo "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$ACTOR_HOME/.local/state/callbox") || true
  [ -e "$json" ] || { echo "spoofed-SUDO run did not land in the user store" >&2; return 1; }
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || { echo "spoofed SUDO_* escalated into the system store" >&2; return 1; }
  if grep -q '"invoked_via_sudo": true' "$json" 2>/dev/null; then
    echo "spoofed SUDO_* was trusted by a non-root process" >&2
    return 1
  fi
}


test_signal_refuses_tampered_pgid() {
  local out id rec rc
  out=$("$HOLD_BIN" -d /bin/sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rec=$(record_path "$id") || return 1
  # A record whose pgid is tampered to <=1 must never be signaled -- kill(-pgid)
  # with pgid<=1 would hit pid 1 / the whole session. valid_record rejects it, so
  # stop refuses with exit 5 and the real process is left untouched.
  sed -i.bak 's/"pgid":[[:space:]]*[0-9][0-9]*/"pgid":1/' "$rec" || return 1
  set +e; "$HOLD_BIN" stop "$id" >/dev/null 2>&1; rc=$?; set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$rc" -eq 5 ] || { echo "stop on tampered pgid<=1: rc=$rc (want 5 refused)" >&2; return 1; }
}

test_public_index_write_rollback() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local rc pids leftover
  set +e
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE=1 as_root "$HOLD_REAL_BIN" -d bash -c 'exec -a hold_pubidx_test sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "public-index rollback start: rc=$rc (want 1)" >&2; return 1; }
  # The rollback SIGKILLs the group; poll for its disappearance instead of
  # sampling one instant. A genuine leak survives the whole window.
  local rb_tries=0
  while :; do
    pids=$(ps -eo pid=,args= | awk '/hold_pubidx_test/ && !/awk/ {print $1}')
    [ -z "$pids" ] && break
    rb_tries=$((rb_tries + 1))
    [ "$rb_tries" -lt 20 ] || { echo "leaked process after public-index rollback" >&2; return 1; }
    sleep 0.1
  done
  leftover=$(as_root sh -c 'ls "$1"/runs/*.json "$1"/public/*.json 2>/dev/null' sh "$HOLD_TEST_SYSTEM_STATE_DIR" 2>/dev/null || true)
  [ -z "$leftover" ] || { echo "leftover state after rollback: $leftover" >&2; return 1; }
}

test_quiet_suppresses_banner_keeps_id() {
  local out id
  out=$("$HOLD_BIN" --quiet -d /bin/sleep 300 2>"$TEST_ROOT/q.err") || return 1
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { echo "no id from --quiet start" >&2; return 1; }
  [ "$out" = "$id" ] || { echo "--quiet stdout is not the bare 64-hex id: [$out]" >&2; return 1; }
  [ ! -s "$TEST_ROOT/q.err" ] || { echo "--quiet start wrote to stderr:" >&2; cat "$TEST_ROOT/q.err" >&2; return 1; }
  "$HOLD_BIN" --quiet stop "$id" >/dev/null 2>"$TEST_ROOT/q2.err" || return 1
  [ ! -s "$TEST_ROOT/q2.err" ] || { echo "--quiet stop wrote to stderr" >&2; return 1; }
}

test_run_id_prefix_resolution() {
  local out id pgid pfx got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  pfx=$(printf '%s' "$id" | cut -c1-4)
  got=$("$HOLD_BIN" stop --print "$pfx") || { echo "id-prefix did not resolve" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { echo "prefix resolved to wrong run: [$got]" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_misc_action_guards() {
  local rc
  set +e; "$HOLD_BIN" --console end deadbeef >/dev/null 2>"$TEST_ROOT/c.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "--console end: rc=$rc (want 1)" >&2; return 1; }
  grep -q -- "--console is for placing a call; it does not apply to 'callbox end'" "$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; return 1; }
  set +e; "$HOLD_BIN" help bogustopic >/dev/null 2>"$TEST_ROOT/h.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "help bogustopic: rc=$rc (want 1)" >&2; return 1; }
  grep -q 'unknown help topic' "$TEST_ROOT/h.err" || { cat "$TEST_ROOT/h.err" >&2; return 1; }
  # -l is now the --live filter, not the legacy --iso surface: list renders only
  # the Docker-shaped table, never the old STARTED_AT ISO header.
  "$HOLD_BIN" list -l >"$TEST_ROOT/l.out" 2>&1 || true
  ! grep -q 'STARTED_AT' "$TEST_ROOT/l.out" || { echo "list still emits the legacy ISO header" >&2; cat "$TEST_ROOT/l.out" >&2; return 1; }
}


test_owned_command_exact_arity() {
  local rc
  set +e; "$HOLD_BIN" tail deadbeef extra >/dev/null 2>"$TEST_ROOT/tail-extra.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "tail extra: rc=$rc (want 1)" >&2; return 1; }
  grep -q 'usage: callbox tail <target>' "$TEST_ROOT/tail-extra.err" || { cat "$TEST_ROOT/tail-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" console deadbeef extra >/dev/null 2>"$TEST_ROOT/console-extra.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "console extra: rc=$rc (want 1)" >&2; return 1; }
  grep -q 'usage: callbox console <target>' "$TEST_ROOT/console-extra.err" || { cat "$TEST_ROOT/console-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" purge deadbeef extra >/dev/null 2>"$TEST_ROOT/prune-extra.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "purge extra: rc=$rc (want 1)" >&2; return 1; }
  grep -q 'usage: callbox purge' "$TEST_ROOT/prune-extra.err" || { cat "$TEST_ROOT/prune-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" tail --all deadbeef >/dev/null 2>"$TEST_ROOT/tail-all.err"; rc=$?; set -e
  [ "$rc" -eq 1 ] || { echo "tail --all: rc=$rc (want 1)" >&2; return 1; }
  grep -q 'usage: callbox tail <target>' "$TEST_ROOT/tail-all.err" || { cat "$TEST_ROOT/tail-all.err" >&2; return 1; }

  "$HOLD_BIN" prune >/dev/null || { echo "prune with zero targets should remain valid" >&2; return 1; }
}


test_print_over_all_and_multiple() {
  local id1 id2 pgid1 pgid2 out
  id1=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  id2=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  pgid1=$(record_pgid "$id1"); pgid2=$(record_pgid "$id2")
  [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  out=$("$HOLD_BIN" stop --print "$id1" "$id2") || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" && printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "explicit multi --print missing a pgid" >&2; return 1; }
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# P1 contract-gap pins (reconstruction audit 2026-07-20). Each test pins a
# documented identity/SPEC behavior that previously had zero coverage.
# ---------------------------------------------------------------------------

# G1: --env-file feeds the recipe env (blank/comment lines skipped) without
# touching callbox's own environment.
test_env_file_populates_recipe_env() {
  local out id store fake envfile record log
  store="$HOME/.local/state/callbox"
  fake="$TEST_ROOT/envfile-home"
  mkdir -p "$fake" || return 1
  envfile="$TEST_ROOT/recipe.env"
  cat >"$envfile" <<EOF

# comment lines and blank lines are skipped
HOLD_EF_ONE=alpha
HOME=$fake
HOLD_EF_TWO=beta
EOF
  out=$("$HOLD_BIN" -d --env-file "$envfile" -- /bin/sh -c 'echo "ef:$HOLD_EF_ONE:$HOLD_EF_TWO home:$HOME"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || { echo "--env-file HOME relocated the record store" >&2; return 1; }
  [ ! -d "$fake/.local/state/callbox" ] || { echo "--env-file leaked into callbox's own environment" >&2; return 1; }
  record_ended_soon "$id" || return 1
  log=$(log_path "$id" "$store") || return 1
  grep -q "ef:alpha:beta home:$fake" "$log" || { cat "$log" >&2; return 1; }
  # The file's assignments are recipe data in the record; comments are not.
  grep -q '"HOLD_EF_ONE=alpha"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"HOLD_EF_TWO=beta"' "$record" || { cat "$record" >&2; return 1; }
  ! grep -q 'comment lines' "$record" || { cat "$record" >&2; return 1; }
}


# G10: identity 'end the call politely: TERM, then KILL' — pinned for a plain
# (non-console) call; before this only the console path pinned escalation.
test_end_plain_call_escalates_to_kill() {
  local out id record pgid ok _
  out=$("$HOLD_BIN" -dw -- /bin/sh -c 'trap "" TERM HUP; while :; do sleep 30; done' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  pgid=$(record_pgid "$id") || return 1
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "plain group $pgid survived end escalation" >&2; return 1; }
  record_ended_soon "$id" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 137' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 137:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 9' "$record" || { cat "$record" >&2; return 1; }
}

# G11: SPEC safety invariant — when the leader is gone, signals reach the group
# via the recorded pgid+sid within the same boot, never fail on the dead leader.
test_signal_delivers_to_group_after_leader_exit() {
  local out id pid pgid waiter children p _
  out=$("$HOLD_BIN" -d bash -c 'sleep 300 & sleep 301 & exit 0' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$(record_path "$id")" | head -n1)
  [ -n "$pgid" ] && [ -n "$pid" ] || return 1
  sleep 0.3
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || { echo "no children found in group $pgid" >&2; return 1; }
  # Wait until the leader pid is fully gone (exited and reaped), so end must
  # match the group by its recorded pgid+sid alone. If the waiter still holds
  # the leader as a zombie, remove the waiter so init collects it.
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    waiter=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
    [ -n "$waiter" ] && [ "$waiter" -gt 1 ] && kill -9 "$waiter" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
  fi
  ! kill -0 "$pid" 2>/dev/null || { echo "leader $pid never fully reaped" >&2; return 1; }
  "$HOLD_BIN" end "$id" >/dev/null || { echo "end failed against a leader-gone group" >&2; return 1; }
  for p in $children; do
    pid_dead_enough "$p" || { echo "child $p survived end after leader exit" >&2; return 1; }
  done
}

# G14: identity — `callbox attach <target>` is the primary spelling (console the
# alias); it round-trips bytes and tees to the log exactly like console.
test_attach_verb_matches_console() {
  local out id store logf out2 id2 rc
  "$HOLD_BIN" help attach >"$TEST_ROOT/attach-help.out" || return 1
  grep -q 'usage: callbox attach <target>' "$TEST_ROOT/attach-help.out" || { cat "$TEST_ROOT/attach-help.out" >&2; return 1; }
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "attach:$line"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/callbox"
  # the program ends after the line: attach exits 8 ("the call ended")
  { printf 'ping\n' | "$HOLD_BIN" attach "$id" >"$TEST_ROOT/attach.out" 2>"$TEST_ROOT/attach.err" || [ $? -eq 8 ]; } || {
    cat "$TEST_ROOT/attach.out" "$TEST_ROOT/attach.err" >&2
    return 1
  }
  grep -q 'attach:ping' "$TEST_ROOT/attach.out" || { cat "$TEST_ROOT/attach.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'attach:ping' "$logf" || { cat "$logf" >&2; return 1; }
  # attach on a plain call reports 'has no console', like console.
  out2=$("$HOLD_BIN" -d sleep 30 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id2" ] || return 1
  # (exit 10: the call exists but has no console - the wrong kind of call)
  rc=0; "$HOLD_BIN" attach "$id2" >/dev/null 2>"$TEST_ROOT/attach-none.err" || rc=$?
  [ "$rc" -eq 10 ] || { echo "attach without a console exited $rc (want 10)" >&2; return 1; }
  grep -q 'has no console' "$TEST_ROOT/attach-none.err" || { cat "$TEST_ROOT/attach-none.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id2" >/dev/null || return 1
}

# G19: identity — 'plain output automatically when not a TTY': bare callbox logs
# piped away dumps the raw log bytes, no viewer chrome, rc 0.
test_logs_non_tty_auto_plain() {
  local out id
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'printf "auto-plain-1\nauto-plain-2\n"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" logs "$id" </dev/null >"$TEST_ROOT/auto-plain.out" 2>"$TEST_ROOT/auto-plain.err" || {
    cat "$TEST_ROOT/auto-plain.err" >&2
    return 1
  }
  printf 'auto-plain-1\nauto-plain-2\n' >"$TEST_ROOT/auto-plain.want"
  diff -u "$TEST_ROOT/auto-plain.want" "$TEST_ROOT/auto-plain.out" || { echo "non-TTY logs output is not the raw log bytes" >&2; return 1; }
}

# G24: identity — `callbox list -u/--user`: your personal calls only, even under
# sudo (the invoking user's store, never root's), no global/redacted rows.
test_list_user_scope_only_personal() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local uout uid_call gout gid_call
  uout=$("$HOLD_BIN" -d --name personal_scope sleep 300 2>&1) || return 1
  uid_call=$(printf '%s\n' "$uout" | extract_id)
  [ -n "$uid_call" ] || { printf '%s\n' "$uout" >&2; return 1; }
  gout=$(as_root "$HOLD_REAL_BIN" -d --name global_scope sleep 300 2>&1) || return 1
  gid_call=$(printf '%s\n' "$gout" | extract_id)
  [ -n "$gid_call" ] || { printf '%s\n' "$gout" >&2; return 1; }
  # Under sudo, -u lists the invoking user's ledger only.
  as_sudo_from_user "$HOLD_REAL_BIN" list -u >"$TEST_ROOT/sudo-list-u.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]+$TEST_USER[[:space:]].*personal_scope$" "$TEST_ROOT/sudo-list-u.out" || { cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  ! grep -q "$gid_call" "$TEST_ROOT/sudo-list-u.out" || { echo "sudo list -u leaked a global call" >&2; cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  ! grep -q 'hidden' "$TEST_ROOT/sudo-list-u.out" || { echo "sudo list -u carried redacted rows" >&2; cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  # Long spelling matches.
  as_sudo_from_user "$HOLD_REAL_BIN" list --user >"$TEST_ROOT/sudo-list-user.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]+$TEST_USER[[:space:]].*personal_scope$" "$TEST_ROOT/sudo-list-user.out" || { cat "$TEST_ROOT/sudo-list-user.out" >&2; return 1; }
  # As a plain user, -u excludes the global projection that -a includes.
  "$HOLD_BIN" list -a >"$TEST_ROOT/user-list-a.out" 2>&1 || return 1
  grep -q "$gid_call" "$TEST_ROOT/user-list-a.out" || { echo "list -a fixture missing the global call" >&2; cat "$TEST_ROOT/user-list-a.out" >&2; return 1; }
  "$HOLD_BIN" list -u >"$TEST_ROOT/user-list-u.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]" "$TEST_ROOT/user-list-u.out" || { cat "$TEST_ROOT/user-list-u.out" >&2; return 1; }
  ! grep -q "$gid_call" "$TEST_ROOT/user-list-u.out" || { echo "user list -u leaked a global call" >&2; cat "$TEST_ROOT/user-list-u.out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" stop "system:$gid_call" >/dev/null 2>&1 || true
}

# G25: identity — rm, prune, and drop are accepted purge aliases; drop removes
# record + log exactly like prune, targeted and sweeping.
test_drop_alias_matches_prune() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d true 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/callbox"
  record_ended_soon "$id1" || return 1
  record_ended_soon "$id2" || return 1
  "$HOLD_BIN" help drop >"$TEST_ROOT/drop-help.out" || return 1
  grep -q 'usage: callbox purge' "$TEST_ROOT/drop-help.out" || { cat "$TEST_ROOT/drop-help.out" >&2; return 1; }
  "$HOLD_BIN" drop "$id1" >/dev/null || return 1
  ! record_exists "$id1" "$store" && ! log_exists "$id1" "$store" || { echo "drop left artifacts of $id1" >&2; return 1; }
  record_exists "$id2" "$store" && log_exists "$id2" "$store" || { echo "targeted drop touched another call" >&2; return 1; }
  # A bare drop sweeps ended calls, exactly like a bare purge/prune.
  "$HOLD_BIN" drop >/dev/null 2>&1 || return 1
  ! record_exists "$id2" "$store" || { echo "sweeping drop kept an ended call" >&2; return 1; }
}

# G35: SPEC — a public entry whose private record is gone is an orphan;
# a root purge of the global store sweeps it so no view can resurrect it.
# Root's plain -d leaves nothing behind either: no waiter rewrites the public
# entry at exit, so other users' list looks at the call's process group
# (anyone can) instead of trusting the entry's "running" hint.
test_global_plain_d_seen_live_by_users() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local id public pid parent
  id=$(as_root "$HOLD_REAL_BIN" -d --name globald -- sleep 300 2>/dev/null | extract_id) || return 1
  [ -n "$id" ] || return 1
  public=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$public" | head -n1)
  [ -n "$pid" ] || { cat "$public" >&2; return 1; }
  parent=$(ps -o ppid= -p "$pid" | tr -d ' ')
  ! ps -o args= -p "$parent" | grep -q callbox || { echo "root -d left a callbox process" >&2; return 1; }
  "$HOLD_BIN" list -s | grep -Eq "Up .* globald$" || { "$HOLD_BIN" list -s >&2; return 1; }
  as_root kill -KILL "$pid" || return 1
  sleep 0.3
  grep -q '"state_hint": "running"' "$public" || { echo "something rewrote the public entry" >&2; return 1; }
  ! "$HOLD_BIN" list -s | grep -Eq "Up .* globald$" || { echo "a dead global call still listed as running" >&2; return 1; }
  "$HOLD_BIN" list -s | grep -Eq "Exited .* globald$" || { "$HOLD_BIN" list -s >&2; return 1; }
  ! "$HOLD_BIN" list -s -l | grep -q " globald$" || { echo "list --live showed a dead global call" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" purge -s globald >/dev/null 2>&1 || true
}

test_orphan_public_entry_swept() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local out id json public_json ok _
  out=$(as_root "$HOLD_REAL_BIN" -dw true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  public_json=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  ok=0
  for _ in $(seq 1 50); do
    as_root grep -q '"state": "exited"' "$json" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "global call never marked exited" >&2; return 1; }
  # Simulate the orphan: the private record vanishes, the projection stays.
  as_root rm -f "$json" || return 1
  [ -f "$public_json" ] || { echo "public entry disappeared before the sweep" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" purge -s >/dev/null 2>&1 || return 1
  [ ! -e "$public_json" ] || { echo "orphan public entry survived sudo callbox purge" >&2; cat "$public_json" >&2; return 1; }
  # No view resurrects it.
  "$HOLD_BIN" list -a >"$TEST_ROOT/orphan-list.out" 2>&1 || return 1
  ! grep -q "^$id" "$TEST_ROOT/orphan-list.out" || { echo "swept orphan still listed" >&2; cat "$TEST_ROOT/orphan-list.out" >&2; return 1; }
}

# G37: parity — `--restart unless-stopped` restarts crashes, but `callbox end`
# stops the loop for good and records the term status.
test_restart_unless_stopped() {
  local out id count record _
  out=$("$HOLD_BIN" -d --restart unless-stopped --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/us-count"; c=$(wc -l < "$HOME/us-count"); c=$((c)); [ "$c" -lt 3 ] && exit 7; sleep 60' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 50); do
    count=$(wc -l <"$HOME/us-count" 2>/dev/null || echo 0)
    count=$((count))
    [ "$count" -ge 3 ] && break
    sleep 0.1
  done
  [ "$count" -eq 3 ] || { echo "unless-stopped attempts: $count (want 3)" >&2; "$HOLD_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/unless-stopped.json" || return 1
  grep -q '"restart": "unless-stopped"' "$TEST_ROOT/unless-stopped.json" || { cat "$TEST_ROOT/unless-stopped.json" >&2; return 1; }
  # The third attempt is now sleeping; end must bring it down and stay down.
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  record_ended_soon "$id" || { echo "ended unless-stopped call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"term_signal": ' "$record" || { cat "$record" >&2; return 1; }
  sleep 1
  count=$(wc -l <"$HOME/us-count" 2>/dev/null || echo 0)
  count=$((count))
  [ "$count" -eq 3 ] || { echo "unless-stopped restarted after callbox end: attempts=$count" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -Eq "^$id[[:space:]].*Exited" || { "$HOLD_BIN" list -a >&2; return 1; }
}

# callbox msg: a call messages another; the header names the sender's call and
# how to reply, a sender outside any call is said to be, and newlines are
# flattened when there is no bracketed paste.
test_msg_between_calls() {
  "$HOLD_BIN" -dt --name lead -- /bin/sh -c 'stty -echo; echo lead-ready; while IFS= read -r l; do printf "LEAD GOT: %s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait lead --match lead-ready --timeout 5000 || return 1
  "$HOLD_BIN" resize lead 200 24 || return 1 # headers stay on one line
  # The sender runs in a new session, as agent harnesses run tool commands:
  # it is still named by its call.
  if command -v setsid >/dev/null 2>&1; then
    "$HOLD_BIN" -dt --name worker -- /bin/sh -c "setsid \"$HOLD_REAL_BIN\" msg lead 'task done'; sleep 30" >/dev/null || return 1
  else
    "$HOLD_BIN" -dt --name worker -- /bin/sh -c "\"$HOLD_REAL_BIN\" msg lead 'task done'; sleep 30" >/dev/null || return 1
  fi
  "$HOLD_BIN" wait lead --match 'LEAD GOT' --timeout 10000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" peek lead | grep -Eq '^LEAD GOT: \[msg from worker; reply: callbox msg worker "\.\.\."\] task done$' || { "$HOLD_BIN" peek lead >&2; return 1; }
  # --received is the call's history: by id, the sender and the full text.
  local mid
  mid=$("$HOLD_BIN" msg lead --received | awk '/task done$/ {print $1; exit}')
  [ -n "$mid" ] || { "$HOLD_BIN" msg lead --received >&2; return 1; }
  "$HOLD_BIN" msg lead --received "$mid" >"$TEST_ROOT/recv.out" || { echo "no record of delivered message $mid" >&2; return 1; }
  grep -qx 'from: worker' "$TEST_ROOT/recv.out" && grep -qx 'task done' "$TEST_ROOT/recv.out" || { cat "$TEST_ROOT/recv.out" >&2; return 1; }
  if "$HOLD_BIN" msg lead --received 000000 >/dev/null 2>&1; then echo "--received found an id never delivered" >&2; return 1; fi
  CALLBOX_ID="$("$HOLD_BIN" inspect lead | sed -n 's/^  "id": "\(.*\)",$/\1/p')" "$HOLD_BIN" msg --received "$mid" | grep -qx 'task done' || { echo "--received without a target did not read the current call" >&2; return 1; }
  # A sender named by its call's name is that call: the header is addressing.
  CALLBOX_ID= CALLBOX_NAME=worker "$HOLD_BIN" msg lead 'by name' || return 1
  "$HOLD_BIN" wait lead --match 'by name' --timeout 5000 || return 1
  "$HOLD_BIN" peek lead | grep -Eq '^LEAD GOT: \[msg from worker; reply: callbox msg worker "\.\.\."\] by name$' || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" msg lead --received | grep -Eq '^[0-9a-f]{6} .* worker +by name$' || { "$HOLD_BIN" msg lead --received >&2; return 1; }
  # msg prints the id it was delivered under
  mid=$(env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg lead "$(printf 'two\nlines')") || return 1
  "$HOLD_BIN" wait lead --match 'two lines' --timeout 5000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" peek lead | grep -Eq '^LEAD GOT: \[msg from outside callbox\] two lines$' || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" msg lead --received "$mid" | grep -qx 'two' || { echo "msg printed no id --received knows" >&2; return 1; }
  "$HOLD_BIN" end lead worker >/dev/null 2>&1 || true
}

# A sender's claim is only ever an id or a call name: one carrying a newline
# cannot write a line of its own into the receiver's .msgs log, and a corrupt
# record in the store does not crash the receiver's broker while it looks a
# claimed name up. And --received still reads a log past 1 MiB.
test_msg_claim_cannot_forge_or_crash() {
  "$HOLD_BIN" -dt --name lead -- /bin/sh -c 'stty -echo; echo lead-ready; while IFS= read -r l; do printf "LEAD GOT: %s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait lead --match lead-ready --timeout 5000 || return 1
  "$HOLD_BIN" resize lead 200 24 || return 1
  local store="$HOME/.local/state/callbox" lid lpath
  printf 'not json' >"$store/$(printf 'f%.0s' $(seq 64)).json"
  CALLBOX_ID= CALLBOX_NAME=nosuchcall "$HOLD_BIN" msg lead 'past a corrupt record' || return 1
  "$HOLD_BIN" wait lead --match 'past a corrupt record' --timeout 5000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  CALLBOX_ID=$'x\nabcdef 1795129200 1 lead_agent' "$HOLD_BIN" msg lead 'forged' || return 1
  "$HOLD_BIN" wait lead --match 'forged' --timeout 5000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" peek lead | grep -Eq '^LEAD GOT: \[msg from outside callbox\] forged$' || { "$HOLD_BIN" peek lead >&2; return 1; }
  if "$HOLD_BIN" msg lead --received abcdef >/dev/null 2>&1; then echo "a forged claim wrote a line of its own" >&2; return 1; fi
  lid=$("$HOLD_BIN" inspect lead | sed -n 's/^  "id": "\(.*\)",$/\1/p')
  lpath="$store/$lid.msgs"
  # 1.2 MiB of earlier messages, then one more: --received still finds it.
  python3 -c 'import sys; open(sys.argv[1], "a").write(("000001 1795129200 0 - " + "61" * 600000 + "\n") * 1)' "$lpath" || return 1
  local mid
  mid=$(CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg lead 'after a big log') || return 1
  "$HOLD_BIN" wait lead --match 'after a big log' --timeout 5000 || return 1
  [ -n "$mid" ] || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" msg lead --received "$mid" | grep -qx 'after a big log' || { echo "--received lost a message past 1 MiB" >&2; return 1; }
  "$HOLD_BIN" end lead >/dev/null 2>&1 || true
}

# A PNG too large to render safely is refused, not allocated (or wrapped on
# 32-bit targets).
test_peek_image_refuses_oversize() {
  "$HOLD_BIN" -dt --size 1000 1000 --name big -- /bin/sh -c 'echo big-ready; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait big --match big-ready --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  local rc=0
  "$HOLD_BIN" peek big --image "$W/huge.png" --scale 4 2>"$W/err" || rc=$?
  [ "$rc" -eq 3 ] || { echo "expected exit 3, got $rc" >&2; cat "$W/err" >&2; return 1; }
  grep -q 'failed to write image' "$W/err" || { cat "$W/err" >&2; return 1; }
  "$HOLD_BIN" end big >/dev/null 2>&1 || true
}

# A message cannot carry terminal controls: no Escape, carriage return,
# bracketed-paste end, or title sequence reaches the receiver - newlines and
# tabs still do.
test_msg_strips_control_characters() {
  "$HOLD_BIN" -dt --name ctl -- /bin/sh -c 'stty -echo; echo ctl-ready; while IFS= read -r l; do printf "GOT<%s>\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait ctl --match ctl-ready --timeout 5000 || return 1
  "$HOLD_BIN" resize ctl 200 24 || return 1
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg ctl -- "$(printf 'one\033]0;HIJACK\007two\033[2Jthree\r')" || return 1
  "$HOLD_BIN" wait ctl --match 'three' --timeout 5000 || { "$HOLD_BIN" peek ctl >&2; return 1; }
  ! "$HOLD_BIN" peek ctl --json | grep -q '"title": "HIJACK"' || { echo "a message set the terminal title" >&2; return 1; }
  # whole sequences go, not just their ESC (no "]0;HIJACK" or "[2J" residue)
  "$HOLD_BIN" peek ctl | grep -q 'onetwothree>' || { "$HOLD_BIN" peek ctl >&2; return 1; }
  local mid
  mid=$("$HOLD_BIN" msg ctl --received | awk '{print $1; exit}')
  ! "$HOLD_BIN" msg ctl --received "$mid" | LC_ALL=C grep -q "$(printf '\033')" || { echo "--received printed an escape" >&2; return 1; }
  "$HOLD_BIN" end ctl >/dev/null 2>&1 || true
}

# Only a verb's own value flags are split at '=': text after --, a flag's
# value, and msg text after -- reach the program as typed.
test_screen_verbs_keep_literal_eq_text() {
  "$HOLD_BIN" -dt --name eq -- /bin/sh -c 'stty -echo; echo eq-ready; while IFS= read -r l; do printf "GOT:%s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait eq --match eq-ready --timeout 5000 || return 1
  "$HOLD_BIN" resize eq 200 24 || return 1
  "$HOLD_BIN" send eq -- --foo=bar || return 1
  "$HOLD_BIN" send eq --enter || return 1
  "$HOLD_BIN" wait eq --match 'GOT:--foo=bar' --timeout=5000 || { "$HOLD_BIN" peek eq >&2; return 1; }
  "$HOLD_BIN" send eq --timeout=5000 --text --a=b --enter || return 1
  "$HOLD_BIN" wait eq --match 'GOT:--a=b' --timeout 5000 || { "$HOLD_BIN" peek eq >&2; return 1; }
  "$HOLD_BIN" send eq --paste '--p=q' --enter || return 1
  "$HOLD_BIN" wait eq --match 'GOT:--p=q' --timeout 5000 || { "$HOLD_BIN" peek eq >&2; return 1; }
  # A short value flag's value is taken as typed too.
  W=$(actor_dir) || return 1
  (cd "$W" && "$HOLD_BIN" peek eq -o "--odd=name.png") || { echo "peek -o did not take its value as typed" >&2; return 1; }
  [ -s "$W/--odd=name.png" ] || { echo "peek -o wrote no image" >&2; return 1; }
  "$HOLD_BIN" send eq -k Enter || return 1
  CALLBOX_ID= CALLBOX_NAME= "$HOLD_BIN" msg eq -- check --flag=1 || return 1
  "$HOLD_BIN" wait eq --match 'check --flag=1' --timeout 5000 || { "$HOLD_BIN" peek eq >&2; return 1; }
  "$HOLD_BIN" end eq >/dev/null 2>&1 || true
}

# An argument or -e value over 4 KiB survives list, the exit stamp, and redial.
test_long_argv_and_env_survive() {
  local big
  big=$(head -c 5000 /dev/zero | tr '\0' a)
  # -dw: the waiter stamps the exit, rewriting the record from what it read.
  "$HOLD_BIN" -dw --name longarg -e "BIG=$big" -- /bin/sh -c 'echo "len=${#BIG} arg=${#1}"' sh "$big" >/dev/null || return 1
  "$HOLD_BIN" wait longarg --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs longarg | grep -q '^len=5000 arg=5000' || { echo "the call did not see its long values" >&2; return 1; }
  out_has '"exit_code": 0' "$HOLD_BIN" inspect longarg || { echo "no exit stamp to test" >&2; return 1; }
  "$HOLD_BIN" inspect longarg | grep -q "BIG=$big" || { echo "the exit stamp dropped the long env" >&2; return 1; }
  "$HOLD_BIN" longarg >/dev/null || return 1
  "$HOLD_BIN" wait longarg --timeout 5000 >/dev/null || return 1
  [ "$("$HOLD_BIN" logs longarg | grep -c '^len=5000 arg=5000')" -ge 2 ] || { echo "redial lost the long values" >&2; return 1; }
}

# Redialing an ended -dt call starts it detached again, as it first ran.
test_dt_redial_stays_detached() {
  "$HOLD_BIN" -dt --name dtr -- /bin/sh -c 'echo dtr-ran' >/dev/null || return 1
  "$HOLD_BIN" wait dtr --exit --timeout 5000 >/dev/null || return 1
  local out
  out=$("$HOLD_BIN" dtr </dev/null 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "-dt redial did not detach: [$out]" >&2; return 1; }
}

# --tail CALL follows that call's log - no PTY and no redial, so a running
# call is followed (not refused) and its recipe keeps its mode. A word that
# names no call is still a command to launch.
test_tail_flag_follows_a_calls_log() {
  "$HOLD_BIN" -dt --name ticker -- /bin/sh -c 'i=0; while [ $i -lt 6 ]; do echo "tick $i"; i=$((i+1)); sleep 0.3; done' >/dev/null || return 1
  "$HOLD_BIN" wait ticker --match 'tick 1' --timeout 5000 >/dev/null || return 1
  local rc=0
  "$HOLD_BIN" --tail ticker </dev/null >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" || rc=$?
  [ "$rc" -eq 0 ] || { echo "--tail on a running call: rc=$rc" >&2; cat "$TEST_ROOT/tail.err" >&2; return 1; }
  grep -q 'tick 5' "$TEST_ROOT/tail.out" || { cat "$TEST_ROOT/tail.out" >&2; return 1; }
  out_has '"tty": true' "$HOLD_BIN" inspect ticker || { echo "--tail rewrote the recipe" >&2; return 1; }
  "$HOLD_BIN" wait ticker --exit --timeout 5000 >/dev/null || return 1
  local runs_before
  runs_before=$("$HOLD_BIN" logs ticker | grep -c 'tick 0')
  "$HOLD_BIN" -f ticker </dev/null >"$TEST_ROOT/tail-ended.out" 2>&1 || true
  grep -q 'tick 5' "$TEST_ROOT/tail-ended.out" || { echo "-f on an ended call did not print its log" >&2; cat "$TEST_ROOT/tail-ended.out" >&2; return 1; }
  [ "$("$HOLD_BIN" logs ticker | grep -c 'tick 0')" -eq "$runs_before" ] || { echo "--tail of an ended call ran it again" >&2; return 1; }
  "$HOLD_BIN" tail ticker </dev/null | grep -q 'tick 5' || { echo "tail of an ended call (by name) did not print its log" >&2; return 1; }
  [ "$("$HOLD_BIN" --tail echo tail-launch </dev/null)" = "tail-launch" ] || { echo "--tail with a command did not launch it" >&2; return 1; }
}

# A TTY call's environment describes callbox's terminal, not the launcher's: TERM
# is xterm-256color whatever the launcher had (none under cron/CI, or its own
# terminal's), and tmux/screen variables are dropped - a program seeing TMUX
# acts on the caller's tmux. An explicit -e TERM=... wins; plain calls are
# left alone.
test_tty_call_defaults_term() {
  # /usr/bin/env directly: some /bin/sh (macOS) set TERM=dumb themselves.
  env -u TERM "$HOLD_BIN" -dt --name t1 -- /usr/bin/env >/dev/null || return 1
  TERM=dumb "$HOLD_BIN" -dt --name t2 -- /usr/bin/env >/dev/null || return 1
  TERM=unknown "$HOLD_BIN" -dt --name t5 -- /usr/bin/env >/dev/null || return 1
  env -u TERM "$HOLD_BIN" -dt -e TERM=vt100 --name t3 -- /usr/bin/env >/dev/null || return 1
  env -u TERM "$HOLD_BIN" -d --name t4 -- /usr/bin/env >/dev/null || return 1
  TERM=screen-256color TMUX=/tmp/tmux-x/default,1,0 TMUX_PANE=%1 STY=1.pts TERM_PROGRAM=tmux \
    "$HOLD_BIN" -dt --name t6 -- /usr/bin/env >/dev/null || return 1
  TMUX=/tmp/tmux-x/default,1,0 "$HOLD_BIN" -d --name t7 -- /usr/bin/env >/dev/null || return 1
  for c in t1 t2 t3 t4 t5 t6 t7; do "$HOLD_BIN" wait "$c" --timeout 5000 >/dev/null || { echo "wait $c failed" >&2; return 1; }; done
  # logs, not peek: the environment can scroll past the 24-row screen. Each
  # call's environment goes to a file first: grep -q on a pipe hangs up at its
  # first match, which fails the pipeline (pipefail) when the environment is
  # large. And only the variables in question are shown on failure - the
  # environment holds whatever secrets the person running the suite has.
  local c
  for c in t1 t2 t3 t4 t5 t6 t7; do
    "$HOLD_BIN" logs "$c" | tr -d '\r' >"$TEST_ROOT/env.$c" || return 1
  done
  shown() { echo "$1:" >&2; grep -E '^(TERM|TMUX|TMUX_PANE|STY|TERM_PROGRAM)=' "$TEST_ROOT/env.$1" >&2 || true; }
  for c in t5 t1 t2 t6; do
    grep -qx 'TERM=xterm-256color' "$TEST_ROOT/env.$c" || { shown "$c"; return 1; }
  done
  grep -qx 'TERM=vt100' "$TEST_ROOT/env.t3" || { shown t3; return 1; }
  # A plain (non -t) call is left alone: callbox injects nothing.
  ! grep -qx 'TERM=xterm-256color' "$TEST_ROOT/env.t4" || { shown t4; return 1; }
  ! grep -Eq '^(TMUX|TMUX_PANE|STY|TERM_PROGRAM)=' "$TEST_ROOT/env.t6" || { shown t6; return 1; }
  grep -q '^TMUX=/tmp/tmux-x/default,1,0$' "$TEST_ROOT/env.t7" || { shown t7; return 1; }
}

# Arrow keys and mouse events move a real program's cursor (vim, mouse=a).
test_cursor_control_keys_and_mouse() {
  command -v vim >/dev/null 2>&1 || skip_tool "vim not installed"
  W=$(actor_dir) || return 1
  seq -f 'line %g of a file long enough to click anywhere on it' 1 60 >"$W/f.txt"
  "$HOLD_BIN" -dt --name v -- vim -u NONE -N -i NONE -c 'set mouse=a' -c 'set ttymouse=sgr' "$W/f.txt" >/dev/null || return 1
  "$HOLD_BIN" wait v --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"mouse": "drag"' || { "$HOLD_BIN" peek v --json >&2; return 1; }
  "$HOLD_BIN" send v -k Down -k Down -k Right -k Right -k Right --wait 200 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"cursor": {"row": 2, "col": 3,' || { "$HOLD_BIN" peek v --json | grep cursor >&2; return 1; }
  "$HOLD_BIN" send v --mouse click,20,10 --wait 200 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"cursor": {"row": 10, "col": 20,' || return 1
  "$HOLD_BIN" send v --mouse press,5,3 --mouse drag,30,5 --mouse release,30,5 --wait 300 || return 1
  "$HOLD_BIN" peek v | tail -1 | grep -q -- '-- VISUAL --' || { "$HOLD_BIN" peek v | tail -2 >&2; return 1; }
  "$HOLD_BIN" send v -k Escape --mouse scroll-down,10,10 --wait 300 || return 1
  "$HOLD_BIN" peek v | head -1 | grep -qv '^line 1 ' || return 1
  "$HOLD_BIN" send v ':q!' --enter || return 1
  "$HOLD_BIN" wait v --timeout 5000 >/dev/null || return 1
}

# Keys arrive as exact xterm bytes, one write per key, in both cursor modes.
test_keys_exact_bytes_one_write_each() {
  cat >"$TEST_ROOT/keybytes.py" <<'PY'
import os, sys, tty, termios
fd = sys.stdin.fileno()
if sys.argv[1] == "app":
    sys.stdout.write("\033[?1h\033=")
sys.stdout.write("keys ready\r\n"); sys.stdout.flush()
tty.setraw(fd)
while True:
    chunk = os.read(fd, 64)
    sys.stdout.write("%s\r\n" % chunk.hex()); sys.stdout.flush()
    if chunk == b"Q": break
PY
  keys="Enter Backspace C-h Tab S-Tab Escape C-a C-c C-z M-x M-Enter Up Home C-Left F1 F12 C-Space"
  want_normal="0d 7f 08 09 1b5b5a 1b 01 03 1a 1b78 1b0d 1b5b41 1b5b48 1b5b313b3544 1b4f50 1b5b32347e 00"
  want_app="0d 7f 08 09 1b5b5a 1b 01 03 1a 1b78 1b0d 1b4f41 1b4f48 1b5b313b3544 1b4f50 1b5b32347e 00"
  for mode in normal app; do
    "$HOLD_BIN" -dt --name "kb$mode" -- python3 "$TEST_ROOT/keybytes.py" "$mode" >/dev/null || return 1
    "$HOLD_BIN" wait "kb$mode" --match 'keys ready' --timeout 5000 || return 1
    for k in $keys; do "$HOLD_BIN" send "kb$mode" -k "$k" || return 1; done
    "$HOLD_BIN" send "kb$mode" Q || return 1
    "$HOLD_BIN" wait "kb$mode" --timeout 5000 >/dev/null || return 1
    got=$("$HOLD_BIN" logs "kb$mode" | tr -d '\r' | sed -n '2,18p' | tr '\n' ' ' | sed 's/ $//')
    if [ "$mode" = normal ]; then want=$want_normal; else want=$want_app; fi
    [ "$got" = "$want" ] || { echo "$mode: got  $got" >&2; echo "$mode: want $want" >&2; return 1; }
  done
}

# A dialog-style form driven by keyboard only: Down, Space, Tab, Tab,
# Shift+Tab back to <Ok>, Enter -> the checklist answers "2".
test_whiptail_form_by_keyboard() {
  command -v whiptail >/dev/null 2>&1 || skip_tool "whiptail not installed"
  "$HOLD_BIN" -dt --name wt -- /bin/sh -c 'r=$(whiptail --checklist "Select options:" 15 40 3 1 "Option A" off 2 "Option B" off 3 "Option C" off 3>&1 1>&2 2>&3); echo "rc=$? RESULT:$r"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait wt --match 'Option C' --timeout 5000 || return 1
  "$HOLD_BIN" wait wt --idle 300 --timeout 3000 || return 1
  "$HOLD_BIN" send wt -k Down -k Space --wait 200 || return 1
  "$HOLD_BIN" send wt -k Tab -k Tab -k S-Tab --wait 200 || return 1
  "$HOLD_BIN" send wt -k Enter || return 1
  "$HOLD_BIN" wait wt --match 'RESULT' --timeout 5000 || return 1
  "$HOLD_BIN" peek wt | grep -qF 'rc=0 RESULT:"2"' || { "$HOLD_BIN" peek wt >&2; return 1; }
  "$HOLD_BIN" end wt >/dev/null 2>&1 || true
}

# The legacy byte mouse encoding cannot reach past cell 222: send says so.
test_send_mouse_legacy_clamp_is_reported() {
  "$HOLD_BIN" -dt --name lm -- /bin/sh -c 'printf "\033[?1000hready\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait lm --match ready --timeout 5000 || return 1
  "$HOLD_BIN" resize lm 300 30 || return 1
  "$HOLD_BIN" send lm --mouse click,100,2 2>"$TEST_ROOT/lm1.err" || return 1
  ! grep -q 'legacy mouse encoding' "$TEST_ROOT/lm1.err" || return 1
  "$HOLD_BIN" send lm --mouse click,250,2 2>"$TEST_ROOT/lm2.err" || return 1
  grep -q 'legacy mouse encoding' "$TEST_ROOT/lm2.err" || { cat "$TEST_ROOT/lm2.err" >&2; return 1; }
  "$HOLD_BIN" end lm >/dev/null 2>&1 || true
}

# tmux under Callbox: prefix chord split, mouse pane focus, prefix detach.
test_tmux_prefix_mouse_and_detach() {
  command -v tmux >/dev/null 2>&1 || skip_tool "tmux not installed"
  [ -z "${HOLD_TEST_SOCKETS_DENIED:-}" ] || skip "tmux itself needs Unix sockets"
  local sock
  sock="$(actor_dir)/tmux.sock" || return 1
  "$HOLD_BIN" -dt --name tx -- tmux -S "$sock" -f /dev/null new-session \; set -g mouse on >/dev/null || return 1
  "$HOLD_BIN" wait tx --idle 500 --timeout 8000 || return 1
  "$HOLD_BIN" send tx -k C-b --text % --wait 500 || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" list-panes | wc -l | tr -d ' ')" -eq 2 ] || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" display -p '#{pane_index}')" = 1 ] || { tmux -S "$sock" kill-server; return 1; }
  "$HOLD_BIN" send tx --mouse click,5,3 --wait 400 || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" display -p '#{pane_index}')" = 0 ] || { tmux -S "$sock" kill-server; echo "mouse did not focus pane 0" >&2; return 1; }
  "$HOLD_BIN" send tx -k C-b --text d || { tmux -S "$sock" kill-server; return 1; }
  [ "$("$HOLD_BIN" wait tx --timeout 8000)" = 0 ] || { tmux -S "$sock" kill-server; return 1; }
  tmux -S "$sock" kill-server
}

# An ncurses program's own mouse handling: a click toggles a checkbox.
# (Multi-click flows are covered by the vim, tmux, and micro tests; on
# GitHub's runners this python-curses toy registers only its first click.)
test_curses_mouse_checklist() {
  python3 -c 'import curses' 2>/dev/null || skip_tool "python3 curses unavailable"
  cat >"$TEST_ROOT/checklist.py" <<'PY'
import curses, sys, time
items = ["Option A", "Option B", "Option C"]
log = open(sys.argv[1], "w")
def main(scr):
    curses.mousemask(curses.ALL_MOUSE_EVENTS | curses.REPORT_MOUSE_POSITION)
    curses.mouseinterval(0)
    checked = [False] * 3
    pressed = False
    while True:
        scr.erase()
        for i, it in enumerate(items):
            scr.addstr(3 + i, 4, "[%s] %s" % ("X" if checked[i] else " ", it))
        scr.addstr(7, 4, "<  OK  >")
        scr.refresh()
        if scr.getch() != curses.KEY_MOUSE: continue
        try:
            _, x, y, _, b = curses.getmouse()
        except curses.error:
            continue
        log.write("x=%d y=%d bstate=%#x\n" % (x, y, b)); log.flush()
        # ncurses versions differ in which of press/click/release they report
        # (and may merge quick clicks into double/triple clicks); act once per
        # click on whichever arrives first.
        if b & (curses.BUTTON1_PRESSED | curses.BUTTON1_CLICKED | curses.BUTTON1_DOUBLE_CLICKED | curses.BUTTON1_TRIPLE_CLICKED):
            pressed = True
        elif b & curses.BUTTON1_RELEASED:
            if pressed:
                pressed = False
                continue
        else:
            continue
        if 3 <= y <= 5 and 4 <= x <= 16: checked[y - 3] = not checked[y - 3]
        elif y == 7 and 4 <= x <= 11: return " ".join(str(i + 1) for i, c in enumerate(checked) if c)
print("RESULT:" + curses.wrapper(main)); sys.stdout.flush(); time.sleep(20)
PY
  W=$(actor_dir) || return 1
  "$HOLD_BIN" -dt --name cl -- python3 "$TEST_ROOT/checklist.py" "$W/mouse-events.log" >/dev/null || return 1
  "$HOLD_BIN" wait cl --match 'OK' --timeout 15000 || { "$HOLD_BIN" peek cl --json >&2; "$HOLD_BIN" logs cl >&2; return 1; }
  "$HOLD_BIN" wait cl --idle 300 --timeout 5000 || { "$HOLD_BIN" peek cl >&2; return 1; }
  "$HOLD_BIN" send cl --mouse click,8,4 --wait 300 || return 1
  "$HOLD_BIN" wait cl --match '[X] Option B' --timeout 5000 || {
    "$HOLD_BIN" peek cl >&2; echo "mouse events:" >&2; cat "$W/mouse-events.log" >&2; return 1; }
  grep -q 'bstate=' "$W/mouse-events.log" || return 1
  "$HOLD_BIN" end cl >/dev/null 2>&1 || true
}

# micro: click places the cursor, double-click selects a word, drag selects
# across lines, the wheel scrolls.
test_micro_mouse_editing() {
  command -v micro >/dev/null 2>&1 || skip_tool "micro not installed"
  W=$(actor_dir) || return 1
  mkdir -p "$W/mcfg" && { [ "${USER_ACTOR_NEEDS_SUDO:-0}" -ne 1 ] || chown "$TEST_UID:$TEST_GID" "$W/mcfg"; }
  seq -f 'alpha bravo charlie delta %g' 1 80 >"$W/m.txt"
  "$HOLD_BIN" -dt --name mi -- micro -config-dir "$W/mcfg" "$W/m.txt" >/dev/null || return 1
  "$HOLD_BIN" wait mi --match 'delta 5' --timeout 8000 || return 1
  "$HOLD_BIN" wait mi --idle 500 --timeout 5000 || return 1
  "$HOLD_BIN" send mi --mouse click,12,2 --wait 300 || return 1
  "$HOLD_BIN" peek mi --json | grep -q '"cursor": {"row": 2, "col": 12,' || { "$HOLD_BIN" peek mi --json | grep cursor >&2; return 1; }
  "$HOLD_BIN" send mi --mouse click,11,1 --mouse click,11,1 --wait 300 || return 1
  "$HOLD_BIN" send mi -k C-c -k End -k Enter -k C-v --wait 300 || return 1
  "$HOLD_BIN" peek mi | sed -n 3p | grep -qE '^ *3 bravo$' || { "$HOLD_BIN" peek mi | head -4 >&2; return 1; }
  "$HOLD_BIN" send mi --mouse press,9,4 --mouse drag,10,5 --mouse release,10,5 --wait 300 || return 1
  "$HOLD_BIN" send mi -k Delete --wait 300 || return 1
  "$HOLD_BIN" peek mi | grep -qE '^ *5 alpha ravo charlie delta 5$' || { "$HOLD_BIN" peek mi | head -7 >&2; return 1; }
  "$HOLD_BIN" send mi --mouse scroll-down,10,10 --wait 300 || return 1
  "$HOLD_BIN" peek mi | head -1 | grep -qv '^ *1 ' || return 1
  "$HOLD_BIN" end mi >/dev/null 2>&1 || true
}

# wait --idle never passes before the program's first output; and a program
# launched through a symlink (a venv's python) runs under the name it was given.
test_wait_idle_needs_first_output_and_symlink_launch() {
  "$HOLD_BIN" -dt --name slow -- /bin/sh -c 'sleep 1.5; echo drawn-late; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait slow --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek slow | grep -qx 'drawn-late' || { echo "idle passed before first output" >&2; return 1; }
  "$HOLD_BIN" end slow >/dev/null 2>&1 || true
  mkdir -p "$TEST_ROOT/bin"
  printf '#!/bin/sh\necho "argv0=$0"\n' >"$TEST_ROOT/bin/real-tool"
  chmod +x "$TEST_ROOT/bin/real-tool"
  ln -s real-tool "$TEST_ROOT/bin/alias-tool"
  ( cd "$TEST_ROOT" && "$HOLD_BIN" -d --name via-link -- bin/alias-tool ) >/dev/null || return 1
  "$HOLD_BIN" wait via-link --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs via-link | grep -q 'argv0=.*/bin/alias-tool$' || { "$HOLD_BIN" logs via-link >&2; return 1; }
  "$HOLD_BIN" inspect via-link | grep -q '"exe": ".*/bin/real-tool"' || return 1
}

# --since makes --match wait for a changed screen: text already showing is
# not a fresh answer.
test_wait_match_since_ignores_stale_screen() {
  "$HOLD_BIN" -dt --name stale -- /bin/sh -c 'stty -echo; echo "BLUE BACKGROUND"; while IFS= read -r l; do echo "again: $l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --timeout 5000 || return 1
  seq=$("$HOLD_BIN" peek stale --json | sed -n 's/.*"output_seq": \([0-9]*\).*/\1/p')
  set +e
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --since "$seq" --timeout 600 2>/dev/null; rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "stale match passed (rc $rc)" >&2; return 1; }
  seq=$("$HOLD_BIN" send stale --seq x --enter) || return 1
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --since "$seq" --timeout 5000 || return 1
  "$HOLD_BIN" end stale >/dev/null 2>&1 || true
}

# The screenshot footer wraps within the screen's width instead of stretching
# the image beside a narrow terminal: a 10x3 screen at scale 1, no ruler, puts
# CURSOR and SIZE on two lines (160 px wide: the longer one; 3 rows + gutter +
# 2 lines = 96 px high).
test_peek_image_footer_wraps_on_narrow_screens() {
  "$HOLD_BIN" -dt --name narrow -- /bin/sh -c 'stty -echo; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" resize narrow 10 3 || return 1
  "$HOLD_BIN" wait narrow --idle 300 --timeout 5000 >/dev/null 2>&1 || true
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek narrow --image "$W/n.png" --scale 1 --no-ruler || return 1
  python3 - "$W/n.png" <<'PY' || return 1
import struct, sys
w, h = struct.unpack(">II", open(sys.argv[1], "rb").read()[16:24])
assert (w, h) == (160, 96), "footer did not wrap: image is %dx%d" % (w, h)
PY
  "$HOLD_BIN" end narrow >/dev/null 2>&1 || true
}

# Screenshot fidelity: a rounded corner is an arc - it reaches the bottom and
# right edge midpoints like a square corner does, but leaves the cell centre
# (where a square corner's lines would meet) empty.
test_peek_image_rounded_corner() {
  "$HOLD_BIN" -dt --name arc -- /bin/sh -c 'printf "\342\225\255\n"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait arc --idle 300 --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek arc --image "$W/a.png" --scale 2 --no-ruler --no-footer || return 1
  python3 - "$W/a.png" <<'PY' || return 1
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24]); pos, idat = 8, b""
while pos < len(d):
    n, = struct.unpack(">I", d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b"IDAT": idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat); st = w * 3 + 1; px = []
for y in range(h):
    line = bytearray(raw[y * st + 1:(y + 1) * st])
    for i in range(3, len(line)): line[i] = (line[i] + line[i - 3]) & 255
    px.append(line)
at = lambda x, y: bytes(px[y][x * 3:x * 3 + 3])
bg = at(15, 31)   # scale 2: the cell is 16x32; its bottom-right corner is empty
assert at(7, 31) != bg, "corner does not reach the bottom edge"
assert at(15, 15) != bg, "corner does not reach the right edge"
assert at(7, 15) == bg, "square corner: the centre is inked"
PY
  "$HOLD_BIN" end arc >/dev/null 2>&1 || true
}

# Screenshot fidelity: a double-width emoji draws the anti-aliased Noto mask
# (many shades, not 1-bit), and invisible characters draw nothing: a
# variation selector, zero-width space, or joiner leaves the cell unchanged.
test_peek_image_emoji_and_invisibles() {
  # row 0: U+2705 U+FE0F   row 1: U+2705   row 2: a U+200B   row 3: a
  "$HOLD_BIN" -dt --name invis -- /bin/sh -c 'printf "\342\234\205\357\270\217\n\342\234\205\na\342\200\213\na\n"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait invis --idle 300 --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek invis --image "$W/i.png" --scale 2 --no-ruler --no-footer || return 1
  python3 - "$W/i.png" <<'PY' || return 1
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24]); pos, idat = 8, b""
while pos < len(d):
    n, = struct.unpack(">I", d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b"IDAT": idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat); st = w * 3 + 1; px = []
for y in range(h):
    line = bytearray(raw[y * st + 1:(y + 1) * st])
    for i in range(3, len(line)): line[i] = (line[i] + line[i - 3]) & 255
    px.append(line)
def cell(row, width):  # scale 2: cells are 16x32 px
    return [bytes(px[row * 32 + y][:16 * width * 3]) for y in range(32)]
assert cell(0, 2) == cell(1, 2), "a variation selector changed the emoji cell"
assert cell(2, 1) == cell(3, 1), "a zero-width space drew something"
shades = {line[i:i + 3] for line in cell(1, 2) for i in range(0, len(line), 3)}
assert len(shades) > 4, "emoji not anti-aliased: %d shades" % len(shades)
PY
  "$HOLD_BIN" end invis >/dev/null 2>&1 || true
}

# Screenshot fidelity: scripts beyond Latin/CJK draw real glyphs (Hebrew,
# Ethiopic - a wide Unifont glyph in a single cell - a sextant, a math letter),
# and a wide glyph squeezed into one cell keeps ink in both halves.
test_peek_image_draws_other_scripts() {
  # U+05D0 alef, U+1230 se, U+1FB00 sextant, U+1D400 bold A, U+E000 (private: missing)
  "$HOLD_BIN" -dt --name scripts -- /bin/sh -c 'printf "\327\220\n\341\210\260\n\360\237\254\200\n\360\235\220\200\n\356\200\200\n"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait scripts --idle 300 --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek scripts --image "$W/s.png" --scale 1 --no-ruler --no-footer || return 1
  python3 - "$W/s.png" <<'PY' || return 1
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24]); pos, idat = 8, b""
while pos < len(d):
    n, = struct.unpack(">I", d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b"IDAT": idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat); st = w * 3 + 1; px = []
for y in range(h):
    line = bytearray(raw[y * st + 1:(y + 1) * st])
    for i in range(3, len(line)): line[i] = (line[i] + line[i - 3]) & 255
    px.append(line)
def cell(row):  # the 8x16 block of the single cell at column 0
    return [bytes(px[row * 16 + y][:8 * 3]) for y in range(16)]
missing = cell(4)
for row, name in enumerate(["hebrew", "ethiopic", "sextant", "math"]):
    assert cell(row) != missing, name + " drawn as the missing-glyph box"
bg = bytes(px[0][0:3])
ink_x = {x for line in cell(1) for x in range(8) if line[x * 3:x * 3 + 3] != bg}
assert ink_x and min(ink_x) < 4 <= max(ink_x), "ethiopic not squeezed into its cell: ink at %s" % sorted(ink_x)
PY
  "$HOLD_BIN" end scripts >/dev/null 2>&1 || true
}

# Screenshot fidelity: CJK and emoji draw real glyphs, not the missing box.
test_peek_image_draws_cjk_and_emoji() {
  "$HOLD_BIN" -dt --name glyphs -- /bin/sh -c 'printf "\347\225\214\n\360\237\232\200\n\356\200\200\n\342\240\277\n"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait glyphs --idle 300 --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek glyphs --image "$W/g.png" --scale 1 --no-ruler --no-footer || return 1
  python3 - "$W/g.png" <<'PY' || return 1
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24]); pos, idat = 8, b""
while pos < len(d):
    n, = struct.unpack(">I", d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b"IDAT": idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat); st = w * 3 + 1; px = []
for y in range(h):
    line = bytearray(raw[y * st + 1:(y + 1) * st])
    for i in range(3, len(line)): line[i] = (line[i] + line[i - 3]) & 255
    px.append(line)
def cell(row):  # the 16x16 block of a wide cell at column 0
    return b"".join(bytes(px[row * 16 + y][:16 * 3]) for y in range(16))
cjk, emoji, missing = cell(0), cell(1), cell(2)
assert missing != cjk and missing != emoji, "CJK or emoji drawn as the missing-glyph box"
# Braille U+283F (row 3): its two dot columns must not touch - x=3,4 is a gap.
bg = bytes(px[3 * 16 + 1][0:3])
for y in range(3 * 16, 4 * 16):
    for x in (3, 4):
        assert bytes(px[y][x * 3:x * 3 + 3]) == bg, "braille dot columns touch"
PY
  "$HOLD_BIN" end glyphs >/dev/null 2>&1 || true
}

# A message is typed in at once, even into a program whose screen never goes
# quiet (a busy agent redraws its spinner constantly): an agent harness
# queues what is typed while it works, so waiting for quiet only blocked the
# sender until the receiver's turn was over.
test_msg_delivers_at_once_to_a_busy_receiver() {
  "$HOLD_BIN" -dt --name busy -- /bin/sh -c 'stty -echo; ( while :; do printf "\r spinner %s" "$(date +%N)"; sleep 0.05; done ) & while IFS= read -r l; do printf "\nGOT[%s]\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait busy --match spinner --timeout 5000 || return 1
  "$HOLD_BIN" resize busy 200 24 || return 1
  local t0 t1
  t0=$(date +%s)
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg busy "while you work" || return 1
  t1=$(date +%s)
  [ $((t1 - t0)) -le 2 ] || { echo "msg took $((t1 - t0)) s against a busy screen" >&2; return 1; }
  "$HOLD_BIN" wait busy --match 'while you work]' --timeout 3000 || { "$HOLD_BIN" peek busy >&2; return 1; }
  "$HOLD_BIN" end busy >/dev/null 2>&1 || true
}

# A full-screen program's last frame survives its switch back to the normal
# screen: peek --last-alt, live and after exit.
test_peek_last_alt_frame() {
  "$HOLD_BIN" -dt --name fs -- /bin/sh -c 'printf "\033[?1049hFULLSCREEN FRAME\n"; sleep 0.4; printf "\033[?1049l"; echo back to normal; sleep 1' >/dev/null || return 1
  "$HOLD_BIN" wait fs --match 'back to normal' --timeout 5000 || return 1
  "$HOLD_BIN" peek fs --last-alt | head -1 | grep -qx 'FULLSCREEN FRAME' || { "$HOLD_BIN" peek fs --last-alt >&2; return 1; }
  "$HOLD_BIN" wait fs --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" peek fs 2>"$TEST_ROOT/fs.err" | grep -qx 'back to normal' || return 1
  grep -q -- '--last-alt' "$TEST_ROOT/fs.err" || return 1
  "$HOLD_BIN" peek fs --last-alt | head -1 | grep -qx 'FULLSCREEN FRAME' || return 1
  "$HOLD_BIN" -dt --name plain -- /bin/sh -c 'echo no alt; sleep 0.2' >/dev/null || return 1
  "$HOLD_BIN" wait plain --timeout 5000 >/dev/null || return 1
  set +e; "$HOLD_BIN" peek plain --last-alt >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 5 ] || return 1
}

# ---- the screen verbs: peek / send / wait / resize -------------------------

# A TTY call's screen reads back as plain text, with a ruler, and as JSON.
test_peek_text_ruler_and_json() {
  "$HOLD_BIN" -dt --name scr -- /bin/sh -c 'printf "\033[1;32malpha\033[0m\nbeta\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait scr --match beta --timeout 5000 || return 1
  out=$("$HOLD_BIN" peek scr) || return 1
  [ "$(printf '%s\n' "$out" | sed -n 1p)" = alpha ] || { printf '%s\n' "$out" >&2; return 1; }
  [ "$(printf '%s\n' "$out" | sed -n 2p)" = beta ] || return 1
  [ "$("$HOLD_BIN" peek scr | wc -l | tr -d ' ')" -eq 24 ] || { echo "expected 24 rows" >&2; return 1; }
  "$HOLD_BIN" peek scr --ruler | sed -n 3p | grep -qx ' 0|alpha' || return 1
  "$HOLD_BIN" peek scr --json >"$TEST_ROOT/peek.json" || return 1
  python3 - "$TEST_ROOT/peek.json" <<'PY' || { cat "$TEST_ROOT/peek.json" >&2; return 1; }
import json, sys
d = json.load(open(sys.argv[1]))
assert d["name"] == "scr" and d["running"] is True and d["exit_code"] is None
assert (d["rows"], d["cols"]) == (24, 80)
assert d["cursor"] == {"row": 2, "col": 0, "visible": True}, d["cursor"]
assert d["lines"][:2] == ["alpha", "beta"] and len(d["lines"]) == 24
assert d["output_seq"] > 0 and d["mouse"] == "none" and d["cursor_keys"] == "normal"
PY
  "$HOLD_BIN" end scr >/dev/null 2>&1 || true
}

# send types text and presses keys; --wait returns once the program answered.
test_send_text_keys_and_wait() {
  "$HOLD_BIN" -dt --name typer -- /bin/sh -c 'stty -echo; echo ready; while IFS= read -r l; do printf "got:%s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait typer --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send typer "hello world" --enter --wait 200 --timeout 5000 || return 1
  "$HOLD_BIN" peek typer | grep -qx 'got:hello world' || { "$HOLD_BIN" peek typer >&2; return 1; }
  "$HOLD_BIN" send typer --text --not-a-flag -k Enter --wait 200 || return 1
  "$HOLD_BIN" peek typer | grep -qx 'got:--not-a-flag' || return 1
  set +e
  "$HOLD_BIN" send typer -k NoSuchKey 2>"$TEST_ROOT/badkey.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q "unknown key 'NoSuchKey'" "$TEST_ROOT/badkey.err" || return 1
  "$HOLD_BIN" send typer -k C-d || return 1
  "$HOLD_BIN" wait typer --timeout 5000 | grep -qx 0 || return 1
}

# Keys follow the program's modes: arrows switch to ESC O x under DECCKM.
test_send_keys_follow_cursor_key_mode() {
  "$HOLD_BIN" -dt --name keys -- /bin/sh -c 'printf "\033[?1h"; stty raw -echo; echo ready; dd bs=1 count=3 2>/dev/null | od -An -c; sleep 5' >/dev/null || return 1
  "$HOLD_BIN" wait keys --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send keys -k Up || return 1
  "$HOLD_BIN" wait keys --regex '033 +O +A' --timeout 5000 || { "$HOLD_BIN" peek keys >&2; return 1; }
  "$HOLD_BIN" end keys >/dev/null 2>&1 || true
}

# Bracketed paste wraps the text only when the program enabled mode 2004.
test_send_paste_brackets_when_requested() {
  "$HOLD_BIN" -dt --name paste -- /bin/sh -c 'printf "\033[?2004h"; stty raw -echo; echo ready; dd bs=1 count=15 2>/dev/null | od -An -c; sleep 5' >/dev/null || return 1
  "$HOLD_BIN" wait paste --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send paste --paste 'a b' || return 1
  "$HOLD_BIN" wait paste --regex '033 +\[ +2 +0 +0 +~ +a' --timeout 5000 || { "$HOLD_BIN" peek paste >&2; return 1; }
  "$HOLD_BIN" end paste >/dev/null 2>&1 || true
}

# wait: idle that never comes times out (7); a call that ends first is 8;
# no condition is docker wait (prints the exit code).
test_wait_timeout_ended_and_exit_code() {
  "$HOLD_BIN" -dt --name chatty -- /bin/sh -c 'while :; do echo tick; sleep 0.05; done' >/dev/null || return 1
  set +e
  "$HOLD_BIN" wait chatty --idle 400 --timeout 900 2>/dev/null; rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "idle on a chatty call: rc $rc, want 7" >&2; return 1; }
  "$HOLD_BIN" end chatty >/dev/null 2>&1 || true
  "$HOLD_BIN" -dt --name brief -- /bin/sh -c 'echo hi; sleep 0.5; exit 3' >/dev/null || return 1
  set +e
  "$HOLD_BIN" wait brief --match never-appears --timeout 10000 2>"$TEST_ROOT/ended.err"; rc=$?
  set -e
  [ "$rc" -eq 8 ] || { cat "$TEST_ROOT/ended.err" >&2; echo "rc $rc, want 8" >&2; return 1; }
  [ "$("$HOLD_BIN" wait brief)" = 3 ] || return 1
  "$HOLD_BIN" -dw --name waited -- /bin/sh -c 'exit 4' >/dev/null || return 1
  [ "$("$HOLD_BIN" wait waited --timeout 5000)" = 4 ] || return 1
  # A plain -d call leaves nothing behind to record its exit: wait says so.
  "$HOLD_BIN" -d --name plain -- /bin/sh -c 'exit 4' >/dev/null || return 1
  [ "$("$HOLD_BIN" wait plain --timeout 5000 2>"$TEST_ROOT/plain.err")" = -1 ] || return 1
  grep -q "records no exit code" "$TEST_ROOT/plain.err" || { cat "$TEST_ROOT/plain.err" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -Eq "Exited [0-9L].* plain$" || { "$HOLD_BIN" list -a >&2; return 1; }
}

# wait --idle counts silence from the start of the wait, and settles.
test_wait_idle_settles() {
  "$HOLD_BIN" -dt --name settle -- /bin/sh -c 'for i in 1 2 3 4 5; do echo "step $i"; sleep 0.1; done; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait settle --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek settle | grep -qx 'step 5' || { "$HOLD_BIN" peek settle >&2; return 1; }
  "$HOLD_BIN" end settle >/dev/null 2>&1 || true
}

# wait: a bare --idle (before the target, before a flag, or last) means 500 ms.
test_wait_bare_idle() {
  "$HOLD_BIN" -dt --name bare -- /bin/sh -c 'echo ready; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait --idle bare || return 1
  "$HOLD_BIN" wait bare --idle --timeout 3000 || return 1
  "$HOLD_BIN" wait bare --idle || return 1
  "$HOLD_BIN" wait --help | grep -q '500 if MS is left out' || return 1
  "$HOLD_BIN" end bare >/dev/null 2>&1 || true
}

# --help after the target prints the full help (usage and prose), exit 0.
test_help_after_target() {
  local v out
  for v in peek send wait resize msg logs; do
    out=$("$HOLD_BIN" "$v" some_call --help) || { echo "$v: --help after target failed" >&2; return 1; }
    [ "$(printf '%s\n' "$out" | grep -c .)" -ge 2 ] || { echo "$v: usage only, no help text" >&2; return 1; }
  done
  ! "$HOLD_BIN" logs --help | grep -q -- '--filter' || { echo "logs help lists removed --filter" >&2; return 1; }
}

# send/resize/msg on an ended call say it ended (exit 8), not "no call matches".
test_screen_verbs_on_ended_call() {
  "$HOLD_BIN" -dt --name gone -- /bin/sh -c 'echo bye' >/dev/null || return 1
  "$HOLD_BIN" wait gone >/dev/null || return 1
  local v rc
  for v in "send gone hi" "resize gone 50 10" "msg gone hi"; do
    set +e
    $HOLD_BIN $v 2>"$TEST_ROOT/gone.err"; rc=$?
    set -e
    [ "$rc" -eq 8 ] && grep -q 'has ended' "$TEST_ROOT/gone.err" || { cat "$TEST_ROOT/gone.err" >&2; echo "$v: rc $rc, want 8" >&2; return 1; }
  done
  "$HOLD_BIN" purge gone >/dev/null 2>&1 || true
}

# A call name may not start with '-' (it could never be targeted).
test_name_rejects_leading_dash() {
  if "$HOLD_BIN" -d --name -lead -- true >/dev/null 2>&1; then echo "accepted -lead" >&2; return 1; fi
  "$HOLD_BIN" -d --rm --name mid-dash -- true >/dev/null || return 1
}

# More text than one control request holds fails up front with a clear message.
test_send_too_large_says_so() {
  "$HOLD_BIN" -dt --name big -- sleep 30 >/dev/null || return 1
  set +e
  head -c 700000 /dev/zero | tr '\0' y | "$HOLD_BIN" send big --paste - 2>"$TEST_ROOT/big.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q 'too much text' "$TEST_ROOT/big.err" || { cat "$TEST_ROOT/big.err" >&2; echo "rc $rc" >&2; return 1; }
  head -c 100000 /dev/zero | tr '\0' y | "$HOLD_BIN" send big --paste - || return 1
  "$HOLD_BIN" end big >/dev/null 2>&1 || true
}

# More waiters than the broker has control slots: the rest are told "busy",
# back off, and still get their answer.
test_wait_more_clients_than_slots() {
  "$HOLD_BIN" -dt --name crowd -- /bin/sh -c 'echo start; sleep 2; echo GO; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait crowd --match start --timeout 5000 >/dev/null || return 1
  local i fails=0
  for i in $(seq 1 72); do
    ( "$HOLD_BIN" wait crowd --match GO --timeout 20000 >/dev/null 2>"$TEST_ROOT/crowd.$i.err" || echo fail >"$TEST_ROOT/crowd.$i.fail" ) &
  done
  wait
  for i in $(seq 1 72); do [ -e "$TEST_ROOT/crowd.$i.fail" ] && { fails=$((fails + 1)); cat "$TEST_ROOT/crowd.$i.err" >&2; }; done
  "$HOLD_BIN" end crowd >/dev/null 2>&1 || true
  [ "$fails" -eq 0 ] || { echo "$fails of 72 waiters failed" >&2; return 1; }
}

# The FIFO console (for environments that deny Unix sockets) keeps everything:
# peek/send/resize, a msg named by its sender's call, attach/detach/reattach, and a
# half-made channel is swept with the call.
test_fifo_console_full_capability() {
  local ep mid n
  CALLBOX_CONSOLE_TRANSPORT=fifo "$HOLD_BIN" -dt --name ff -- /bin/sh -c 'stty -echo; echo ready; while IFS= read -r l; do printf "GOT %s\n" "$l"; done' >/dev/null || return 1
  ep=$("$HOLD_BIN" inspect ff | sed -n 's/.*"console_sock": "\(.*\)",/\1/p')
  [ -p "$ep" ] || { ls -l "$ep" >&2; echo "the console endpoint is not a FIFO" >&2; return 1; }
  "$HOLD_BIN" wait ff --match ready --timeout 5000 || return 1
  "$HOLD_BIN" resize ff 200 24 || return 1
  "$HOLD_BIN" send ff hello --enter --wait 300 >/dev/null || return 1
  "$HOLD_BIN" peek ff | grep -qx 'GOT hello' || { "$HOLD_BIN" peek ff >&2; return 1; }
  "$HOLD_BIN" -dt --name fw -- /bin/sh -c "\"$HOLD_REAL_BIN\" msg ff 'from worker'; sleep 5" >/dev/null || return 1
  "$HOLD_BIN" wait ff --match 'from worker' --timeout 10000 || { "$HOLD_BIN" peek ff >&2; return 1; }
  "$HOLD_BIN" peek ff | grep -q '^GOT \[msg from fw; reply: callbox msg fw' || { "$HOLD_BIN" peek ff >&2; echo "sender not named over FIFO" >&2; return 1; }
  mid=$("$HOLD_BIN" msg ff --received | awk '/from worker$/ {print $1; exit}')
  "$HOLD_BIN" msg ff --received "$mid" | grep -qx 'from: fw' || { "$HOLD_BIN" msg ff --received >&2; return 1; }
  for n in 1 2; do
    "$HOLD_BIN" -dt --name "fv$n" -- "$HOLD_REAL_BIN" attach ff >/dev/null || return 1
    "$HOLD_BIN" wait "fv$n" --idle --timeout 5000 >/dev/null || return 1
    "$HOLD_BIN" send "fv$n" "via-attach-$n" --enter --wait 300 >/dev/null || return 1
    "$HOLD_BIN" send "fv$n" -k C-p -k C-q >/dev/null || return 1
    [ "$("$HOLD_BIN" wait "fv$n" --exit --timeout 5000)" = 0 ] || { echo "attach $n did not detach cleanly" >&2; return 1; }
  done
  [ "$("$HOLD_BIN" peek ff | grep -c '^GOT via-attach-')" = 2 ] || { "$HOLD_BIN" peek ff >&2; return 1; }
  as_user /bin/sh -c 'mkdir "$1" && mkfifo "$1/in" "$1/out"' sh "$ep.abcdef" || return 1
  "$HOLD_BIN" end ff fw >/dev/null 2>&1 || true
  "$HOLD_BIN" purge ff >/dev/null || return 1
  [ ! -e "$ep.abcdef" ] || { echo "half-made channel left behind" >&2; return 1; }
}

# A program that closes its terminal but runs on: the console stops taking
# requests at once (no stall), peek shows the last screen and says the program
# still runs, send says why it cannot, and wait --exit still gets the code.
test_console_closed_terminal_is_reported() {
  # On macOS a running session leader keeps its controlling terminal (and
  # cannot drop it: TIOCNOTTY fails), so the terminal stays open until the
  # program exits; the condition only arises on Linux-like systems.
  [ "$(uname -s)" != Darwin ] || skip "a running macOS session leader keeps its terminal"
  local mode start
  for mode in socket fifo; do
    CALLBOX_CONSOLE_TRANSPORT=$mode "$HOLD_BIN" -dt --name "ct$mode" -- /bin/sh -c 'echo before-close; exec </dev/null >/dev/null 2>&1; sleep 3; exit 4' >/dev/null || return 1
    "$HOLD_BIN" wait "ct$mode" --match before-close --timeout 5000 || return 1
    sleep 0.3
    start=$SECONDS
    "$HOLD_BIN" peek "ct$mode" >"$TEST_ROOT/ct.out" 2>"$TEST_ROOT/ct.err" || { cat "$TEST_ROOT/ct.err" >&2; return 1; }
    [ $((SECONDS - start)) -le 1 ] || { echo "$mode: peek stalled $((SECONDS - start))s" >&2; return 1; }
    grep -qx before-close "$TEST_ROOT/ct.out" && grep -q 'still running' "$TEST_ROOT/ct.err" || { cat "$TEST_ROOT/ct.out" "$TEST_ROOT/ct.err" >&2; return 1; }
    set +e
    "$HOLD_BIN" send "ct$mode" x 2>"$TEST_ROOT/ct.err"; rc=$?
    set -e
    [ "$rc" -eq 8 ] && grep -q 'closed its terminal' "$TEST_ROOT/ct.err" || { cat "$TEST_ROOT/ct.err" >&2; echo "$mode: send rc $rc, want 8" >&2; return 1; }
    [ "$("$HOLD_BIN" wait "ct$mode" --exit --timeout 10000)" = 4 ] || { echo "$mode: wait --exit lost the code" >&2; return 1; }
  done
}

# FIFO console access: root drives a user's call (as the socket console lets
# it), and an unrelated user gets nothing even with the directories opened up.
test_fifo_console_access() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  [ -n "$SUDO_BIN" ] || skip "sudo unavailable"
  command -v python3 >/dev/null 2>&1 || skip "python3 unavailable"
  local id ep cdir ack
  CALLBOX_CONSOLE_TRANSPORT=fifo "$HOLD_BIN" -dt --name fa -- /bin/sh -c 'echo SECRET-OUTPUT; while IFS= read -r l; do echo "got:$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait fa --match SECRET-OUTPUT --timeout 5000 || return 1
  id=$("$HOLD_BIN" inspect fa | sed -n 's/^  "id": "\(.*\)",$/\1/p')
  ep=$("$HOLD_BIN" inspect fa | sed -n 's/.*"console_sock": "\(.*\)",/\1/p')
  as_sudo_from_user "$HOLD_REAL_BIN" send "user:$id" from-root --enter --wait 300 >/dev/null || { echo "root could not drive the user's FIFO call" >&2; return 1; }
  "$HOLD_BIN" peek fa | grep -qx 'got:from-root' || { "$HOLD_BIN" peek fa >&2; return 1; }
  if id nobody >/dev/null 2>&1 && "$SUDO_BIN" -n -u nobody true >/dev/null 2>&1; then
    cdir=$(dirname "$ep")
    chmod 0711 "$ACTOR_HOME" "$ACTOR_HOME/.local" "$ACTOR_HOME/.local/state" "$ACTOR_HOME/.local/state/callbox" || return 1
    chmod 0733 "$cdir" && chmod 0622 "$ep" || return 1
    ack=$("$SUDO_BIN" -n -u nobody env -i PATH=/usr/bin:/bin python3 - "$ep" <<'PY'
import os, select, sys, tempfile
ep = sys.argv[1]
lst = os.open(ep, os.O_WRONLY | os.O_NONBLOCK)
x = tempfile.mkdtemp(prefix=os.path.basename(ep) + ".", dir=os.path.dirname(ep))
os.mkfifo(x + "/in", 0o600); os.mkfifo(x + "/out", 0o600)
os.open(x + "/in", os.O_RDONLY | os.O_NONBLOCK); os.open(x + "/in", os.O_WRONLY | os.O_NONBLOCK)
resp = os.open(x + "/out", os.O_RDONLY | os.O_NONBLOCK)
os.write(lst, ("C" + x[-6:] + "%010d" % os.getpid()).encode())
r, _, _ = select.select([resp], [], [], 2)
sys.stdout.write(repr(os.read(resp, 64) if r else b""))
for f in ("in", "out"): os.unlink(x + "/" + f)
os.rmdir(x)
PY
)
    chmod 0700 "$cdir" && chmod 0600 "$ep" || return 1
    case "$ack" in *R*|*SECRET*) echo "an unrelated user was answered: $ack" >&2; return 1 ;; esac
  fi
  "$HOLD_BIN" send fa owner-ok --enter --wait 300 >/dev/null || return 1
  "$HOLD_BIN" peek fa | grep -qx 'got:owner-ok' || return 1
  "$HOLD_BIN" end fa >/dev/null 2>&1 || true
}

# inspect reports what callbox observes, as list does: a call killed together with
# its reaper (as a sandbox that ends a tool call might) was never stamped, so its
# record still says running - inspect shows "exited" and keeps "recorded_state".
test_inspect_reports_observed_state() {
  local pid reaper
  "$HOLD_BIN" -dw --name killed -- sleep 60 >/dev/null || return 1
  sleep 0.3
  pid=$("$HOLD_BIN" inspect killed | sed -n 's/^  "pid": \([0-9]*\),$/\1/p')
  reaper=$(ps -o ppid= -p "$pid" | tr -d ' ')
  [ -n "$pid" ] && [ -n "$reaper" ] || return 1
  kill -KILL "$reaper" "$pid" 2>/dev/null || as_root kill -KILL "$reaper" "$pid" || return 1
  sleep 0.5
  "$HOLD_BIN" list -a | grep -Eq "Exited \(\?\).* killed$" || { "$HOLD_BIN" list -a >&2; return 1; }
  "$HOLD_BIN" inspect killed >"$TEST_ROOT/killed.json" || return 1
  grep -q '"state": "exited"' "$TEST_ROOT/killed.json" && grep -q '"recorded_state": "running"' "$TEST_ROOT/killed.json" ||
    { cat "$TEST_ROOT/killed.json" >&2; return 1; }
  python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$TEST_ROOT/killed.json" || return 1
  "$HOLD_BIN" purge killed >/dev/null 2>&1 || true
}

# The cross-shell machinery rides only on the fallback (Unix sockets denied,
# or CALLBOX_CONSOLE_TRANSPORT=fifo). On a normal host a plain -d call is the
# original: no control FIFO, no lifetime lock on its log, and a waiter blocked
# in waitpid until the child exits. Under the fallback it carries both.
test_plain_d_waiter_is_original_unless_fallback() {
  local id pid waiter log ctl fallback=0
  [ "${CALLBOX_CONSOLE_TRANSPORT:-}" = fifo ] || [ -n "${HOLD_TEST_SOCKETS_DENIED:-}" ] && fallback=1
  "$HOLD_BIN" -d --name plainwait -- sleep 6000 >/dev/null || return 1
  sleep 0.3
  id=$("$HOLD_BIN" inspect plainwait | sed -n 's/^  "id": "\(.*\)",$/\1/p')
  pid=$("$HOLD_BIN" inspect plainwait | sed -n 's/^  "pid": \([0-9]*\),$/\1/p')
  waiter=$(ps -o ppid= -p "$pid" | tr -d ' ')
  log=$(log_path "$id") || return 1
  ctl="$(dirname "$log")/console/$id.ctl"
  local locked
  locked=$(python3 -c 'import fcntl,sys
f = open(sys.argv[1])
try:
    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB); print("free")
except OSError:
    print("held")' "$log") || return 1
  if [ "$fallback" -eq 1 ]; then
    [ -p "$ctl" ] || { echo "fallback: no control FIFO" >&2; return 1; }
    [ "$locked" = held ] || { echo "fallback: log not locked" >&2; return 1; }
  else
    [ ! -e "$ctl" ] || { echo "normal host: a control FIFO appeared" >&2; return 1; }
    [ "$locked" = free ] || { echo "normal host: the log is locked" >&2; return 1; }
    if [ "$(uname -s)" = Linux ] && [ -r "/proc/$waiter/wchan" ]; then
      [ "$(cat "/proc/$waiter/wchan")" = do_wait ] || { echo "normal host: waiter not blocked in waitpid ($(cat "/proc/$waiter/wchan"))" >&2; return 1; }
    fi
  fi
  "$HOLD_BIN" end plainwait >/dev/null || return 1
  [ "$("$HOLD_BIN" wait plainwait)" = 143 ] || return 1
  [ ! -e "$ctl" ] || { echo "control FIFO left behind" >&2; return 1; }
  "$HOLD_BIN" purge plainwait >/dev/null 2>&1 || true
}

# Plain -d leaves nothing behind: one fork, setsid, output straight into the
# log, exec - the program's parent is not callbox. -dw keeps a waiter blocked in
# waitpid that records the exit code. On a normal host neither holds a lock on
# the log; under the fallback (Unix sockets denied, or CALLBOX_CONSOLE_TRANSPORT=
# fifo) the -d program's own output carries one, and the -dw waiter holds one,
# so shells in other PID namespaces can see them running.
log_lock_state() {
  python3 -c 'import fcntl,sys
f = open(sys.argv[1])
try:
    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB); print("free")
except OSError:
    print("held")' "$1"
}

test_plain_d_leaves_nothing_behind_dw_waits() {
  local fallback=0 want=free name pid parent id log
  if [ "${CALLBOX_CONSOLE_TRANSPORT:-}" = fifo ] || [ -n "${HOLD_TEST_SOCKETS_DENIED:-}" ]; then fallback=1; want=held; fi
  "$HOLD_BIN" -d --name plainwait -- sleep 6000 >/dev/null || return 1
  "$HOLD_BIN" -dw --name waited -- sleep 6000 >/dev/null || return 1
  sleep 0.3
  for name in plainwait waited; do
    id=$("$HOLD_BIN" inspect "$name" | sed -n 's/^  "id": "\(.*\)",$/\1/p')
    pid=$("$HOLD_BIN" inspect "$name" | sed -n 's/^  "pid": \([0-9]*\),$/\1/p')
    parent=$(ps -o ppid= -p "$pid" | tr -d ' ')
    log=$(log_path "$id") || return 1
    [ ! -e "$(dirname "$log")/console/$id.ctl" ] || { echo "$name: a control FIFO appeared" >&2; return 1; }
    [ "$(log_lock_state "$log")" = "$want" ] || { echo "$name: log lock is not $want (fallback=$fallback)" >&2; return 1; }
    if [ "$name" = plainwait ]; then
      ! ps -o args= -p "$parent" | grep -q callbox || { echo "plain -d left a callbox process: $(ps -o args= -p "$parent")" >&2; return 1; }
    else
      ps -o args= -p "$parent" | grep -q callbox || { echo "-dw has no waiter" >&2; return 1; }
      if [ "$(uname -s)" = Linux ] && [ -r "/proc/$parent/wchan" ]; then
        [ "$(cat "/proc/$parent/wchan")" = do_wait ] || { echo "waiter not blocked in waitpid: $(cat "/proc/$parent/wchan")" >&2; return 1; }
      fi
    fi
  done
  "$HOLD_BIN" end plainwait waited >/dev/null || return 1
  [ "$("$HOLD_BIN" wait waited)" = 143 ] || return 1
  [ "$("$HOLD_BIN" wait plainwait 2>/dev/null)" = -1 ] || return 1
  "$HOLD_BIN" purge plainwait waited >/dev/null 2>&1 || true
}

# Another PID namespace (some sandboxes give each shell its own): pids from
# the launching shell mean nothing there, so callbox follows the chain of command -
# a supervisor lock says the call is alive, and end/kill/send go through its
# broker or waiter. Run as the test user inside `unshare --pid`.
in_new_pid_ns() {
  local env_args=("HOME=$ACTOR_HOME" "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME" "PATH=$PATH")
  local name
  for name in HOLD_BOOT_ID_PATH HOLD_TEST_HOME_ROOT CALLBOX_CONSOLE_TRANSPORT; do
    if [ "${!name+x}" = x ]; then env_args+=("$name=${!name}"); fi
  done
  as_root unshare --pid --fork --mount-proc "$SUDO_BIN" -n -u "$TEST_USER" env "${env_args[@]}" "$@"
}

test_other_pid_namespace_follows_chain_of_command() {
  [ "$(uname -s)" = Linux ] || skip "PID namespaces are Linux"
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -n "$SUDO_BIN" ] || skip "needs root to make a PID namespace"
  as_root unshare --pid --fork --mount-proc true 2>/dev/null || skip "unshare --pid unavailable"
  local out
  # Launched here, driven and stopped from another namespace.
  # Only calls launched under the fallback carry the cross-shell lock: force it.
  CALLBOX_CONSOLE_TRANSPORT=fifo "$HOLD_BIN" -dt --name nst -- /bin/sh -c 'stty -echo; while IFS= read -r l; do echo "got:$l"; done' >/dev/null || return 1
  CALLBOX_CONSOLE_TRANSPORT=fifo "$HOLD_BIN" -dw --name nsd -- /bin/sh -c 'trap "exit 7" TERM; sleep 300 & wait' >/dev/null || return 1
  sleep 0.4
  out=$(in_new_pid_ns /bin/sh -c '
    "$1" list | grep -Eq "Up .* nst$" && "$1" list | grep -Eq "Up .* nsd$" || { echo "not shown as running"; exit 1; }
    "$1" send nst from-ns --enter --wait 300 >/dev/null && "$1" peek nst | grep -qx got:from-ns || { echo "send/peek failed"; exit 1; }
    # No broker for a non-TTY call: from here it can be seen, not ended.
    if msg=$("$1" end nsd 2>&1); then echo "ended a -d call from another namespace"; exit 1; fi
    case "$msg" in *"not running in this environment"*) ;; *) echo "no explanation for -d: $msg"; exit 1 ;; esac
    "$1" kill nst >/dev/null && [ "$("$1" wait nst)" = 137 ] || { echo "kill of the TTY call failed"; exit 1; }
    echo ok' sh "$HOLD_REAL_BIN" 2>/dev/null) || true
  [ "$out" = ok ] || { echo "from another namespace: $out" >&2; return 1; }
  "$HOLD_BIN" end nsd >/dev/null && [ "$("$HOLD_BIN" wait nsd)" = 7 ] || { echo "end of the -d call at home failed" >&2; return 1; }
  # Launched with a waiter under the fallback in a namespace that then ends
  # (its processes die with it): the waiter's lock is gone, so it ended -
  # Exited (?), and wait does not hang on an exit nobody will record. A plain
  # sweep removes it.
  in_new_pid_ns /bin/sh -c 'CALLBOX_CONSOLE_TRANSPORT=fifo "$1" -dw --name nsgone -- sleep 300 >/dev/null; sleep 0.5' sh "$HOLD_REAL_BIN" || return 1
  # A plain -d call (no supervisor; at most its own output holds a lock, which
  # a program may close) in a namespace that ends: this shell cannot tell, so
  # Unknown - sweeps skip it, end explains, only a named purge --force removes it.
  in_new_pid_ns /bin/sh -c '"$1" -d --name nsunk -- sleep 300 >/dev/null; sleep 0.5' sh "$HOLD_REAL_BIN" || return 1
  sleep 0.3
  "$HOLD_BIN" list -a | grep -Eq 'Exited \(\?\).* nsgone$' || { "$HOLD_BIN" list -a >&2; return 1; }
  "$HOLD_BIN" list -a | grep -Eq 'Unknown.* nsunk$' || { "$HOLD_BIN" list -a >&2; return 1; }
  local start=$SECONDS
  [ "$("$HOLD_BIN" wait nsgone 2>/dev/null)" = -1 ] && [ $((SECONDS - start)) -le 5 ] || { echo "wait hung or misreported" >&2; return 1; }
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  ! "$HOLD_BIN" list -a | grep -q ' nsgone$' || { echo "a sweep kept a call whose shell ended" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -q ' nsunk$' || { echo "a sweep removed a call it could not judge" >&2; return 1; }
  if "$HOLD_BIN" end nsunk 2>"$TEST_ROOT/nsunk-end.err"; then echo "end claimed an unknown call" >&2; return 1; fi
  grep -q "not running in this environment" "$TEST_ROOT/nsunk-end.err" || { cat "$TEST_ROOT/nsunk-end.err" >&2; return 1; }
  if "$HOLD_BIN" purge nsunk 2>/dev/null; then echo "purge removed an unknown call without --force" >&2; return 1; fi
  "$HOLD_BIN" purge nsunk --force >/dev/null || return 1
  ! "$HOLD_BIN" list -a | grep -q ' nsunk$' || return 1
  # (nst and nsd had ended, so the sweep above already removed them)
}

# resize: the program gets SIGWINCH and the screen reports the new size.
test_resize_delivers_sigwinch() {
  "$HOLD_BIN" -dt --name sized -- /bin/sh -c 'trap "echo SIZE:\$(stty size)" WINCH; echo ready; while :; do sleep 0.1; done' >/dev/null || return 1
  "$HOLD_BIN" wait sized --match ready --timeout 5000 || return 1
  "$HOLD_BIN" resize sized 100 30 || return 1
  "$HOLD_BIN" wait sized --match 'SIZE:30 100' --timeout 5000 || { "$HOLD_BIN" peek sized >&2; return 1; }
  "$HOLD_BIN" peek sized --json | grep -q '"size_set_by": "resize"' || return 1
  "$HOLD_BIN" resize sized 90x20 || return 1
  "$HOLD_BIN" wait sized --match 'SIZE:20 90' --timeout 5000 || return 1
  "$HOLD_BIN" end sized >/dev/null 2>&1 || true
}

# ---- terminal size: --size, the saved detach-size ---------------------------

# grep -q over a command's output, by way of a file. On a pipe grep -q hangs up
# at its first match; a writer cut off mid-write then fails the pipeline under
# pipefail, which turns a match into a failure (or, in an `if`, hides one).
out_has() {
  local pat="$1"
  shift
  "$@" >"$TEST_ROOT/out_has.txt" 2>/dev/null || true
  grep -q -- "$pat" "$TEST_ROOT/out_has.txt"
}

# A call placed from inside another call exists a moment after its launcher
# was started, not at once.
wait_call() {
  local i
  for i in $(seq 1 100); do
    out_has " $1\$" "$HOLD_BIN" list -a && return 0
    sleep 0.1
  done
  echo "the call $1 never appeared" >&2
  return 1
}

# What a TTY call's program saw on its very first look at its terminal: the
# call runs `stty size` at once, and this reads it back from the screen.
first_size() {
  "$HOLD_BIN" wait "$1" --regex '^[0-9]+ [0-9]+$' --timeout 8000 >/dev/null || return 1
  "$HOLD_BIN" peek "$1" | sed -n 1p
}

# --size COLS ROWS: the PTY and callbox's screen are that size before the program
# starts; it is where the call starts, not a lock, and redial replays it.
test_size_flag_sets_first_size() {
  "$HOLD_BIN" -dt --size 120 40 --name s1 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size s1)" = "40 120" ] || { "$HOLD_BIN" peek s1 >&2; return 1; }
  out_has '"rows": 40,' "$HOLD_BIN" peek s1 --json || return 1
  out_has '"cols": 120,' "$HOLD_BIN" peek s1 --json || return 1
  [ "$("$HOLD_BIN" peek s1 | wc -l | tr -d ' ')" -eq 40 ] || { echo "the screen is not 40 rows" >&2; return 1; }
  "$HOLD_BIN" -dt --size 90x20 --name s2 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size s2)" = "20 90" ] || { echo "--size 90x20 not honored" >&2; return 1; }
  "$HOLD_BIN" -dt --size=70 18 --name s3 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size s3)" = "18 70" ] || { echo "--size=70 18 not honored" >&2; return 1; }
  "$HOLD_BIN" resize s1 100 30 || return 1
  out_has '"cols": 100,' "$HOLD_BIN" peek s1 --json || { echo "--size locked the size" >&2; return 1; }
  # Redialed (detached again), the recipe's size comes back with it.
  "$HOLD_BIN" kill s2 >/dev/null 2>&1 || return 1
  "$HOLD_BIN" -dt s2 </dev/null >/dev/null || { echo "redial failed" >&2; return 1; }
  out_has '"cols": 90,' "$HOLD_BIN" peek s2 --json || { echo "redial did not replay --size" >&2; return 1; }
  out_has '"rows": 20,' "$HOLD_BIN" peek s2 --json || return 1
  "$HOLD_BIN" kill s2 >/dev/null 2>&1 || return 1
  "$HOLD_BIN" -dt --size 50 10 s2 </dev/null >/dev/null || return 1
  out_has '"cols": 50,' "$HOLD_BIN" peek s2 --json || { echo "--size on a redial did not override the recipe" >&2; return 1; }
}

# The initial size, in order: --size, the saved detach-size (detached launches
# only), the invoking terminal, 80x24. An outer call plays the invoking
# terminal, at a size of its own.
test_detach_size_default_and_precedence() {
  if "$HOLD_BIN" config get detach-size >/dev/null 2>&1; then echo "a fresh home has a detach-size" >&2; return 1; fi
  "$HOLD_BIN" config set detach-size 100 30 2>/dev/null || return 1
  [ "$("$HOLD_BIN" config get detach-size)" = "100 30" ] || return 1
  grep -qx 'detach-size 100 30' "$HOME/.config/callbox/config" || { echo "setting not saved where documented" >&2; return 1; }
  "$HOLD_BIN" -dt --name d1 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size d1)" = "30 100" ] || { echo "-dt ignored the saved default" >&2; return 1; }
  "$HOLD_BIN" -dwt --name d2 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size d2)" = "30 100" ] || { echo "-dwt ignored the saved default" >&2; return 1; }
  "$HOLD_BIN" -dt --size 60 15 --name d3 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size d3)" = "15 60" ] || { echo "--size did not beat the saved default" >&2; return 1; }
  # Launched from a 77x21 terminal: detached takes the default, foreground the terminal.
  "$HOLD_BIN" -dt --size 77 21 --name outer -- /bin/sh -c "\"$HOLD_REAL_BIN\" -dt --name inner -- /bin/sh -c 'stty size; sleep 30'; \"$HOLD_REAL_BIN\" -it --name fg -- /bin/sh -c 'stty size; sleep 30'" </dev/null >/dev/null || return 1
  wait_call inner || return 1
  [ "$(first_size inner)" = "30 100" ] || { echo "the invoking terminal beat the saved default" >&2; return 1; }
  wait_call fg || return 1
  [ "$(first_size fg)" = "21 77" ] || { echo "a foreground launch used the saved default" >&2; return 1; }
  # A new default never touches a call that is already running.
  "$HOLD_BIN" config set detach-size 50 12 2>/dev/null || return 1
  out_has '"cols": 100,' "$HOLD_BIN" peek d1 --json || { echo "config set resized a running call" >&2; return 1; }
  # Without a default: the invoking terminal when there is one, else 80x24.
  "$HOLD_BIN" config unset detach-size 2>/dev/null || return 1
  if "$HOLD_BIN" config get detach-size >/dev/null 2>&1; then echo "unset left the default" >&2; return 1; fi
  "$HOLD_BIN" config unset detach-size 2>/dev/null || { echo "unset is not idempotent" >&2; return 1; }
  "$HOLD_BIN" -dt --size 66 19 --name outer2 -- /bin/sh -c "\"$HOLD_REAL_BIN\" -dt --name inner2 -- /bin/sh -c 'stty size; sleep 30'; sleep 30" </dev/null >/dev/null || return 1
  wait_call inner2 || return 1
  [ "$(first_size inner2)" = "19 66" ] || { echo "the invoking terminal's size was not used" >&2; return 1; }
  "$HOLD_BIN" -dt --name d4 -- /bin/sh -c 'stty size; sleep 30' </dev/null >/dev/null || return 1
  [ "$(first_size d4)" = "24 80" ] || { echo "no terminal and no default should be 80x24" >&2; return 1; }
  # A person's own symlinks are followed: ~/.config kept somewhere else.
  as_user rm -rf "$HOME/.config" || return 1
  as_user mkdir "$HOME/dotfiles" || return 1
  as_user ln -s dotfiles "$HOME/.config" || return 1
  "$HOLD_BIN" config set detach-size 64 16 2>/dev/null || { echo "config set failed through a symlinked ~/.config" >&2; return 1; }
  grep -qx 'detach-size 64 16' "$HOME/dotfiles/callbox/config" || { echo "the setting did not follow the symlink" >&2; return 1; }
  [ -L "$HOME/.config" ] || { echo "config set replaced the symlink" >&2; return 1; }
  [ "$("$HOLD_BIN" config get detach-size)" = "64 16" ] || return 1
  # Under sudo a launch may read the invoking user's settings; root does not
  # change them.
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    local rc
    [ "$(as_sudo_from_user "$HOLD_REAL_BIN" config get detach-size)" = "64 16" ] || { echo "sudo did not read the invoking user's setting" >&2; return 1; }
    set +e
    as_sudo_from_user "$HOLD_REAL_BIN" config set detach-size 90 20 >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 2 ] || { echo "sudo callbox config set exited $rc" >&2; return 1; }
    [ "$("$HOLD_BIN" config get detach-size)" = "64 16" ] || { echo "sudo changed the user's setting" >&2; return 1; }
  fi
}

# One grammar and one set of bounds for --size, config set, and resize. A
# refused size starts nothing and replaces nothing.
test_invalid_sizes_start_nothing() {
  local bad rc
  for bad in "0 40" "40 0" "1001 5" "5 1001" "-5 40" "12a 40" "120x" "x40" "120" ""; do
    set +e
    # shellcheck disable=SC2086
    "$HOLD_BIN" -dt --size $bad --name bad -- /bin/sh -c 'sleep 30' </dev/null >/dev/null 2>"$TEST_ROOT/size.err"; rc=$?
    set -e
    [ "$rc" -eq 1 ] || { echo "--size '$bad' exited $rc" >&2; cat "$TEST_ROOT/size.err" >&2; return 1; }
    grep -q -- '--size' "$TEST_ROOT/size.err" || { echo "--size '$bad': no explanation" >&2; return 1; }
    [ -z "$("$HOLD_BIN" list -a | sed 1d)" ] || { echo "--size '$bad' started a call" >&2; "$HOLD_BIN" list -a >&2; return 1; }
  done
  # --size never allocates a PTY by itself, and belongs to a launch.
  set +e
  "$HOLD_BIN" -d --size 120 40 -- /bin/sh -c 'sleep 30' </dev/null >/dev/null 2>"$TEST_ROOT/size.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q 'add -t' "$TEST_ROOT/size.err" || { echo "--size without -t was not refused" >&2; return 1; }
  [ -z "$("$HOLD_BIN" list -a | sed 1d)" ] || { echo "--size without -t started a call" >&2; return 1; }
  set +e
  "$HOLD_BIN" --size 120 40 list >/dev/null 2>&1; rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "--size with a verb exited $rc" >&2; return 1; }

  "$HOLD_BIN" config set detach-size 100 30 2>/dev/null || return 1
  for bad in "0 40" "1001x5" "-5 40" "120" "90x25 7" "banana"; do
    set +e
    # shellcheck disable=SC2086
    "$HOLD_BIN" config set detach-size $bad >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 1 ] || { echo "config set detach-size '$bad' exited $rc" >&2; return 1; }
    [ "$("$HOLD_BIN" config get detach-size)" = "100 30" ] || { echo "a refused value replaced the saved one ('$bad')" >&2; return 1; }
  done
  set +e
  "$HOLD_BIN" config set no-such-setting 1 >/dev/null 2>&1; rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1

  # A saved value that is not a size (a hand edit) is not guessed at.
  printf 'detach-size banana\n' >"$HOME/.config/callbox/config" || return 1
  set +e
  "$HOLD_BIN" -dt --name guess -- /bin/sh -c 'sleep 30' </dev/null >/dev/null 2>"$TEST_ROOT/size.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q 'saved detach-size' "$TEST_ROOT/size.err" || { echo "a bad saved size was not refused" >&2; cat "$TEST_ROOT/size.err" >&2; return 1; }
  [ -z "$("$HOLD_BIN" list -a | sed 1d)" ] || { echo "a bad saved size started a call" >&2; return 1; }
  "$HOLD_BIN" config unset detach-size 2>/dev/null || return 1

  "$HOLD_BIN" -dt --size 80 24 --name live -- /bin/sh -c 'sleep 30' </dev/null >/dev/null || return 1
  for bad in "0 10" "10 0" "2000 10" "12a 10" "80"; do
    set +e
    # shellcheck disable=SC2086
    "$HOLD_BIN" resize live $bad >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 1 ] || { echo "resize '$bad' exited $rc" >&2; return 1; }
  done
  out_has '"cols": 80,' "$HOLD_BIN" peek live --json || { echo "a refused resize changed the size" >&2; return 1; }
}

# ---- callbox shell: a person's shell with the chat window over it ---------------

# A callbox shell draws on a terminal, so an outer TTY call plays the person's
# terminal: `callbox peek term` is exactly what they would see, and `callbox send
# term` types and clicks as they would. The shell itself is the call "work".
start_shell_in_terminal() {
  local cols="$1" rows="$2"
  shift 2
  "$HOLD_BIN" -dt --size "$cols" "$rows" --name term -- "$HOLD_REAL_BIN" shell --name work "$@" </dev/null >/dev/null || return 1
  # the corner hint shows that the composited display is up
  "$HOLD_BIN" wait term --match 'callbox shell | Ctrl-P Ctrl-T: chat' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
}

# Where a piece of text sits on a TTY call's screen: "COL ROW" of its first
# occurrence, in cells from zero.
screen_pos() {
  "$HOLD_BIN" peek "$1" --json | python3 -c '
import json, sys
for row, line in enumerate(json.load(sys.stdin)["lines"]):
    col = line.find(sys.argv[1])
    if col >= 0:
        print(col, row)
        sys.exit(0)
sys.exit(1)' "$2"
}

# The terminal reports the mouse (the chat window is up, or the program asked
# for it): only then does a click sent to it arrive.
wait_mouse() {
  local i
  for i in $(seq 1 80); do
    out_has '"mouse": "\(click\|drag\|move\)"' "$HOLD_BIN" peek "$1" --json && return 0
    sleep 0.1
  done
  echo "$1 never asked for the mouse" >&2
  return 1
}

# Until a piece of text sits at COL ROW on a call's screen (a frame may still
# be on its way when the keys or clicks that caused it have been sent).
wait_pos() {
  local i
  for i in $(seq 1 80); do
    [ "$(screen_pos "$1" "$2" 2>/dev/null)" = "$3 $4" ] && return 0
    sleep 0.1
  done
  echo "'$2' never reached $3,$4 on $1's screen (it is at: $(screen_pos "$1" "$2" 2>/dev/null))" >&2
  "$HOLD_BIN" peek "$1" >&2
  return 1
}

wait_gone() {
  local i
  for i in $(seq 1 80); do
    "$HOLD_BIN" peek "$1" >"$TEST_ROOT/wait_gone.txt" 2>/dev/null || true
    grep -qF -- "$2" "$TEST_ROOT/wait_gone.txt" || return 0
    sleep 0.1
  done
  echo "'$2' never left $1's screen" >&2
  "$HOLD_BIN" peek "$1" >&2
  return 1
}

# The chat window is drawn over the shell, never into it.
shell_untouched() {
  if out_has 'chat ·\|who to talk to\|\[ \] screen' "$HOLD_BIN" peek work; then
    echo "the chat window is on the shell's own screen" >&2
    "$HOLD_BIN" peek work >&2
    return 1
  fi
}

# An agent stand-in: echoes what is typed at its prompt and answers every
# "ping" with an ordinary callbox msg to the call "work". It knows nothing of the
# chat window.
write_agent() {
  cat >"$1/agent.sh" <<EOF || return 1
#!/bin/sh
echo AGENT READY
n=0
while IFS= read -r line; do
  printf 'GOT: %s\n' "\$line"
  case "\$line" in *ping*) n=\$((n + 1)); "$HOLD_REAL_BIN" msg work "pong \$n" ;; esac
done
EOF
  chmod 755 "$1/agent.sh"
}

# callbox shell's own grammar: its options end at the shell's name, a shell is
# never detached or headless, and --chat-key moves the shortcut.
test_shell_launch_grammar() {
  local rc
  "$HOLD_BIN" -dt --size 80 24 --name term -- "$HOLD_REAL_BIN" shell --name work --size 70 20 /bin/sh -c 'echo "ARGS:$0:$1:$2"; sleep 30' zero --name -d </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait term --match 'ARGS:zero:--name:-d' --timeout 8000 || { echo "the terminal does not show the shell" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait work --match 'ARGS:zero:--name:-d' --timeout 8000 || { "$HOLD_BIN" peek work >&2; return 1; }
  # --size was where it started; attached, it follows the terminal.
  out_has '"cols": 80,' "$HOLD_BIN" peek work --json || { echo "the shell did not take its terminal's size" >&2; return 1; }
  out_has '"chat": true' "$HOLD_BIN" inspect work || { echo "the call is not recorded as a shell with chat" >&2; "$HOLD_BIN" inspect work >&2; return 1; }
  # No shell named: $SHELL.
  "$HOLD_BIN" -dt -e SHELL=/bin/sh --name term2 -- "$HOLD_REAL_BIN" shell --name plain </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait term2 --match 'callbox shell | Ctrl-P Ctrl-T: chat' --timeout 8000 || { "$HOLD_BIN" peek term2 >&2; return 1; }
  out_has '"/bin/sh" .* plain$' "$HOLD_BIN" list || { echo "callbox shell did not start \$SHELL" >&2; "$HOLD_BIN" list >&2; return 1; }
  # --chat-key: the lead key, then that key, opens chat; the default one is
  # then just another key, and goes through with the lead key.
  "$HOLD_BIN" -dt --name term3 -- "$HOLD_REAL_BIN" shell --name keyed --chat-key ctrl-g /bin/sh -c 'stty raw -echo; echo KEYED; exec cat -v' </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait term3 --match KEYED --timeout 8000 || { "$HOLD_BIN" peek term3 >&2; return 1; }
  "$HOLD_BIN" send term3 -k C-p -k C-t || return 1
  "$HOLD_BIN" wait keyed --match '^P^T' --timeout 8000 || { echo "Ctrl-P Ctrl-T did not reach the shell under --chat-key" >&2; "$HOLD_BIN" peek keyed >&2; return 1; }
  "$HOLD_BIN" send term3 -k C-p -k C-g || return 1
  "$HOLD_BIN" wait term3 --match 'who to talk to' --timeout 8000 || { echo "--chat-key did not open chat" >&2; "$HOLD_BIN" peek term3 >&2; return 1; }
  for args in "shell -d" "shell --restart always" "shell --no-such-option" "-d --chat-key ctrl-g -- /bin/sh" "shell --chat-key x" "shell --chat-key ctrl-q" "shell --chat-key ctrl-p" "shell --detach-keys ctrl-a"; do
    set +e
    # shellcheck disable=SC2086
    "$HOLD_BIN" $args </dev/null >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 1 ] || { echo "callbox $args exited $rc" >&2; return 1; }
  done
  # No terminal to draw on: refused, and nothing is started.
  set +e
  "$HOLD_BIN" shell --name headless /bin/sh </dev/null >/dev/null 2>"$TEST_ROOT/shell.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q 'needs a terminal' "$TEST_ROOT/shell.err" || { echo "callbox shell without a terminal exited $rc" >&2; return 1; }
  ! out_has ' headless$' "$HOLD_BIN" list -a || { echo "callbox shell without a terminal left a call" >&2; return 1; }
  out_has 'Ctrl-P Ctrl-T' "$HOLD_BIN" help shell || return 1
}

# The acceptance flow: a named shell, a separate agent; a request from the
# chat window, a reply by ordinary callbox msg. The reply is in chat and nowhere
# in the shell. Again with the window hidden: an unread badge, no interruption.
test_shell_chat_request_and_reply() {
  local d
  d="$(actor_dir)" || return 1
  write_agent "$d" || return 1
  start_shell_in_terminal 100 30 /bin/sh || return 1
  "$HOLD_BIN" -dt --size 200 20 --name agent -- "$d/agent.sh" </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait agent --match 'AGENT READY' --timeout 8000 || return 1
  # Open chat, choose the agent by name, send.
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'who to talk to' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term 'agen' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'message agent' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term 'ping from the chat window' -k Enter || return 1
  # It reaches the agent's prompt as a callbox msg from the shell's call...
  "$HOLD_BIN" wait agent --regex '^GOT: \[msg from work; reply: callbox msg work "\.\.\."\] ping from the chat window$' --timeout 10000 || { "$HOLD_BIN" peek agent >&2; return 1; }
  # ...and the agent's plain callbox msg reply appears in chat,
  "$HOLD_BIN" wait term --match 'pong 1' --timeout 10000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --match 'delivered' --timeout 8000 || { echo "the sent message is not marked delivered" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  # in the agent's conversation, never at the shell prompt, on its screen, or in its log.
  out_has 'chat · agent' "$HOLD_BIN" peek term || { "$HOLD_BIN" peek term >&2; return 1; }
  if out_has 'pong' "$HOLD_BIN" peek work; then echo "the reply was typed into the shell" >&2; "$HOLD_BIN" peek work >&2; return 1; fi
  if out_has 'pong\|chat ·\|ping from' "$HOLD_BIN" logs work; then echo "chat text reached the shell's log" >&2; return 1; fi
  shell_untouched || return 1
  "$HOLD_BIN" msg work --received | grep -Eq '^[0-9a-f]{6} .* agent +pong 1$' || { "$HOLD_BIN" msg work --received >&2; return 1; }
  # Hidden: the conversation stays, the shell goes on, a new reply waits behind a badge.
  "$HOLD_BIN" send term -k Escape || return 1
  wait_gone term 'chat · agent' || return 1
  "$HOLD_BIN" wait term --match 'chat hidden | Ctrl-P Ctrl-T: bring it back' --timeout 8000 || { echo "hiding chat did not say how to bring it back" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  if out_has 'chat hidden' "$HOLD_BIN" peek work; then echo "the tip reached the shell's screen" >&2; return 1; fi
  "$HOLD_BIN" send term 'echo typed-at-the-prompt' -k Enter || return 1
  "$HOLD_BIN" wait work --regex '^typed-at-the-prompt$' --timeout 8000 || { echo "the shell did not get the keyboard back" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  "$HOLD_BIN" send agent 'ping again' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'chat: 1 new from agent' --timeout 10000 || { echo "no unread badge" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  if out_has 'pong 2\|chat · agent' "$HOLD_BIN" peek term; then echo "a reply opened the hidden window" >&2; return 1; fi
  if out_has 'pong\|chat:' "$HOLD_BIN" peek work; then echo "a hidden reply reached the shell" >&2; "$HOLD_BIN" peek work >&2; return 1; fi
  # Opening shows it, after the first exchange.
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'pong 2' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  out_has 'ping from the chat window' "$HOLD_BIN" peek term || { echo "the conversation was not kept" >&2; return 1; }
  out_has 'pong 1' "$HOLD_BIN" peek term || return 1
  wait_gone term 'chat: 1 new' || return 1
  # The selected recipient is kept: a follow-up needs no new choice. With
  # "Include screen" it carries the shell's clean screen.
  "$HOLD_BIN" send term -k C-s 'ping with my screen' -k Enter || return 1
  "$HOLD_BIN" wait agent --match 'ping with my screen' --timeout 10000 || { "$HOLD_BIN" peek agent >&2; return 1; }
  out_has 'screen of work: 100 columns x 30 rows' "$HOLD_BIN" peek agent || { echo "the screen did not go with the message" >&2; "$HOLD_BIN" peek agent >&2; return 1; }
  out_has 'typed-at-the-prompt' "$HOLD_BIN" peek agent || { echo "the shared screen lacks the shell's content" >&2; return 1; }
  if out_has 'chat · agent\|\[x\] screen' "$HOLD_BIN" peek agent; then echo "the shared screen shows the chat window" >&2; return 1; fi
  "$HOLD_BIN" wait term --match 'pong 3' --timeout 10000 || return 1
  out_has '\[ \] screen' "$HOLD_BIN" peek term || { echo "Include screen stayed on after one message" >&2; return 1; }
}

# A message that cannot be delivered says so; a message joins the conversation
# of the call that sent it, and one from outside any call gets its own, which
# has no one to answer.
test_shell_chat_failures_and_outside_senders() {
  local d
  d="$(actor_dir)" || return 1
  write_agent "$d" || return 1
  start_shell_in_terminal 100 30 /bin/sh || return 1
  "$HOLD_BIN" -dt --size 200 20 --name agent -- "$d/agent.sh" </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait agent --match 'AGENT READY' --timeout 8000 || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'who to talk to' --timeout 8000 || return 1
  "$HOLD_BIN" send term 'agent' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'message agent' --timeout 8000 || return 1
  "$HOLD_BIN" kill agent >/dev/null 2>&1 || return 1
  "$HOLD_BIN" send term 'are you there' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'NOT DELIVERED' --timeout 10000 || { echo "a failed send was not reported" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  out_has 'agent is not running' "$HOLD_BIN" peek term || { "$HOLD_BIN" peek term >&2; return 1; }
  if out_has 'you .* delivered' "$HOLD_BIN" peek term; then echo "a failed send reads as delivered" >&2; return 1; fi
  # Named by its call's name: the agent's conversation, open on screen.
  CALLBOX_ID='' CALLBOX_NAME=agent "$HOLD_BIN" msg work 'by name' || return 1
  "$HOLD_BIN" wait term --match 'by name' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  # From outside any call: a conversation of its own.
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg work 'from a plain terminal' || return 1
  "$HOLD_BIN" wait term --match '+1 new' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  if out_has 'from a plain terminal' "$HOLD_BIN" peek term; then echo "an outside message joined the agent's conversation" >&2; "$HOLD_BIN" peek term >&2; return 1; fi
  "$HOLD_BIN" send term -k Tab || return 1
  "$HOLD_BIN" wait term --match 'outside callbox  1 new' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term 'outside' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'from a plain terminal' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  out_has 'chat · outside callbox' "$HOLD_BIN" peek term || return 1
  "$HOLD_BIN" send term 'hello?' -k Enter || return 1
  "$HOLD_BIN" wait term --match 'no call to answer' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  shell_untouched || return 1
  if out_has 'by name\|plain terminal' "$HOLD_BIN" peek work; then echo "a message was typed into the shell" >&2; return 1; fi
}

# Chat over a continuously redrawing full-screen program: the shell is live
# under the window, and moving, resizing, or closing the window uncovers what
# the program shows now. callbox peek never contains the window.
test_shell_overlay_over_live_program() {
  command -v python3 >/dev/null 2>&1 || { skip_tool "python3 not installed"; return; }
  local d x y
  d="$(actor_dir)" || return 1
  # every row carries the frame number across its whole width
  cat >"$d/ticker.sh" <<'EOF' || return 1
#!/bin/sh
printf '\033[?1049h\033[?25l'
i=100000
while :; do
  i=$((i + 1)); r=0
  while [ $r -lt 30 ]; do
    printf '\033[%d;1Hr%02d %s %s %s %s %s %s %s %s %s %s %s %s %s' $((r + 1)) $r $i $i $i $i $i $i $i $i $i $i $i $i $i
    r=$((r + 1))
  done
  sleep 0.05
done
EOF
  chmod 755 "$d/ticker.sh" || return 1
  # Every row shows one frame number, all across, none older than $2: nothing
  # the program has since redrawn is left over from when the window covered it.
  fresh() {
    "$HOLD_BIN" peek term --json | python3 -c '
import json, re, sys
floor = int(sys.argv[1])
rows = [l for l in json.load(sys.stdin)["lines"] if re.match(r"r\d\d ", l)]
assert len(rows) >= 20, "the program is not on screen"
newest = 0
for line in rows:
    nums = [int(n) for n in re.findall(r"(?<!\d)\d{6}(?!\d)", line)]
    seen = set(nums)
    assert seen, "a row shows nothing of the program: " + line
    # A row the program is redrawing at this instant: the new frame as a
    # prefix, the one just before after it (the screen was read between two
    # pieces of one write - macOS PTYs deliver writes in small pieces).
    if len(seen) == 2 and max(seen) == min(seen) + 1:
        k = nums.index(min(seen))
        mid_redraw = all(n == max(seen) for n in nums[:k]) and all(n == min(seen) for n in nums[k:])
    else:
        mid_redraw = False
    assert len(seen) == 1 or mid_redraw, "a row mixes frames: " + line
    assert min(seen) >= floor, "a stale cell (frame %d < %d): %s" % (min(seen), floor, line)
    newest = max(newest, max(seen))
print(newest)' "$1"
  }
  start_shell_in_terminal 100 30 "$d/ticker.sh" || return 1
  "$HOLD_BIN" wait term --match 'r29 1' --timeout 10000 || { echo "the program never drew" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'who to talk to' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  wait_mouse term || return 1
  t0=$(fresh 0) || { "$HOLD_BIN" peek term >&2; return 1; }
  sleep 0.5
  t1=$(fresh "$t0") || { "$HOLD_BIN" peek term >&2; return 1; }
  [ "$t1" -gt "$t0" ] || { echo "the program stopped updating under the window" >&2; return 1; }
  shell_untouched || return 1
  # Move it by its title bar: what it covered shows the current frame.
  read -r x y < <(screen_pos term 'chat ·') || return 1
  "$HOLD_BIN" send term --mouse "press,$x,$y" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 20)),$((y + 5))" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 40)),$((y + 10))" || return 1
  "$HOLD_BIN" send term --mouse "release,$((x - 40)),$((y + 10))" || return 1
  "$HOLD_BIN" wait term --regex "^r$(printf '%02d' $((y + 10))) [0-9 ]{4}.* chat · who to talk to" --timeout 8000 || { echo "the window did not move" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  sleep 0.3
  t2=$(fresh "$t1") || { echo "moving the window uncovered stale cells" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  # Resize it from the lower-right corner: the strip it gave up is current too.
  read -r x y < <(screen_pos term '◢') || return 1
  "$HOLD_BIN" send term --mouse "press,$x,$y" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 12)),$((y - 5))" || return 1
  "$HOLD_BIN" send term --mouse "release,$((x - 12)),$((y - 5))" || return 1
  wait_pos term '◢' $((x - 12)) $((y - 5)) || { echo "the window did not resize" >&2; return 1; }
  sleep 0.3
  t3=$(fresh "$t2") || { echo "resizing the window uncovered stale cells" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  shell_untouched || return 1
  # Close it with the ×: the whole screen is the program's again.
  read -r x y < <(screen_pos term '×') || return 1
  "$HOLD_BIN" send term --mouse "click,$x,$y" || return 1
  wait_gone term 'who to talk to' || return 1
  sleep 0.3
  fresh "$t3" >/dev/null || { echo "closing the window left stale cells" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  [ "$("$HOLD_BIN" peek term | grep -c '^r[0-9][0-9] ')" -eq 30 ] || { echo "the program does not fill the screen again" >&2; return 1; }
  shell_untouched || return 1
  if out_has 'who to talk to' "$HOLD_BIN" logs work; then echo "the chat window reached the shell's log" >&2; return 1; fi
}

# A real window resize reaches the shell's program, chat open or closed;
# moving or resizing only the chat window never does.
test_shell_resize_reaches_program_overlay_does_not() {
  command -v python3 >/dev/null 2>&1 || { skip_tool "python3 not installed"; return; }
  local x y winches
  start_shell_in_terminal 100 30 /bin/sh -c 'trap "echo WINCH \$(stty size)" WINCH; echo ready; while :; do sleep 0.1; done' || return 1
  "$HOLD_BIN" wait work --match ready --timeout 8000 || return 1
  # chat closed
  "$HOLD_BIN" resize term 90 26 || return 1
  "$HOLD_BIN" wait work --match 'WINCH 26 90' --timeout 8000 || { echo "no resize with chat closed" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  out_has '"cols": 90,' "$HOLD_BIN" peek work --json || return 1
  # chat open: the program is told, and the window refits inside the new bounds
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'who to talk to' --timeout 8000 || return 1
  "$HOLD_BIN" resize term 110 32 || return 1
  "$HOLD_BIN" wait work --match 'WINCH 32 110' --timeout 8000 || { echo "no resize with chat open" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  "$HOLD_BIN" resize term 40 9 || return 1
  "$HOLD_BIN" wait work --match 'WINCH 9 40' --timeout 8000 || return 1
  "$HOLD_BIN" wait term --match 'chat ·' --timeout 8000 || { echo "the title bar left the shrunken terminal" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  screen_pos term '×' >/dev/null || { echo "the close control left the shrunken terminal" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" resize term 100 30 || return 1
  "$HOLD_BIN" wait work --match 'WINCH 30 100' --timeout 8000 || return 1
  wait_mouse term || return 1
  winches=$("$HOLD_BIN" logs work | grep -c WINCH)
  # only the chat window: moved, resized, hidden, shown
  read -r x y < <(screen_pos term 'chat ·') || return 1
  "$HOLD_BIN" send term --mouse "press,$x,$y" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 30)),$((y + 6))" || return 1
  "$HOLD_BIN" send term --mouse "release,$((x - 30)),$((y + 6))" || return 1
  read -r x y < <(screen_pos term '◢') || return 1
  "$HOLD_BIN" send term --mouse "press,$x,$y" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 8)),$((y - 3))" || return 1
  "$HOLD_BIN" send term --mouse "release,$((x - 8)),$((y - 3))" || return 1
  "$HOLD_BIN" send term -k Escape || return 1
  wait_gone term 'who to talk to' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'who to talk to' --timeout 8000 || return 1
  sleep 0.5
  out_has '"cols": 100,' "$HOLD_BIN" peek work --json || { echo "the chat window changed the shell's size" >&2; return 1; }
  out_has '"rows": 30,' "$HOLD_BIN" peek work --json || return 1
  [ "$("$HOLD_BIN" logs work | grep -c WINCH)" -eq "$winches" ] || { echo "the chat window made the shell see a resize" >&2; "$HOLD_BIN" peek work >&2; return 1; }
}

# Chat has the keyboard while it is focused, and a drag that starts on the
# window is the window's until the button is released. A program that asked
# for the mouse still gets clicks outside the window. × and Esc keep the
# conversation.
test_shell_chat_input_never_reaches_program() {
  command -v python3 >/dev/null 2>&1 || { skip_tool "python3 not installed"; return; }
  local x y before
  start_shell_in_terminal 100 30 /bin/sh -c 'printf "\033[?1000h\033[?1006h"; stty raw -echo; echo APP; exec cat -v' || return 1
  "$HOLD_BIN" wait work --match APP --timeout 8000 || return 1
  # with chat hidden the program's mouse works as ever
  wait_mouse term || return 1
  "$HOLD_BIN" send term --mouse click,5,10 || return 1
  "$HOLD_BIN" wait work --match '^[[<0;6;11M^[[<0;6;11m' --timeout 8000 || { echo "no mouse with chat hidden" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  # a message waiting in chat, so there is a conversation to keep
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg work 'kept through closing' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'kept through closing' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  before=$("$HOLD_BIN" peek work)
  # typing goes to chat
  "$HOLD_BIN" send term 'secret words' || return 1
  "$HOLD_BIN" wait term --match '> secret words' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  # a drag from the title bar, across the window's edge, released far outside
  read -r x y < <(screen_pos term 'chat ·') || return 1
  "$HOLD_BIN" send term --mouse "press,$x,$y" || return 1
  "$HOLD_BIN" send term --mouse "drag,$((x - 20)),$((y + 4))" || return 1
  "$HOLD_BIN" send term --mouse "drag,2,20" || return 1
  "$HOLD_BIN" send term --mouse "release,2,20" || return 1
  # and a click on the window itself
  read -r x y < <(screen_pos term 'kept through closing') || return 1
  "$HOLD_BIN" send term --mouse "click,$x,$y" || return 1
  sleep 0.5
  [ "$("$HOLD_BIN" peek work)" = "$before" ] || { echo "chat input reached the program" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  if out_has 'secret' "$HOLD_BIN" logs work; then echo "chat keystrokes reached the shell's log" >&2; return 1; fi
  # a click outside the window is the program's, and so is the keyboard after it
  read -r x y < <(screen_pos term '×') || return 1
  "$HOLD_BIN" send term --mouse "click,$((x + 3)),$y" || return 1
  "$HOLD_BIN" wait work --match "^[[<0;$((x + 4));$((y + 1))M^[[<0;$((x + 4));$((y + 1))m" --timeout 8000 || { echo "a click outside the window did not reach the program" >&2; "$HOLD_BIN" peek work >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --match 'Ctrl-P Ctrl-T: chat | typing in the shell' --timeout 8000 || { echo "the window did not say where typing goes" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term 'Q' || return 1
  "$HOLD_BIN" wait work --regex 'mQ$' --timeout 8000 || { echo "the keyboard did not return to the shell" >&2; "$HOLD_BIN" peek work >&2; return 1; }
  # closed by the ×, the conversation and the unsent draft are still there...
  "$HOLD_BIN" send term --mouse "click,$x,$y" || return 1
  wait_gone term 'kept through closing' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'kept through closing' --timeout 8000 || { echo "closing with the x lost the conversation" >&2; return 1; }
  out_has '> secret words' "$HOLD_BIN" peek term || { echo "closing lost the draft" >&2; return 1; }
  # ...and by Esc as well
  "$HOLD_BIN" send term -k Escape || return 1
  wait_gone term 'kept through closing' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'kept through closing' --timeout 8000 || { echo "Esc lost the conversation" >&2; return 1; }
  shell_untouched || return 1
}

# callbox -it is docker run -it: when the program ends, callbox exits as it did, and
# what it printed is still there to read - also when it is over before the
# attach gets to it (that used to be an error, with the output lost).
test_tty_foreground_exit_status_and_last_screen() {
  local rc
  cat > "$TEST_ROOT/it-rc.sh" <<EOF
#!/bin/sh
"$HOLD_REAL_BIN" -it --name itq -- sh -c 'echo inner-quick; exit 7'; echo "IT_QUICK_RC=\$?"
"$HOLD_REAL_BIN" -it --name its -- sh -c 'sleep 1; echo inner-slow; exit 4'; echo "IT_SLOW_RC=\$?"
"$HOLD_REAL_BIN" -it --name itz -- true; echo "IT_TRUE_RC=\$?"
echo IT_DONE; sleep 60
EOF
  chmod +x "$TEST_ROOT/it-rc.sh"
  "$HOLD_BIN" -dt --size 100 20 --name itterm -- "$TEST_ROOT/it-rc.sh" </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait itterm --match IT_DONE --timeout 20000 || { "$HOLD_BIN" peek itterm >&2; return 1; }
  "$HOLD_BIN" peek itterm > "$TEST_ROOT/it-rc.out" || return 1
  grep -q '^IT_QUICK_RC=7$' "$TEST_ROOT/it-rc.out" || { echo "-it did not exit as a fast command did" >&2; cat "$TEST_ROOT/it-rc.out" >&2; return 1; }
  grep -q '^IT_SLOW_RC=4$' "$TEST_ROOT/it-rc.out" || { echo "-it did not exit as its command did" >&2; cat "$TEST_ROOT/it-rc.out" >&2; return 1; }
  grep -q '^IT_TRUE_RC=0$' "$TEST_ROOT/it-rc.out" || { cat "$TEST_ROOT/it-rc.out" >&2; return 1; }
  grep -q '^inner-quick$' "$TEST_ROOT/it-rc.out" || { echo "a fast -it command's output was lost" >&2; cat "$TEST_ROOT/it-rc.out" >&2; return 1; }
  grep -q '^inner-slow$' "$TEST_ROOT/it-rc.out" || { echo "an -it command's last screen was not left on the terminal" >&2; cat "$TEST_ROOT/it-rc.out" >&2; return 1; }
  if grep -q 'no such file\|No such file\|error' "$TEST_ROOT/it-rc.out"; then cat "$TEST_ROOT/it-rc.out" >&2; return 1; fi
  # with no terminal to attach, the output is the log's and the status still the command's
  rc=0
  "$HOLD_BIN" -it --name itp -- sh -c 'echo piped-quick; exit 5' </dev/null > "$TEST_ROOT/it-pipe.out" 2> "$TEST_ROOT/it-pipe.err" || rc=$?
  [ "$rc" -eq 5 ] || { echo "-it without a terminal: rc=$rc, expected the command's 5" >&2; cat "$TEST_ROOT/it-pipe.err" >&2; return 1; }
  grep -q 'piped-quick' "$TEST_ROOT/it-pipe.out" || { echo "-it without a terminal lost the output" >&2; cat "$TEST_ROOT/it-pipe.err" >&2; return 1; }
}

# However an attach ends, what the program last showed stays on the terminal
# and callbox says which ending it was: the call ended while attached, or had
# ended already, or closed its terminal and runs on (its client is then let
# go at once, not left on a dead screen until the program exits).
test_attach_endings_leave_the_last_screen() {
  cat > "$TEST_ROOT/endings.sh" <<EOF
#!/bin/sh
"$HOLD_REAL_BIN" -dt --name bgend -- sh -c 'echo bg-line-one; sleep 2; echo bg-last-words; exit 6' >/dev/null
"$HOLD_REAL_BIN" attach bgend; echo "ATTACH_RC=\$?"
"$HOLD_REAL_BIN" attach bgend; echo "AGAIN_RC=\$?"
"$HOLD_REAL_BIN" -it --rm --name rmend -- sh -c 'echo rm-quick; exit 3'; echo "RM_RC=\$?"
echo ENDINGS_DONE; sleep 60
EOF
  chmod +x "$TEST_ROOT/endings.sh"
  "$HOLD_BIN" -dt --size 110 20 --name endterm -- "$TEST_ROOT/endings.sh" </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait endterm --match ENDINGS_DONE --timeout 30000 || { "$HOLD_BIN" peek endterm >&2; return 1; }
  "$HOLD_BIN" peek endterm > "$TEST_ROOT/endings.out" || return 1
  grep -q '^bg-last-words$' "$TEST_ROOT/endings.out" || { echo "the last screen of a call that ended while attached was lost" >&2; cat "$TEST_ROOT/endings.out" >&2; return 1; }
  # it ended while attached: attach exits 8, as wait does for an ended call
  grep -q '^ATTACH_RC=8$' "$TEST_ROOT/endings.out" || { cat "$TEST_ROOT/endings.out" >&2; return 1; }
  grep -q '^AGAIN_RC=8$' "$TEST_ROOT/endings.out" || { cat "$TEST_ROOT/endings.out" >&2; return 1; }
  grep -q "^callbox: bgend has ended - see 'callbox logs bgend'" "$TEST_ROOT/endings.out" || { echo "attach to an ended call did not say so" >&2; cat "$TEST_ROOT/endings.out" >&2; return 1; }
  grep -q '^rm-quick$' "$TEST_ROOT/endings.out" && grep -q '^RM_RC=3$' "$TEST_ROOT/endings.out" || { echo "-it --rm lost its output or exit status" >&2; cat "$TEST_ROOT/endings.out" >&2; return 1; }
  if grep -q 'exit status unknown' "$TEST_ROOT/endings.out"; then cat "$TEST_ROOT/endings.out" >&2; return 1; fi

  [ "$(uname -s)" != Darwin ] || return 0 # a running macOS session leader keeps its terminal
  cat > "$TEST_ROOT/closing.sh" <<EOF
#!/bin/sh
"$HOLD_REAL_BIN" -it --name closer -- sh -c 'echo before-closing; sleep 1; exec >/dev/null 2>&1 </dev/null; sleep 60'; echo "CLOSER_RC=\$?"
sleep 60
EOF
  chmod +x "$TEST_ROOT/closing.sh"
  "$HOLD_BIN" -dt --size 110 12 --name closeterm -- "$TEST_ROOT/closing.sh" </dev/null >/dev/null || return 1
  # well before the program's own 60 s are up
  "$HOLD_BIN" wait closeterm --match 'CLOSER_RC=0' --timeout 15000 || { echo "an attach to a program that closed its terminal was not let go" >&2; "$HOLD_BIN" peek closeterm >&2; return 1; }
  out_has '^before-closing$' "$HOLD_BIN" peek closeterm || { echo "its last screen was lost" >&2; "$HOLD_BIN" peek closeterm >&2; return 1; }
  out_has 'closer has closed its terminal and runs on' "$HOLD_BIN" peek closeterm || { "$HOLD_BIN" peek closeterm >&2; return 1; }
  out_has ' closer$' "$HOLD_BIN" list -l || { echo "the program did not run on" >&2; return 1; }
  "$HOLD_BIN" kill closer >/dev/null 2>&1 || true
}

# A mistyped command line is an error, said plainly, with nothing started and
# nothing ended: exit 1 for a usage error everywhere, 5 only for "no such call".
test_cli_mistakes_are_usage_errors() {
  local rc args
  "$HOLD_BIN" -dw --name=eqname -- sleep 60 </dev/null >/dev/null || { echo "--name=NAME was not accepted" >&2; return 1; }
  out_has ' eqname$' "$HOLD_BIN" list -l || { echo "--name=NAME did not name the call" >&2; return 1; }
  # a mistyped flag is not a program to run; a launch flag means nothing to a verb
  while IFS='|' read -r args want; do
    # shellcheck disable=SC2086
    rc=0
    "$HOLD_BIN" $args </dev/null > "$TEST_ROOT/mistake.out" 2> "$TEST_ROOT/mistake.err" || rc=$?
    [ "$rc" -eq 1 ] || { echo "callbox $args: rc=$rc, expected 1" >&2; cat "$TEST_ROOT/mistake.err" >&2; return 1; }
    grep -q -- "$want" "$TEST_ROOT/mistake.err" || { echo "callbox $args: no '$want' in:" >&2; cat "$TEST_ROOT/mistake.err" >&2; return 1; }
  done <<'EOF'
--bogus true|unknown flag '--bogus'
-x true|unknown flag '-x'
--privileged -d true|--privileged is not supported
-d|usage: callbox
--size 80 24 -d true|add -t
-d list|-d is for placing a call
--rm ps|--rm is for placing a call
--name x end eqname|--name is for placing a call
-t list|-t is for placing a call
end|usage: callbox end
end --all eqname|unknown flag '--all'
kill --all eqname|unknown flag '--all'
inspect eqname --bogus|unknown flag '--bogus'
inspect eqname other|usage: callbox inspect
save --bogus eqname|unknown flag '--bogus'
logs eqname --bogus|unknown flag '--bogus'
attach eqname --detach-keys nope|invalid --detach-keys
help no-such-topic|unknown help topic
EOF
  if out_has 'bogus\|privileged' "$HOLD_BIN" list -a; then echo "a mistyped launch still placed a call" >&2; return 1; fi
  out_has ' eqname$' "$HOLD_BIN" list -l || { echo "a refused command line still ended the call" >&2; return 1; }
  # -h is help wherever it stands among a verb's words, and acts on nothing
  for args in "end eqname -h" "kill -h eqname" "inspect eqname --help" "list -a -h"; do
    # shellcheck disable=SC2086
    "$HOLD_BIN" $args </dev/null > "$TEST_ROOT/mistake.out" 2> "$TEST_ROOT/mistake.err" || { echo "callbox $args failed" >&2; cat "$TEST_ROOT/mistake.err" >&2; return 1; }
    grep -q '^usage: callbox ' "$TEST_ROOT/mistake.out" || { echo "callbox $args did not print help" >&2; return 1; }
  done
  out_has ' eqname$' "$HOLD_BIN" list -l || { echo "end -h ended the call" >&2; return 1; }
  # ...but a dash-led word after -- is the command's name, and "no such call" keeps its own code
  "$HOLD_BIN" -- --no-such-program </dev/null > /dev/null 2> "$TEST_ROOT/mistake.err" || true
  grep -q "cannot start '--no-such-program'" "$TEST_ROOT/mistake.err" || { cat "$TEST_ROOT/mistake.err" >&2; return 1; }
  rc=0
  "$HOLD_BIN" end no-such-call-here </dev/null > /dev/null 2> "$TEST_ROOT/mistake.err" || rc=$?
  [ "$rc" -eq 5 ] || { echo "end <no such call>: rc=$rc, expected 5" >&2; return 1; }
  "$HOLD_BIN" kill eqname >/dev/null 2>&1 || return 1
}

# Every help page fits an 80-column terminal, says what the command does, and
# callbox help launch lists the flags for placing a call.
test_help_pages_fit_and_say_it() {
  local topic wide
  for topic in "" launch targets system scripting list ps end stop kill tail logs inspect ports stats peek send wait msg resize config shell attach console purge save rename; do
    # shellcheck disable=SC2086
    "$HOLD_BIN" help $topic > "$TEST_ROOT/help.out" 2> "$TEST_ROOT/help.err" || { echo "callbox help $topic failed" >&2; return 1; }
    [ -s "$TEST_ROOT/help.out" ] || { echo "callbox help $topic printed nothing" >&2; return 1; }
    wide=$(awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$TEST_ROOT/help.out")
    [ "$wide" -le 80 ] || { echo "callbox help $topic: a line is $wide columns wide" >&2; return 1; }
  done
  for topic in peek send wait msg; do
    "$HOLD_BIN" "$topic" x --bogus > /dev/null 2> "$TEST_ROOT/help.err" || true
    wide=$(awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$TEST_ROOT/help.err")
    [ "$wide" -le 80 ] || { echo "callbox $topic usage: a line is $wide columns wide" >&2; return 1; }
  done
  "$HOLD_BIN" help launch > "$TEST_ROOT/help.out" || return 1
  for topic in '-d, --detach' '-w, --wait' '-t, --tty' '--size COLS ROWS' '--name NAME' '--env-file' '--rm' '--restart POLICY' '--detach-keys'; do
    grep -q -- "$topic" "$TEST_ROOT/help.out" || { echo "callbox help launch does not list $topic" >&2; return 1; }
  done
  out_has 'callbox help launch' "$HOLD_BIN" help || { echo "callbox help does not point at the launch page" >&2; return 1; }
  out_has 'from now on' "$HOLD_BIN" help tail || return 1
  out_has 'log_path' "$HOLD_BIN" help inspect || return 1
  if out_has '--plain' "$HOLD_BIN" help inspect; then echo "help inspect still names --plain" >&2; return 1; fi
  out_has 'usage error' "$HOLD_BIN" help scripting || return 1
}

# The lead key (Ctrl-P) waits for the key after it, however long that takes,
# and a callbox shell shows what it offers while it waits. The detach key
# detaches, the chat key opens chat, the lead key again sends one through, and
# anything else sends both through: nothing typed is lost.
test_lead_key_waits_and_offers() {
  local t
  # in a callbox shell (its broker reads the keys), and in a plain -it call (the
  # attach client does)
  for t in shell plain; do
    if [ "$t" = shell ]; then
      "$HOLD_BIN" -dt --size 100 16 --name "term-$t" -- "$HOLD_REAL_BIN" shell --name "in-$t" /bin/sh -c 'stty raw -echo; echo KEYS; exec cat -v' </dev/null >/dev/null || return 1
    else
      "$HOLD_BIN" -dt --size 100 16 --name "term-$t" -- "$HOLD_REAL_BIN" -it --name "in-$t" -- /bin/sh -c 'stty raw -echo; echo KEYS; exec cat -v' </dev/null >/dev/null || return 1
    fi
    "$HOLD_BIN" wait "term-$t" --match KEYS --timeout 8000 || { "$HOLD_BIN" peek "term-$t" >&2; return 1; }
    # pressed alone, it waits: nothing reaches the program, even well past
    # the half second it used to be held for
    "$HOLD_BIN" send "term-$t" -k C-p || return 1
    sleep 1.2
    if out_has '\^P' "$HOLD_BIN" peek "in-$t"; then echo "($t) the lead key did not wait for the next key" >&2; return 1; fi
    if [ "$t" = shell ]; then
      out_has 'Ctrl-Q: detach | Ctrl-T: chat | Ctrl-P: send Ctrl-P' "$HOLD_BIN" peek "term-$t" || { echo "the lead key's menu is not on screen" >&2; "$HOLD_BIN" peek "term-$t" >&2; return 1; }
      if out_has 'Ctrl-Q: detach' "$HOLD_BIN" peek "in-$t"; then echo "the menu reached the shell's own screen" >&2; return 1; fi
    fi
    # the lead key again: one goes through
    "$HOLD_BIN" send "term-$t" -k C-p || return 1
    "$HOLD_BIN" wait "in-$t" --regex '^ *\^P$' --timeout 8000 || { echo "($t) the lead key twice did not send one through" >&2; "$HOLD_BIN" peek "in-$t" >&2; return 1; }
    wait_gone "term-$t" 'Ctrl-Q: detach' || return 1
    # any other key: both go through, in order - a letter, and a key that is an escape sequence
    "$HOLD_BIN" send "term-$t" -k C-p || return 1
    "$HOLD_BIN" send "term-$t" x || return 1
    "$HOLD_BIN" wait "in-$t" --regex '^ *\^P\^Px$' --timeout 8000 || { echo "($t) the lead key and the next key did not both go through" >&2; "$HOLD_BIN" peek "in-$t" >&2; return 1; }
    "$HOLD_BIN" send "term-$t" -k C-p -k Up || return 1
    "$HOLD_BIN" wait "in-$t" --match '^P^Px^P^[[A' --timeout 8000 || { echo "($t) the lead key before an arrow key was lost" >&2; "$HOLD_BIN" peek "in-$t" >&2; return 1; }
    # the detach key, after a pause no timer allows for: detached, still running
    "$HOLD_BIN" send "term-$t" -k C-p || return 1
    sleep 1.2
    "$HOLD_BIN" send "term-$t" -k C-q || return 1
    "$HOLD_BIN" wait "term-$t" --timeout 8000 >/dev/null || { echo "($t) a slow Ctrl-P ... Ctrl-Q did not detach" >&2; return 1; }
    out_has " in-$t\$" "$HOLD_BIN" list -l || { echo "($t) detaching ended the call" >&2; return 1; }
    out_has "callbox: detached from in-$t; it is still running" "$HOLD_BIN" peek "term-$t" || { echo "($t) no word on detaching" >&2; "$HOLD_BIN" peek "term-$t" >&2; return 1; }
  done
}

# The draft is edited like a line of text: any script, a paste kept whole (its
# newlines too), and none of it sent before Enter.
test_shell_chat_draft_editing() {
  start_shell_in_terminal 100 30 /bin/sh || return 1
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg work 'hello' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'chat · outside callbox' --timeout 8000 || return 1
  "$HOLD_BIN" send term 'héllo wörld 日本語' || return 1
  "$HOLD_BIN" wait term --match '> héllo wörld 日本語' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k Backspace -k Backspace -k Home 'X' -k End '!' || return 1
  "$HOLD_BIN" wait term --match '> Xhéllo wörld 日!' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k C-u || return 1
  wait_gone term 'Xhéllo' || return 1
  # a paste arrives whole, as text: its newline starts a second line of the draft
  "$HOLD_BIN" send term --paste "$(printf 'first line\nsecond line')" || return 1
  "$HOLD_BIN" wait term --match '> first line' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --regex '^ +  second line$' --timeout 8000 || { echo "the pasted newline was lost" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k M-Enter 'third' || return 1
  "$HOLD_BIN" wait term --regex '^ +  third$' --timeout 8000 || { "$HOLD_BIN" peek term >&2; return 1; }
  # nothing typed or pasted into the draft went anywhere
  if out_has 'first line\|llo' "$HOLD_BIN" peek work; then echo "the draft reached the shell" >&2; return 1; fi
  shell_untouched || return 1
}

# Can a call be picked up right now? An attach on a terminal of its own shows
# the call's screen (a second client would be told "console already attached"
# and exit); it is then detached again. Retried: a console is freed a moment
# after its client goes, not in the same instant.
PROBES=0
console_free() {
  local i n
  for i in $(seq 1 30); do
    PROBES=$((PROBES + 1))
    n="probe$PROBES"
    "$HOLD_BIN" -dt --name "$n" -- "$HOLD_REAL_BIN" attach "$1" </dev/null >/dev/null || return 1
    if "$HOLD_BIN" wait "$n" --match "$2" --timeout 2000 >/dev/null 2>&1; then
      "$HOLD_BIN" send "$n" -k C-p -k C-q || return 1
      "$HOLD_BIN" wait "$n" --timeout 8000 >/dev/null || { echo "the probe attach did not detach" >&2; return 1; }
      return 0
    fi
    sleep 0.2
  done
  echo "$1 cannot be attached: its console is still taken" >&2
  return 1
}

# A console is free again once its attach client is gone, however it went. The
# program here prints nothing more, so nothing it writes can reveal a dead
# client: the broker has to notice the client itself.
test_attach_again_after_client_vanishes() {
  "$HOLD_BIN" -dt --name quiet -- /bin/sh -c 'echo QUIET-UP; while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait quiet --match QUIET-UP --timeout 8000 || return 1
  # an attach on a terminal, killed along with it (a closed window)
  "$HOLD_BIN" -dt --name window -- "$HOLD_REAL_BIN" attach quiet </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait window --match QUIET-UP --timeout 8000 || { "$HOLD_BIN" peek window >&2; return 1; }
  "$HOLD_BIN" kill window >/dev/null 2>&1 || return 1
  console_free quiet QUIET-UP || { echo "a killed attach client kept the console" >&2; return 1; }
  # an attach whose input had already ended (it only reads), then killed
  "$HOLD_BIN" -d --name reader -- /bin/sh -c "exec \"$HOLD_REAL_BIN\" attach quiet </dev/null" </dev/null >/dev/null || return 1
  sleep 0.5
  "$HOLD_BIN" kill reader >/dev/null 2>&1 || return 1
  console_free quiet QUIET-UP || { echo "a killed read-only attach kept the console" >&2; return 1; }
  # and after a clean detach, as ever
  console_free quiet QUIET-UP || return 1
}

# An attach that cannot have the call says who has it, where a person will see
# it (not on a screen that is wiped as the attach gives up), and fails. Leaving
# a call says it is still running and how to return. Calls go by their names.
test_attach_messages_say_what_happened() {
  local rc
  "$HOLD_BIN" -dt --name app -- /bin/sh -c 'echo APP-UP; while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait app --match APP-UP --timeout 8000 || return 1
  # attached from inside another call
  "$HOLD_BIN" -dt --size 120 12 --name around -- /bin/sh </dev/null >/dev/null || return 1
  "$HOLD_BIN" send around "\"$HOLD_REAL_BIN\" attach app" -k Enter || return 1
  "$HOLD_BIN" wait around --match APP-UP --timeout 8000 || { "$HOLD_BIN" peek around >&2; return 1; }
  set +e
  "$HOLD_BIN" attach app </dev/null >"$TEST_ROOT/refused.out" 2>"$TEST_ROOT/refused.err"; rc=$?
  set -e
  [ "$rc" -eq 3 ] || { echo "a refused attach exited $rc" >&2; return 1; }
  grep -q 'callbox: app is already attached, from inside the call around (pid [0-9]*)' "$TEST_ROOT/refused.err" || { cat "$TEST_ROOT/refused.err" >&2; return 1; }
  [ ! -s "$TEST_ROOT/refused.out" ] || { echo "the refusal was written to stdout" >&2; cat "$TEST_ROOT/refused.out" >&2; return 1; }
  # on a terminal, the reason is still on screen after the attach has given up
  "$HOLD_BIN" -dt --size 120 6 --name second -- "$HOLD_REAL_BIN" attach app </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait second --timeout 8000 >/dev/null || return 1
  out_has 'app is already attached, from inside the call around' "$HOLD_BIN" peek second || { echo "a refused attach left nothing on the terminal" >&2; "$HOLD_BIN" peek second >&2; return 1; }
  # leaving says where things stand
  "$HOLD_BIN" send around -k C-p -k C-q || return 1
  "$HOLD_BIN" wait around --match 'callbox: detached from app; it is still running. Pick it back up: callbox attach app' --timeout 8000 || { echo "detaching said nothing" >&2; "$HOLD_BIN" peek around >&2; return 1; }
  # a call cannot be attached from inside itself
  "$HOLD_BIN" send around "\"$HOLD_REAL_BIN\" attach around; echo RC=\$?" -k Enter || return 1
  "$HOLD_BIN" wait around --match 'callbox: you are inside around already' --timeout 8000 || { "$HOLD_BIN" peek around >&2; return 1; }
  "$HOLD_BIN" wait around --regex '^RC=3$' --timeout 8000 || return 1
  # names, not 64 hex digits
  "$HOLD_BIN" end app 2>"$TEST_ROOT/end.err" || return 1
  grep -qx 'callbox: stopped app' "$TEST_ROOT/end.err" || { cat "$TEST_ROOT/end.err" >&2; return 1; }
  rc=0; "$HOLD_BIN" attach app 2>"$TEST_ROOT/ended.err" || rc=$?
  [ "$rc" -eq 8 ] || { echo "attach to an ended call exited $rc (want 8)" >&2; return 1; }
  grep -qx "callbox: app has ended - see 'callbox logs app'" "$TEST_ROOT/ended.err" || { cat "$TEST_ROOT/ended.err" >&2; return 1; }
  "$HOLD_BIN" kill around 2>"$TEST_ROOT/kill.err" || return 1
  grep -qx 'callbox: killed around' "$TEST_ROOT/kill.err" || { cat "$TEST_ROOT/kill.err" >&2; return 1; }
}

# Attached to one call from inside another, the detach keys let go of the
# innermost attach - the one on screen - and leave you in the call around it;
# pressed again they detach that one. An attach left running in a terminal
# nobody is attached to any more lets go by itself.
test_nested_attach_detaches_innermost_first() {
  local t launch
  "$HOLD_BIN" -dt --name inner -- /bin/sh -c 'echo INNER-UP; while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait inner --match INNER-UP --timeout 8000 || return 1
  # around it: a callbox shell, then a plain -it call
  for t in sh it; do
    if [ "$t" = sh ]; then launch="shell --name around-$t /bin/sh"; else launch="-it --name around-$t -- /bin/sh"; fi
    # shellcheck disable=SC2086
    "$HOLD_BIN" -dt --size 100 24 --name "term-$t" -- "$HOLD_REAL_BIN" $launch </dev/null >/dev/null || return 1
    wait_call "around-$t" || return 1
    "$HOLD_BIN" send "term-$t" 'echo READY' -k Enter || return 1
    "$HOLD_BIN" wait "term-$t" --regex '^READY$' --timeout 8000 || { "$HOLD_BIN" peek "term-$t" >&2; return 1; }
    "$HOLD_BIN" send "term-$t" "\"$HOLD_REAL_BIN\" attach inner" -k Enter || return 1
    "$HOLD_BIN" wait "term-$t" --match INNER-UP --timeout 8000 || { "$HOLD_BIN" peek "term-$t" >&2; return 1; }
    # the keys reach the inner attach: it detaches, the outer one stays
    "$HOLD_BIN" send "term-$t" -k C-p -k C-q || return 1
    wait_gone "term-$t" INNER-UP || { echo "($t) the inner attach did not let go" >&2; return 1; }
    out_has " term-$t\$" "$HOLD_BIN" list -l || { echo "($t) the detach keys detached the outer call instead" >&2; "$HOLD_BIN" list >&2; return 1; }
    "$HOLD_BIN" send "term-$t" 'echo BACK-HERE' -k Enter || return 1
    "$HOLD_BIN" wait "term-$t" --regex '^BACK-HERE$' --timeout 8000 || { echo "($t) not back in the outer call" >&2; "$HOLD_BIN" peek "term-$t" >&2; return 1; }
    console_free inner INNER-UP || { echo "($t) the inner call stayed attached" >&2; return 1; }
    # nothing nested now: the same keys detach the outer call, which runs on
    "$HOLD_BIN" send "term-$t" -k C-p -k C-q || return 1
    "$HOLD_BIN" wait "term-$t" --timeout 8000 >/dev/null || { echo "($t) the outer call did not detach" >&2; return 1; }
    out_has " around-$t\$" "$HOLD_BIN" list -l || { echo "($t) detaching ended the outer call" >&2; return 1; }
    # picked up again, attached inside again - and then its terminal is gone
    "$HOLD_BIN" -dt --size 100 24 --name "again-$t" -- "$HOLD_REAL_BIN" attach "around-$t" </dev/null >/dev/null || return 1
    "$HOLD_BIN" wait "again-$t" --match BACK-HERE --timeout 8000 || { "$HOLD_BIN" peek "again-$t" >&2; return 1; }
    "$HOLD_BIN" send "again-$t" "\"$HOLD_REAL_BIN\" attach inner" -k Enter || return 1
    "$HOLD_BIN" wait "again-$t" --match INNER-UP --timeout 8000 || { "$HOLD_BIN" peek "again-$t" >&2; return 1; }
    "$HOLD_BIN" kill "again-$t" >/dev/null 2>&1 || return 1
    console_free inner INNER-UP || { echo "($t) an attach buried in an unattended call kept the console" >&2; return 1; }
    out_has " around-$t\$" "$HOLD_BIN" list -l || { echo "($t) the outer call ended with its terminal" >&2; return 1; }
  done
}

# Detach leaves the shell and its chat running; attach picks both back up, and
# redial brings a shell back as a shell.
test_shell_detach_attach_and_redial() {
  start_shell_in_terminal 100 30 /bin/sh || return 1
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg work 'said before detaching' || return 1
  "$HOLD_BIN" send term -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term --match 'said before detaching' --timeout 8000 || return 1
  "$HOLD_BIN" send term 'a draft' || return 1
  "$HOLD_BIN" wait term --match '> a draft' --timeout 8000 || return 1
  "$HOLD_BIN" send term -k C-p -k C-q || return 1
  "$HOLD_BIN" wait term --timeout 8000 >/dev/null || { echo "detaching did not end the attach" >&2; return 1; }
  out_has ' work$' "$HOLD_BIN" list -l || { echo "detaching ended the shell" >&2; return 1; }
  env -u CALLBOX_ID -u CALLBOX_NAME "$HOLD_BIN" msg work 'said while detached' || return 1
  "$HOLD_BIN" -dt --size 90 28 --name term2 -- "$HOLD_REAL_BIN" attach work </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait term2 --match 'said while detached' --timeout 8000 || { "$HOLD_BIN" peek term2 >&2; return 1; }
  out_has 'said before detaching' "$HOLD_BIN" peek term2 || { echo "reattach lost the conversation" >&2; return 1; }
  out_has '> a draft' "$HOLD_BIN" peek term2 || { echo "reattach lost the draft" >&2; return 1; }
  out_has '"cols": 90,' "$HOLD_BIN" peek work --json || { echo "the shell did not follow the new terminal" >&2; return 1; }
  # The shell ends; its name redials it, as a shell with chat again.
  "$HOLD_BIN" send term2 -k Escape || return 1
  wait_gone term2 'said while detached' || return 1
  "$HOLD_BIN" send term2 'exit' -k Enter || return 1
  "$HOLD_BIN" wait work --timeout 8000 >/dev/null || { echo "the shell did not exit" >&2; return 1; }
  "$HOLD_BIN" -dt --size 80 24 --name term3 -- "$HOLD_REAL_BIN" work </dev/null >/dev/null || return 1
  "$HOLD_BIN" wait term3 --match 'callbox shell | Ctrl-P Ctrl-T: chat' --timeout 8000 || { echo "redial did not bring the shell back with chat" >&2; "$HOLD_BIN" peek term3 >&2; return 1; }
  "$HOLD_BIN" send term3 -k C-p -k C-t || return 1
  "$HOLD_BIN" wait term3 --match 'chat' --timeout 8000 || return 1
}

# peek --image writes a PNG; without ruler or footer at scale 1 it is exactly
# 8x16 pixels per cell.
test_peek_image_png() {
  "$HOLD_BIN" -dt --name pic -- /bin/sh -c 'printf "\033[44m box \033[0m \342\224\214\342\224\200\342\224\220\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait pic --match box --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek pic --image "$W/full.png" --cursor-ring || return 1
  "$HOLD_BIN" peek pic --image "$W/bare.png" --scale 1 --no-ruler --no-footer || return 1
  python3 - "$W/full.png" "$W/bare.png" <<'PY' || return 1
import struct, sys, zlib
def ihdr(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    w, h = struct.unpack(">II", data[16:24])
    pos, idat = 8, b""
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4]); kind = data[pos + 4:pos + 8]
        if kind == b"IDAT": idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    assert len(raw) == h * (w * 3 + 1), (len(raw), w, h)
    return w, h
assert ihdr(sys.argv[2]) == (640, 384), ihdr(sys.argv[2])
w, h = ihdr(sys.argv[1])
assert w > 1280 and h > 768, (w, h)
PY
  "$HOLD_BIN" end pic >/dev/null 2>&1 || true
}

# After the call ends, peek shows its last screen; purge removes it.
test_peek_after_exit_and_purge() {
  "$HOLD_BIN" -dt --name gone -- /bin/sh -c 'echo last words; sleep 0.3' >/dev/null || return 1
  "$HOLD_BIN" wait gone --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" peek gone | grep -qx 'last words' || return 1
  "$HOLD_BIN" peek gone --json | grep -q '"running": false' || return 1
  local id
  id=$("$HOLD_BIN" inspect gone | sed -n 's/^  "id": "\(.*\)",$/\1/p')
  [ -f "$HOME/.local/state/callbox/$id.screen" ] || return 1
  "$HOLD_BIN" purge gone >/dev/null 2>&1 || return 1
  [ ! -e "$HOME/.local/state/callbox/$id.screen" ] || return 1
}

# Calls without a TTY have no screen; the verbs say so.
test_screen_verbs_need_a_tty() {
  "$HOLD_BIN" -d --name notty -- /bin/sh -c 'sleep 30' >/dev/null || return 1
  set +e
  "$HOLD_BIN" peek notty 2>"$TEST_ROOT/notty.err"; rc=$?
  set -e
  [ "$rc" -eq 10 ] && grep -q 'no terminal screen' "$TEST_ROOT/notty.err" || { echo "rc=$rc (want 10)" >&2; cat "$TEST_ROOT/notty.err" >&2; return 1; }
  "$HOLD_BIN" end notty >/dev/null 2>&1 || true
}

# A mouse event to a program with mouse reporting off is reported, not lost silently.
test_send_mouse_off_is_reported() {
  "$HOLD_BIN" -dt --name nomouse -- /bin/sh -c 'echo ready; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait nomouse --match ready --timeout 5000 || return 1
  set +e; "$HOLD_BIN" send nomouse --mouse click,3,1 2>"$TEST_ROOT/mouse.err"; rc=$?; set -e
  [ "$rc" -eq 9 ] || { echo "mouse-off send rc $rc, want 9" >&2; return 1; }
  grep -q 'mouse reporting off' "$TEST_ROOT/mouse.err" || return 1
  "$HOLD_BIN" peek nomouse --json | grep -q '"last_mouse": {"row": 1, "col": 3}' || return 1
  "$HOLD_BIN" end nomouse >/dev/null 2>&1 || true
}

# Every call knows its own address; a call launched from inside another
# learns its parent's.
test_call_identity_environment() {
  # Launched from outside any call (the suite itself may run inside one).
  out=$(env -u CALLBOX_NAME -u CALLBOX_ID -u CALLBOX_PARENT "$HOLD_BIN" -d --name selfaware -- /bin/sh -c 'echo "name=$CALLBOX_NAME id=$CALLBOX_ID parent=${CALLBOX_PARENT:-none}"') || return 1
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  "$HOLD_BIN" wait selfaware --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs selfaware | grep -qx "name=selfaware id=$id parent=none" || { "$HOLD_BIN" logs selfaware >&2; return 1; }
  CALLBOX_NAME=lead "$HOLD_BIN" -d --name kid -- /bin/sh -c 'echo "parent=$CALLBOX_PARENT name=$CALLBOX_NAME"' >/dev/null || return 1
  "$HOLD_BIN" wait kid --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs kid | grep -qx 'parent=lead name=kid' || { "$HOLD_BIN" logs kid >&2; return 1; }
}

# Attaching repaints the live screen rather than replaying bytes.
test_attach_repaints_live_screen() {
  "$HOLD_BIN" -dt --name painted -- /bin/sh -c 'printf "first\nsecond\033[1;1Hover"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait painted --match over --timeout 5000 || return 1
  ( sleep 1.5; "$HOLD_BIN" end painted >/dev/null 2>&1 ) &
  "$HOLD_BIN" attach painted </dev/null >"$TEST_ROOT/attach.out" 2>&1 || true
  wait
  grep -a -q 'overt' "$TEST_ROOT/attach.out" || { od -c "$TEST_ROOT/attach.out" | head -20 >&2; return 1; }
  "$HOLD_BIN" end painted >/dev/null 2>&1 || true
}

run_test "--print spans --all and multiple explicit targets" test_print_over_all_and_multiple
run_test "stop refuses a record with a tampered pgid<=1 (exit 5)" test_signal_refuses_tampered_pgid
run_test "public-index write failure rolls back the start" test_public_index_write_rollback
run_test "msg: a claim cannot forge a .msgs line or crash the broker" test_msg_claim_cannot_forge_or_crash
run_test "peek --image refuses a framebuffer too large to render" test_peek_image_refuses_oversize
run_test "send/wait/msg keep literal --x=y text" test_screen_verbs_keep_literal_eq_text
run_test "msg strips terminal control characters" test_msg_strips_control_characters
run_test "argv and env over 4 KiB survive exit and redial" test_long_argv_and_env_survive
run_test "redialing a -dt call stays detached" test_dt_redial_stays_detached
run_test "--tail CALL follows its log, never redials" test_tail_flag_follows_a_calls_log
run_test "--quiet prints bare id and silences stderr" test_quiet_suppresses_banner_keeps_id
run_test "run id prefix resolves to the full run" test_run_id_prefix_resolution
run_test "action/help/list argument guards" test_misc_action_guards
run_test "owned commands reject extra targets and unsupported --all" test_owned_command_exact_arity
run_test "system store directory modes are private (0700/0755)" test_system_store_directory_modes
run_test "system store tightens a pre-existing loose dir" test_system_store_tightens_preexisting_loose_dir
run_test "system store refuses symlinked critical dirs without chmod-following" test_system_store_refuses_symlinked_critical_dirs
run_test "system store artifacts are owned by root:root" test_system_store_artifacts_owned_by_root
run_test "non-root process ignores spoofed SUDO_* provenance" test_nonroot_ignores_spoofed_sudo_provenance
run_test "start/stop lifecycle" test_lifecycle
run_test "kill subcommand kills process group" test_kill_subcommand
run_test "stop kills full process group (children)" test_group_kill_children
run_test "exec failure creates no record" test_exec_failure_no_record
run_test "fast exit command is recorded as exited" test_fast_exit_record_exited
run_test "exec replacement remains controllable" test_exec_replacement_remains_controllable
run_test "corrupt record warning and prune cleanup" test_corrupt_record_handling
run_test "deeply nested/corrupt JSON record is rejected, not crashed" test_deep_json_record_rejected_not_crashed
run_test "symlinked record is rejected" test_symlinked_record_rejected
run_test "symlinked log is rejected" test_symlinked_log_rejected
run_test "invalid pgid=0 record is not listed as running" test_invalid_pgid_record
run_test "unreferenced logs are removed by prune" test_orphan_log_cleanup
run_test "ID input sanitization rejects invalid ids" test_id_sanitization
run_test "stop/kill --print emits group signal command" test_print_signal_output
run_test "signal refuses tampered live process-group identity" test_signal_refuses_tampered_live_group_identity
run_test "stop supports multiple IDs in one command" test_stop_multiple_ids
run_test "argument edge cases" test_argument_edges
run_test "Docker-shaped run/logs/ps/rm surface" test_docker_shaped_cli_flags_and_rm
run_test "ps table humanizes CREATED, phrases STATUS, and marks saved" test_ps_table_humanized_status_and_saved
run_test "ps table content-sized columns do not shear" test_ps_table_columns_do_not_shear
run_test "ports lists a call's bound listeners" test_ports_lists_bound_listener
run_test "stats single-shot prints the documented columns" test_stats_single_shot_shape
run_test "inspect reports live stdio fd targets" test_inspect_reports_stdio_targets
run_test "bare foreground streams output; -d detaches with a 64-hex id" test_docker_bare_launch_foreground_follows_output_by_default
run_test "Docker run foreground follows output by default" test_docker_run_foreground_follows_output_by_default
run_test "unsupported Docker-shaped options fail loudly" test_docker_unsupported_options_fail_loudly
run_test "Docker restart policy restarts failed processes" test_docker_restart_policy_restarts_failures
run_test "purge never follows stored paths" test_purge_never_follows_stored_paths
run_test "usage errors go to stderr; help names both views" test_usage_errors_go_to_stderr_and_help_names_both_views
run_test "crash-leftover temp does not block record rewrites" test_crash_leftover_tmp_does_not_block_rewrites
run_test "purge sweep respects the creation reserve" test_purge_sweep_respects_creation_reserve
run_test "logs --plain dumps the whole log; -n/--tail is the last N" test_logs_plain_dumps_whole_log_and_tail_is_last_n
run_test "foreground launch propagates the exit code" test_docker_foreground_propagates_exit_code
run_test "restart call records its final exit status" test_restart_final_status_recorded
run_test "ending a restart call records the term status" test_end_restart_call_records_term_status
run_test "unwitnessed death renders Exited (?)" test_unwitnessed_death_renders_exited_unknown
run_test "Docker restart validates policies and tty gate" test_docker_restart_validation_and_tty_gate
run_test "start existing run appends retained log" test_start_existing_run_appends_retained_log
run_test "Docker publish and volume flags are rejected" test_docker_publish_and_volume_rejected
run_test "Docker -i keeps non-PTY stdin open" test_docker_interactive_stdin_pipes_to_child
run_test "Docker -it foreground attaches and detaches" test_docker_tty_foreground_attaches_and_detaches
run_test "Docker --detach-keys changes TTY detach sequence" test_docker_tty_custom_detach_keys
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "logs -n prints the newest lines; -f drains to the end" test_logs_tail_and_follow
run_test "-f starts and follows output" test_start_follow_short_form
run_test "tail <id> tails an existing run log" test_tail_verb_existing_id
run_test "persistent stale records remain visible and dumpable" test_persistent_stale_records
run_test "missing boot source does not force stale" test_boot_unavailable_does_not_force_stale
run_test "leader zombie with live group remains running" test_leader_zombie_group_still_running
run_test "tail <id> prints finished log output" test_tail_finished_log_prints_existing_output
run_test "--console works without an external attach tool" test_console_does_not_require_external_attach_tool
run_test "console reports a normal run has no console" test_console_reports_non_console_run
run_test "console attach round-trips and tees to the log" test_console_round_trip_and_log_tee
run_test "console exit code is recorded" test_console_exit_code_is_recorded
run_test "end on a console call signals the held group, not the broker" test_end_console_call_signals_target
run_test "end on a console call escalates TERM to KILL" test_end_console_call_escalates_to_kill
run_test "SIGTERM to the console broker forwards to the held group" test_console_broker_term_forwards_to_target
run_test "console rejects unrelated peer UID before replay" test_console_rejects_unrelated_peer_uid_before_replay
run_test "console can reattach after detach" test_console_can_reattach_after_detach
run_test "console socket lives in store dir, not /tmp, for long paths" test_console_socket_lives_in_store_dir
run_test "console target runs in caller cwd (relative-bind restores cwd)" test_console_target_runs_in_caller_cwd
run_test "prune <id> removes exactly one run record/output" test_prune_by_id
run_test "prune all removes prunable while preserving running" test_prune_all_keeps_running
run_test "save marks the record and is idempotent" test_save_marks_record_and_is_idempotent
run_test "sweeping purge skips saved calls" test_purge_sweep_skips_saved
run_test "targeted purge of a saved call refuses with exact wording" test_purge_targeted_refuses_saved_exact_wording
run_test "purge --force removes a saved ended call" test_purge_force_removes_saved_ended
run_test "purge --force removes a live saved call" test_purge_force_removes_live_saved
run_test "rename of a saved call keeps purge protection" test_rename_of_saved_call_keeps_protection
run_test "rename saves an unsaved call" test_rename_saves_an_unsaved_call
run_test "redial honors a recorded foreground recipe (and -d overrides)" test_redial_honors_recorded_foreground_mode
run_test "redial honors a recorded detached recipe" test_redial_honors_recorded_detached_mode
run_test "redial honors a recorded console recipe" test_redial_honors_recorded_console_mode
run_test "transactional launch rollback on record write failure" test_transactional_record_write_failure
run_test "raw start does not steal trailing --system" test_raw_start_does_not_steal_trailing_system
run_test "long command appears in list, truncated with ..." test_long_command_list_truncates_instead_of_skips
run_test "normal start writes user-local state" test_normal_start_writes_user_local_state
run_test "hand-authored native record is readable; legacy keys ignored" test_native_record_is_readable_when_hand_authored
run_test "root starts use system store and public state is projected" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo --system home executable uses invoking-user store" test_sudo_system_start_of_home_executable_uses_user_store
run_test "sudo --system home argv paths use invoking-user store" test_sudo_system_start_of_home_argv_paths_uses_user_store
run_test "sudo TTY launch from home: every file stays the user's" test_sudo_home_tty_call_files_stay_users
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "public root index list reads projected State" test_public_root_index_list_reads_projected_state
run_test "list is the ledger with a USER column; -l/--live narrows to running" test_list_default_is_full_ledger_live_narrows
run_test "list <name> filters the ledger by the call's name" test_list_name_filter_matches_call_name
run_test "list -a renders the projected global ports" test_list_all_renders_global_ports_projection
run_test "list -s/--system shows the redacted global view only" test_system_list_is_redacted_global_only
run_test "ps is Docker's machine-wide running view" test_ps_is_docker_machine_wide_running_view
run_test "purge -s re-execs the global sweep through sudo" test_purge_system_reexecs_through_sudo
run_test "root list -a labels owners and walks user homes" test_root_list_all_labels_owner_and_walks_homes
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "deleted capability flags are rejected honestly" test_cap_flags_are_rejected
run_test "Docker --rm removes run artifacts after exit" test_docker_rm_removes_run_artifacts_after_exit
run_test "--rm: kill/end remove before returning; wait reports the code" test_rm_removed_on_kill_end_and_wait_reports_code
run_test "a call survives its caller's process group being killed" test_call_survives_caller_group_kill
run_test "env flags are recipe data, not callbox's own environment" test_env_flag_is_recipe_data_only
run_test "tail Ctrl-C detaches from tail and does not stop run" test_tail_ctrl_c_detaches_from_tail_and_keeps_run
run_test "foreground: Ctrl-C goes to the program, Ctrl-P Ctrl-Q detaches" test_foreground_keys_interrupt_or_detach
run_test "foreground without a terminal: SIGINT goes to the program" test_foreground_sigint_reaches_the_program
run_test "a bare launch runs attached on a PTY, streamed to a script" test_bare_launch_runs_on_a_pty
run_test "macOS boot ids: the UUID is exact; older records match by boot time" test_macos_boot_ids_uuid_and_older_records
run_test "user mode: messages go to chat, redial comes back in user mode" test_user_mode_messages_go_to_chat
run_test "-it has no chat window; its messages are typed" test_it_launch_messages_are_typed
run_test "-i: piped input streams in and ends; no PTY" test_pipe_input_streams_and_ends
run_test "-i at a terminal: line editing, detach, attach, Ctrl-D, Ctrl-C" test_pipe_input_keys_detach_and_attach
run_test "-di: attach later types into its input" test_pipe_input_detached_then_attached
run_test "-i: a program that does not read never blocks its broker" test_pipe_input_never_blocks_the_broker
run_test "-i: early close of stdin keeps the exit code and later output" test_pipe_input_early_close_keeps_exit_code
run_test "-i: msg and send --enter complete a line on pipes" test_pipe_input_enter_is_a_newline
run_test "closed or empty stdin: no self-feeding, end of input reaches the program" test_closed_or_empty_stdin_ends_input
run_test "SIGINT to an attached client reaches the program" test_sigint_to_attached_client_reaches_program
run_test "the exit stamp beats a leftover background child" test_exit_stamp_beats_a_leftover_child
run_test "a fast redial prints its own run once; the record is that run's" test_fast_redial_prints_its_own_run
run_test "callbox foo | head ends with 141 and says foo keeps running" test_closed_reader_ends_with_141
run_test "SIGTERM to a launch reaches its program" test_sigterm_to_a_launch_reaches_program
run_test "a piped attach leaves a -di call's input open" test_piped_attach_keeps_di_input_open
run_test "wait --match spans a soft wrap" test_wait_match_spans_a_soft_wrap
run_test "callbox -w cmd | head ends quietly with 141" test_follow_closed_reader_ends_with_141
run_test "a foreground redial shows its own run only" test_foreground_redial_shows_its_run_only
run_test "a stream client's program gets replies to terminal queries" test_stream_client_gets_query_replies
run_test "purges at once each report only what they removed" test_concurrent_purges_count_once
run_test "an ambiguous id prefix lists the calls and exits 6, for every verb" test_ambiguous_id_prefix_lists_and_exits_6
run_test "the start of a name is a target when one call fits" test_name_prefix_targets
run_test "shortened verbs and call names; PATH still wins" test_shortened_top_level_words
run_test "redialing a running call attaches or follows, never starts it twice" test_redial_running_call_goes_back_to_it
run_test "PATH and --restart report a program that cannot run as 126" test_cannot_start_126_by_path_and_restart
run_test "redial keeps the call's saved flag and name" test_redial_keeps_saved_and_name
run_test "concurrent redials of one call start it once" test_concurrent_redials_start_once
run_test "purge leaves a call alone while a redial launches it" test_purge_stands_back_from_a_redial
run_test "end, stop and kill of an ended call succeed and say so" test_end_on_ended_call_is_success
run_test "send --wait says the input was sent when the wait fails" test_send_wait_says_input_was_sent
run_test "a command that cannot start exits 127 or 126" test_cannot_start_exits_127_and_126
run_test "call names: verbs refused, a taken name exits 11" test_call_names_verbs_and_taken
run_test "attach exits 8 when the call ended, 10 without a console" test_attach_exit_codes
run_test "msg prints the delivered message's id" test_msg_prints_its_id
run_test "end acts on each target; the worst code is returned" test_end_acts_on_each_target
run_test "restarts back off without --restart-delay" test_restart_backs_off_by_default
run_test "build artifacts for static and dynamic coexist" test_build_artifact_coexistence
run_test "concurrent starts produce unique ids" test_concurrent_unique_ids
run_test "--env-file populates the recipe env only" test_env_file_populates_recipe_env
run_test "end on a plain call escalates TERM to KILL" test_end_plain_call_escalates_to_kill
run_test "end signals the recorded group after the leader is gone" test_signal_delivers_to_group_after_leader_exit
run_test "attach is the primary console spelling" test_attach_verb_matches_console
# DIVERGENCE: identity doc promises `logs -p` -t/--time and --date timestamp
# flags, but the logs parser (src/runtime/signal.c hold_cmd_view_action) knows
# neither: `callbox logs <id> -p -t` exits 5 with the usage line. Re-register
# the test once the flags exist.
run_test "logs to a non-TTY dumps plain raw bytes automatically" test_logs_non_tty_auto_plain
run_test "list -u shows only the invoking user's calls, even under sudo" test_list_user_scope_only_personal
run_test "drop removes a call exactly like prune" test_drop_alias_matches_prune
run_test "root's plain -d is seen live by other users" test_global_plain_d_seen_live_by_users
run_test "sudo purge sweeps an orphan public entry" test_orphan_public_entry_swept
run_test "unless-stopped restarts crashes but stays down after end" test_restart_unless_stopped
run_test "peek prints the screen as text, with a ruler, and as JSON" test_peek_text_ruler_and_json
run_test "send types text and keys; --wait settles after the answer" test_send_text_keys_and_wait
run_test "send encodes arrows for the program's cursor-key mode" test_send_keys_follow_cursor_key_mode
run_test "send --paste brackets only when the program asked" test_send_paste_brackets_when_requested
run_test "wait times out (7), sees early exit (8), and prints exit codes" test_wait_timeout_ended_and_exit_code
run_test "wait --idle settles after output stops" test_wait_idle_settles
run_test "wait: bare --idle means 500 ms" test_wait_bare_idle
run_test "--help after the target prints the full help" test_help_after_target
run_test "send/resize/msg on an ended call say it ended" test_screen_verbs_on_ended_call
run_test "call names may not start with a dash" test_name_rejects_leading_dash
run_test "send: too much text fails with a clear message" test_send_too_large_says_so
run_test "wait: more clients than broker slots still get answers" test_wait_more_clients_than_slots
run_test "resize delivers SIGWINCH and updates the screen" test_resize_delivers_sigwinch
run_test "peek --image writes a well-formed PNG" test_peek_image_png
run_test "--size is the terminal the program first sees; redial replays it" test_size_flag_sets_first_size
run_test "initial size: --size, saved detach-size, invoking terminal, 80x24" test_detach_size_default_and_precedence
run_test "an invalid size starts nothing and replaces nothing" test_invalid_sizes_start_nothing
run_test "callbox shell: options end at the shell's name; never detached or headless" test_shell_launch_grammar
run_test "callbox shell: a chat request, a callbox msg reply, unread while hidden" test_shell_chat_request_and_reply
run_test "callbox shell: failed sends say so; outside senders get their own conversation" test_shell_chat_failures_and_outside_senders
run_test "callbox shell: the window over a live program uncovers current cells" test_shell_overlay_over_live_program
run_test "callbox shell: a window resize reaches the program, the chat window's does not" test_shell_resize_reaches_program_overlay_does_not
run_test "callbox shell: chat keystrokes and drags never reach the program" test_shell_chat_input_never_reaches_program
run_test "the lead key waits for the next key and shows what it offers" test_lead_key_waits_and_offers
run_test "callbox -it exits as its command did and leaves its last screen" test_tty_foreground_exit_status_and_last_screen
run_test "however an attach ends, the last screen stays and callbox says how" test_attach_endings_leave_the_last_screen
run_test "a mistyped command line is a usage error and acts on nothing" test_cli_mistakes_are_usage_errors
run_test "every help page fits 80 columns; help launch lists the flags" test_help_pages_fit_and_say_it
run_test "callbox shell: the draft takes any script and whole pastes" test_shell_chat_draft_editing
run_test "callbox shell: detach, attach and redial keep the shell a shell" test_shell_detach_attach_and_redial
run_test "a console is free again once its attach client is gone" test_attach_again_after_client_vanishes
run_test "nested attach: the detach keys let go of the innermost first" test_nested_attach_detaches_innermost_first
run_test "attach says who has a call, where you stand after leaving, and uses names" test_attach_messages_say_what_happened
run_test "peek shows the last screen after exit; purge removes it" test_peek_after_exit_and_purge
run_test "screen verbs refuse calls without a TTY" test_screen_verbs_need_a_tty
run_test "send --mouse to a program without mouse reporting exits 9" test_send_mouse_off_is_reported
run_test "calls see CALLBOX_ID, CALLBOX_NAME, CALLBOX_PARENT" test_call_identity_environment
run_test "attach repaints the live screen" test_attach_repaints_live_screen
run_test "msg delivers between calls and names the sender" test_msg_between_calls
run_test "TTY calls default TERM when the launcher has none" test_tty_call_defaults_term
run_test "arrow keys and mouse move a real program's cursor" test_cursor_control_keys_and_mouse
run_test "keys arrive as exact xterm bytes, one write each" test_keys_exact_bytes_one_write_each
run_test "a whiptail form is driven by keyboard alone" test_whiptail_form_by_keyboard
run_test "send reports a clamped legacy mouse event" test_send_mouse_legacy_clamp_is_reported
run_test "tmux: prefix split, mouse pane focus, detach" test_tmux_prefix_mouse_and_detach
run_test "an ncurses program takes mouse clicks" test_curses_mouse_checklist
run_test "micro: click, double-click, drag select, wheel" test_micro_mouse_editing
run_test "wait --idle needs first output; symlinked programs keep their name" test_wait_idle_needs_first_output_and_symlink_launch
run_test "wait --match --since ignores a stale screen" test_wait_match_since_ignores_stale_screen
run_test "peek --image draws CJK and emoji glyphs" test_peek_image_draws_cjk_and_emoji
run_test "peek --image draws Hebrew, Ethiopic, sextants, math letters" test_peek_image_draws_other_scripts
run_test "peek --image: Noto emoji, and invisible characters draw nothing" test_peek_image_emoji_and_invisibles
run_test "peek --image draws rounded corners as arcs" test_peek_image_rounded_corner
run_test "peek --image wraps the footer on narrow screens" test_peek_image_footer_wraps_on_narrow_screens
run_test "inspect reports the observed state, keeping the recorded one" test_inspect_reports_observed_state
run_test "plain -d leaves nothing behind; -dw keeps a waiter" test_plain_d_leaves_nothing_behind_dw_waits
run_test "another PID namespace: status and control go through the supervisor" test_other_pid_namespace_follows_chain_of_command
run_test "the FIFO console keeps full capability" test_fifo_console_full_capability
run_test "a closed terminal is reported at once, in both console modes" test_console_closed_terminal_is_reported
run_test "FIFO console: root drives a user's call; others get nothing" test_fifo_console_access
run_test "msg delivers at once to a receiver that never goes quiet" test_msg_delivers_at_once_to_a_busy_receiver
run_test "peek --last-alt keeps a full-screen program's last frame" test_peek_last_alt_frame

echo "----------------------------------------------------------------"
echo "summary: $PASSES passed, $FAILS failed, $SKIPS skipped"
if [ "$SKIPS" -ne 0 ]; then
  echo "  (some tests skipped; run as root and/or set HOLD_REQUIRE_ROOT_TESTS=1 for full coverage)"
fi
if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
