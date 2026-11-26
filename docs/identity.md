# Identity and validation

[Docs index](index.md) | [Previous: Store](store.md) | Related: [Launcher](launcher.md), [backmux identity](backmux-identity.md)

Backmux records process identity at launch and checks it before signaling a local process. A Backmux process ID or name selects a record, which must match the live process before Backmux sends a signal. From another PID namespace, Backmux asks the process's broker to signal its own program instead.

An unavailable process or lock probe does not mean that a process has exited. When status is `Unknown`, its console may still be reachable through the authenticated broker connection.

## Recorded identity

A record stores:

- `pid`: the program leader's PID, or the restart supervisor's PID under `--restart`.
- `pgid`: the process group to signal.
- `sid`: the session the program's process created with `setsid`.
- `boot_id`: a boot marker, when one can be read.
- `proc_starttime_ticks`: the process start time, or 0 when it could not be read. On Linux it is the starttime field of `/proc/<pid>/stat`, in clock ticks. On macOS it is `p_starttime`, in microseconds.
- `exe_dev` and `exe_ino`: the device and inode of the executable, or 0 when they could not be read.
- `pid_ns`: the PID namespace the process was launched in (Linux; 0 means unknown).
- `liveness_lock`: whether the process's supervisor holds a lock on the log for the process's whole life.

On Linux the probes read `/proc/<pid>/stat`, `/proc/<pid>/exe`, `/proc/self/ns/pid`, and `/proc/sys/kernel/random/boot_id`. macOS has no `/proc`, so start time and executable come from `sysctl(KERN_PROC_PID)` and `proc_pidpath`, and the boot marker is `kern.bootsessionuuid` (`macos-<UUID>`), new at each boot and fixed until the next. Not the boot time: macOS recomputes `kern.boottime` when it adjusts the clock, so the same boot read differently later.

A probe can fail: the process may already be gone, permission may be denied, or the platform may lack the source. A failed probe is recorded as absent, never guessed. Checks use the identity values that exist.

## State evaluation

`hold_eval_state` computes a process's state from its record and available liveness evidence each time a command asks: the local process table, or the log lock across PID namespaces. Nothing stores the result. The record's `state` field records launch or exit information; it is
separate from the state evaluated by each command.

```mermaid
flowchart TD
    Start["Load record"] --> NS["Launched in another PID namespace?"]

    NS -->|no| Failed["Record marked failed?"]
    Failed -->|yes| SF["failed"]
    Failed -->|no| Ids["pgid > 1 and sid > 0?"]
    Ids -->|no| SU["unknown"]
    Ids -->|yes| Boot["Boot ID differs?"]
    Boot -->|yes| SS["stale"]
    Boot -->|no| Zombie["Leader readable and a zombie?"]
    Zombie -->|yes| Scan1["Scan group: same pgid and sid"]
    Scan1 -->|live member| SR["running"]
    Scan1 -->|empty or zombies| SE["exited"]
    Scan1 -->|scan error| SU
    Zombie -->|no| Present["Leader present?"]
    Present -->|yes| Ident["Recorded process identity differs?"]
    Ident -->|yes| SS
    Ident -->|no, or nothing to compare| SR
    Present -->|no| Scan2["Scan group: same pgid and sid"]
    Scan2 -->|live member| SR
    Scan2 -->|empty or zombies| SE
    Scan2 -->|scan error| Pgid["kill(-pgid, 0)"]
    Pgid -->|ESRCH| SE
    Pgid -->|exists or error| SU

    NS -->|yes| NFailed["Record marked failed?"]
    NFailed -->|yes| SF
    NFailed -->|no| NBoot["Boot ID differs?"]
    NBoot -->|yes| SS
    NBoot -->|no| NExit["Exit code recorded?"]
    NExit -->|yes| SE
    NExit -->|no| Lock["Inspect log lock"]
    Lock -->|held| SR
    Lock -->|unavailable or error| SU
    Lock -->|successfully checked: unlocked| LL["liveness_lock recorded?"]
    LL -->|yes| SE
    LL -->|no| SU

    classDef check fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safe fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef refuse fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    class NS,Failed,Ids,Boot,Zombie,Scan1,Present,Ident,Scan2,Pgid,NFailed,NBoot,NExit,Lock,LL check
    class SR,SE,SF safe
    class SS,SU refuse
```

Notes on the diagram:

- **Boot check:** applies only when the record has a boot ID and the current boot ID can be read.
- **Marked failed:** means `state: failed` or a `launch_error` in the record. Backmux does not write either; the evaluator still honors them.
- **Leader present:** the leader's stat is readable and it is not a zombie, or `kill(pid, 0)` succeeds or fails with `EPERM`.
- **Process identity:** compare the recorded starttime, or the executable device and inode when no starttime was recorded and the executable can be inspected.

**Zombie leader.** A zombie leader does not establish that the process group has exited. The process-group leader can exit while useful descendants remain, which is common with launchers and shell wrappers. If live members of the same group and session remain, the process is running; if the group is empty or holds only zombies, it has exited.

**Another PID namespace.** Some sandboxes give each shell its own PID namespace. A PID recorded in another namespace may refer to no process or an unrelated
process in the current namespace, so local signaling does not use it. The filesystem is shared, and `flock` locks are held in the kernel, so a lock on the log can still show that the process is alive.

These processes hold the lifetime lock: the waiter or restart supervisor of a process started under the console fallback (Unix sockets denied, or `BACKMUX_CONSOLE_TRANSPORT=fifo`), and a broker whose endpoint is a FIFO. This includes both terminal processes (`-t`) and processes with input on a pipe (`-i`). These processes record `liveness_lock: true`.

The probe has three outcomes: held, successfully checked and unlocked, or unavailable. A held lock shows the process is running. An unlocked lifetime lock gives `Exited (?)` when no exit was recorded. A missing or unreadable log, or an error from the lock operation, gives `Unknown`; it does not establish that a live supervisor released its lock.

A plain `-d` process under the fallback has no supervisor. Its program's output descriptors hold a shared lock instead. A held lock shows the process runs; a missing lock proves nothing, because a program may close its output.

The normal socket path takes no lifetime lock. A foreign process started that way can therefore have status `Unknown` while its broker is still reachable. Console commands can try the connection in that state, using the same access checks as for a known running process. Status evidence and the ability to communicate with the broker are separate. See [Console](console.md#where-unix-sockets-are-denied) for the supported FIFO path and its regression coverage.

## What each state allows

Purge uses these states. Signaling checks the live process itself (below); the signal column shows the usual outcome.

| State | Signal (`end`, `kill`) | Purge |
|---|---|---|
| running | yes, after validation | only with `--force`, which ends it first |
| exited, failed | nothing to signal: reported as already exited | yes |
| stale | refused | yes |
| unknown | through a reachable broker in another PID namespace; otherwise refused | only with `--force` |

## Signal safety

`signal_one` (src/runtime/signal.c) loads the record. If the process runs in another PID namespace:

- A `-t` or `-i` process is signaled through its broker, which can see its program. This can work even when this shell reports `Unknown` because it cannot inspect the log lock.
- A plain `-d` or `-dw` process has no broker to ask, so it is refused (exit 3).
- `--print` is refused (exit 3), because the recorded PID is not a valid local signal target.
- A failed broker connection is reported as a failure to reach the process, not as proof that it exited.

Otherwise `validate_signal_target` checks the live process before any signal:

1. If `pgid` ≤ 1 or `sid` ≤ 0, refuse (exit 5).
2. If the boot ID changed, refuse (exit 2).
3. If the leader is alive and not a zombie, its recorded starttime must match (or, when no starttime was recorded, its executable device and inode), and its current group and session must match the record. Otherwise refuse (exit 2).
4. If the leader is gone, the group is checked:
   - live members in the recorded group and session: proceed
   - empty or zombies only: the process has already exited
   - the table cannot be scanned: refuse (exit 2)
5. Where step 3 or 4 would proceed, a record with no identity value at all (no starttime, no executable identity) is refused (exit 2).

When the leader is gone but the group lives, nothing is left to compare the recorded identity against, and a process group cannot be checked and signaled in one atomic step. Matching the recorded `pgid` and `sid` within the same boot is the best available check.

Backmux then sends the signal to the whole group with `kill(-pgid, sig)`. `ESRCH` means the process already exited; `EPERM` gives exit 3.

**`end`** (alias `stop`) escalates in steps:

1. Send `SIGTERM`.
2. Poll every 25 ms, for up to 5 seconds (`STOP_TIMEOUT_MS`), until the group is gone.
3. If it remains, validate again and send `SIGKILL`.
4. Wait up to 1 second more. A group that still remains gives exit 4.

**`kill`** sends `SIGKILL` at once and does not wait.

Afterwards, `hold_report_session_escapees` warns about processes still in the session but outside the group.

**`--print`** runs the same validation and also requires the process to be live. It then prints `kill -TERM -- -<pgid>` or `kill -KILL -- -<pgid>` and signals nothing. Nothing is printed for a process that is stale, cannot be validated, has already exited, or runs in another PID namespace.

## Why this design works

PID reuse is the core hazard. Backmux has no central daemon to keep live state, so it records identity at launch and combines it with live probes before every signal. When the machine state is ambiguous, refusing is the safe result.

## Implementation map

`hold_eval_state`, `eval_local_state`, `probe_log_lock`, `hold_call_elsewhere`, `hold_report_session_escapees`, `hold_wait_target_group_gone` (src/runtime/state.c); `hold_read_proc_stat_tokens`, `hold_read_proc_exe`, `hold_leader_present`, `hold_group_exists`, `hold_group_session_liveness`, `hold_count_session_escapees`, `hold_current_pid_ns` (src/platform/process.c); `hold_current_boot_id`, `hold_boot_id_or_null` (src/platform/boot.c); `validate_signal_target`, `signal_through_broker`, `signal_one`, `hold_cmd_signal_action` (src/runtime/signal.c).

## Continue

[Back to docs index](index.md) | [Top](#identity-and-validation) | Branch to: [Launcher](launcher.md), [Store](store.md)
