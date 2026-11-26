# Backmux — implementation invariants

[Docs index](index.md) | [Identity and surface contract](backmux-identity.md) | [Docker familiarity contract](docker-familiarity-contract.md)

The current command model is summarized in [backmux-identity.md](backmux-identity.md)
and Docker-style syntax in
[docker-familiarity-contract.md](docker-familiarity-contract.md). Mechanism references:
[launcher](launcher.md), [store](store.md), [identity](identity.md),
[console](console.md). This document records only the on-disk invariants that
every one of those relies on.

## Storage contexts

Two stores, one shape:

```text
user-local:    ~/.local/state/backmux          (0700, owned by the user)
root-managed:  /var/lib/backmux  (Linux)       /var/db/backmux  (macOS)
```

The user-local store is 0700 because it holds command lines, environment,
and output. Its mode restricts ordinary access to its owner; privileged
processes can also read it. When root creates it under sudo, it
is chowned back to the invoking user. Root and sudo launches normally use the system store. If the program or
an existing path argument is under the invoking user's home, launch routing
selects that user's store instead. A normal user cannot launch into the
system store.

User-local layout, directly under the base:

- process records (`<64hex>.json`, 0600) and logs (`<64hex>.log`, 0600);
- beside each log of a terminal process: its last screen (`<64hex>.screen`) and its
  last full-screen frame (`<64hex>.altscreen`), both HSNAP1 (see
  [screen.md](screen.md)), and its delivered-message log (`<64hex>.msgs`);
- transient dot-files: `.<id>.reserve` while a process is being created, and
  `.<id>.exit.tmp` after an `--rm` process ends, so `backmux wait` can
  report its exit code; a later sweep removes notes older than one minute;
- console endpoints under `console/` (0700): `<id>.sock`, a 0600 Unix
  socket, or a 0600 FIFO where Unix sockets are unavailable;
- outside-sender inbox state under `inbox/` and conversation logs and
  compositor coordination files under `chat/`.

Root-managed layout: the base is 0755. `runs/`, `logs/`, and `console/` are
0700 root-only, keeping private command lines, environment, and invocation
metadata out of the public projection. The `.screen`, `.altscreen`, and `.msgs` files sit beside the log in
`logs/`. `public/` is 0755, with one 0644 redacted projection per global
process, so every user can list global processes without reading their records.

## The public projection

A global process's public entry carries only what every user may see: `id`,
`root_managed`, `name` (when set), `state_hint`, `started_at`, `created_at`,
`ended_at` and `exit_code` once it has ended, `pid`, `pgid`, `sid`,
`running`, and `observed_ports` (listening TCP and bound UDP only, as root
last observed them; never outbound connections, so a process's peers cannot
leak). The invoking user, argv, and environment are never written;
user-facing views render the user and command as `hidden`. An entry whose
private record is gone is an orphan; `sudo backmux purge --system` (or
`backmux purge -s`) sweeps it, so no view can bring the process back.

## Records

One JSON record per process in a single native snake_case schema, written
atomically (temp file, fsync, rename) and never partially visible. Docker
parity lives at the CLI surface (flags and tables), not in storage.

A record contains the launch settings used by recall: executable and
arguments, explicit environment assignments, working directory, launch mode,
explicit terminal size, restart policy, and message formatting. Recall
reuses the Backmux process ID and name and appends to the same log.
`backmux recall NAME` rejects a process confirmed to be running. The bare
target shorthand
reconnects to a running console or follows a running server's log; explicit
`-d` prints its ID without attaching.

Current mode fields include `interactive`, `tty`, `detach`, `wait`, and
`shell`. `agent` and `working` are top-level fields. The reader and writer
also retain `mode.chat`, `lead_key`, `detach_key`, and `chat_key` for the
older broker-chat path. Current launch commands do not enable that path;
chat is drawn by the attach-client compositor. A console-backed record
identifies its broker through `broker_pid`. `saved: true` protects the
record from ordinary purge sweeps.

A target matching several records is reported as ambiguous (exit 6).
Names, ID prefixes, command abbreviations, and bare program names have
distinct resolution rules; see `backmux help targets`.

IDs are 64 hex: the SHA-256 of the executable, cwd, start time in
nanoseconds, launcher pid, argv, and a collision counter, reserved with an
exclusive `.<id>.reserve` file. An id is unique within its store and
against the global public index. Tables show the first 12 hex digits. Names
are generated `adjective_noun`, unique per store because a name is a target
and must resolve to one process, and user-renameable (renaming saves).

## Logs

Raw captured bytes, appended as they arrive: stdout and stderr of a plain
process (one append-mode file for stdout and stderr),
the PTY stream of a terminal process, or the broker-captured output of a
pipe-input process. A plain process writes directly to its log; a
console-backed process's broker copies its output. One plain file, no index; anything can read it without Backmux's help.
`backmux logs` prints it whole, from the newest N lines (`-n`), or followed
to the process's end (`-f`).

## Safety invariants

- Every managed process gets its own process group and session.
- Local group signaling requires revalidation: recorded and current boot
  IDs must agree when both are available;
  a live leader must still have its recorded start time (or, when none was
  recorded, its executable's device and inode) and its recorded pgid and
  sid; a record with no identity token is never signaled. When the leader
  is gone, the group is matched by its recorded pgid+sid within the same
  boot — a deliberate best effort, since no atomic verify-and-signal exists
  for a process group. A recycled PID is never knowingly signaled.
- A process with a console (`-t` or `-i`) in another PID namespace is signaled
  through its broker, which can see its program. This also works when local
  status is `Unknown`; an unavailable PID or log-lock probe does not prove
  exit. A plain `-d` or `-dw` process there has no broker to ask and is refused
  (exit 3). An unreachable broker is reported as an error, not a stopped process.
- `end` sends TERM, waits 5 seconds, revalidates, then sends KILL.
- Records are trusted only after validation. `backmux list` reports a
  corrupt record; a purge sweep deletes it. It is never obeyed.
- Purge removes only store-resident artifacts derived from the process id;
  paths stored in records are never followed for deletion, so a tampered
  record cannot aim purge at a file outside the store.
- stdout carries command results and help; status notes and errors go to
  stderr; `--quiet` suppresses ordinary status notes.
- Backmux never elevates on a user's behalf, with one deliberate exception:
  a no-target `backmux purge --system` run by a normal user re-executes
  itself through `sudo` (which does the prompting) at a single commented
  dispatch site. A targeted purge of a global process is never re-executed: a
  normal user is told it needs root.

## Non-goals

No central daemon: live managed processes can have a waiter, restart
supervisor, or console broker. Interactive attach clients and their
compositors exist while in use. No root-log visibility for
normal users. Backmux process IDs are not reserved across all users' personal stores. No
scanning of other users' private stores by anyone but root. Tests never
touch the real system store or real homes: `make test` compiles with
`HOLD_TESTING`, which honors `HOLD_TEST_SYSTEM_STATE_DIR`,
`HOLD_TEST_HOME_ROOT`, and `HOLD_BOOT_ID_PATH`.
