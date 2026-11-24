# Backmux — implementation invariants

[Docs index](index.md) | [Identity and surface contract](backmux-identity.md) | [Docker familiarity contract](docker-familiarity-contract.md)

The command surface is specified by [backmux-identity.md](backmux-identity.md)
and the borrowed Docker behaviors by
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
and output: only its owner may read it. When root creates it under sudo, it
is chowned back to the invoking user. A process placed as root, or under sudo,
goes in the root-managed store; a normal user cannot place one there.

User-local layout, directly under the base:

- process records (`<64hex>.json`, 0600) and logs (`<64hex>.log`, 0600);
- beside each log of a TTY process: its last screen (`<64hex>.screen`) and its
  last full-screen frame (`<64hex>.altscreen`), both HSNAP1 (see
  [screen.md](screen.md)), and its delivered-message log (`<64hex>.msgs`);
- transient dot-files: `.<id>.reserve` while a process is being created, and
  `.<id>.exit.tmp` for a minute after an `--rm` process ends, so `backmux wait`
  can still report its exit code;
- console endpoints under `console/` (0700): `<id>.sock`, a 0600 Unix
  socket, or a 0600 FIFO where the environment denies Unix sockets.

Root-managed layout: the base is 0755. `runs/`, `logs/`, and `console/` are
0700 root-only, so command lines, environment, and ownership never leave
them. The `.screen`, `.altscreen`, and `.msgs` files sit beside the log in
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

The record is the recipe. Redial reuses the process's id, name, and log (new
output appends), and replays its argv, environment, session mode (`mode{}`:
`interactive`, `tty`, `detach`, `wait`, `chat`; `chat` marks a `backmux
shell`), the `--size` it was placed with (`size_cols`, `size_rows`), and
`--restart` / `--restart-delay`. Mode flags given on the redial override the
recipe. A running process is not redialed: naming it gets you back to it
(attach when it has a console, else follow its log; with `-d`, print its id,
exit 0), because there is nothing to start. A target that matches more than
one process is listed, exit 6, never guessed; the start of a name is a target
when exactly one process fits, and a top-level word that is no verb, process, or
program may be the start of one verb or name (see `backmux help targets`).

A `backmux shell`'s record carries `lead_key`, `detach_key`, and `chat_key`
(control bytes), which its broker reads itself. When `mode.chat` and these
keys are present, `backmux attach` passes every key through instead of
reading the detach keys. A TTY process's record also names its console broker
(`broker_pid`). `saved: true` protects it from sweeps.

IDs are 64 hex: the SHA-256 of the executable, cwd, start time in
nanoseconds, launcher pid, argv, and a collision counter, reserved with an
exclusive `.<id>.reserve` file. An id is unique within its store and
against the global public index. Tables show the first 12 hex digits. Names
are generated `adjective_noun`, unique per store because a name is a target
and must resolve to one process, and user-renameable (renaming saves).

## Logs

Raw captured bytes, appended as they arrive: stdout and stderr of a plain
call (one append-mode file both go to, so they interleave in arrival order),
or the PTY stream of a TTY process. A plain process's program writes its log
directly, with no copying process in between; a TTY process's broker writes
it. One plain file, no index; anything can read it without Backmux's help.
`backmux logs` prints it whole, from the newest N lines (`-n`), or followed
to the process's end (`-f`).

## Safety invariants

- Every managed process gets its own process group and session.
- Signals reach the group only after revalidation: the boot id must match;
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
- stdout carries machine data; human notes go to stderr; `--quiet`
  silences them.
- Backmux never elevates on a user's behalf, with one deliberate exception:
  a no-target `backmux purge --system` run by a normal user re-executes
  itself through `sudo` (which does the prompting) at a single commented
  dispatch site. A targeted purge of a global process is never re-executed: a
  normal user is told it needs root.

## Non-goals

No daemon: nothing runs except the processes of live processes and, per process, a
waiter, restart supervisor, or console broker. No root-log visibility for
normal users. No run-id uniqueness across users' personal stores. No
scanning of other users' private stores by anyone but root. Tests never
touch the real system store or real homes: `make test` compiles with
`HOLD_TESTING`, which honors `HOLD_TEST_SYSTEM_STATE_DIR`,
`HOLD_TEST_HOME_ROOT`, and `HOLD_BOOT_ID_PATH`.
