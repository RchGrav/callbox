#pragma once
#ifndef HOLD_RUNTIME_INTERNAL_H
#define HOLD_RUNTIME_INTERNAL_H

#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/runtime.h"

void hold_report_session_escapees(const struct hold_run_record *r);
/* Before attaching: if this process runs inside a call (CALLBOX_ID) that
 * has a console in one of these stores, the attach is nested in that call's
 * terminal; the console layer is told where that call's broker listens. */
void hold_note_enclosing_call(const struct hold_store *a, const struct hold_store *b);
/* The lines a person reads when an attach is turned away (another client has
 * the console; by is its pid, 0 unknown) and after they detach. */
void hold_note_attached_elsewhere(const char *label, pid_t by, const struct hold_store *a, const struct hold_store *b);
void hold_note_detached(const struct hold_invocation *inv, const char *label);
/* An attach to a TTY call has come back (rc 0), or found its console already
 * gone (unseen). Says what that was - detached, or a program that closed its
 * terminal and runs on - and, when the screen is over for good, leaves the
 * call's last screen on the terminal (the attach's alternate screen went
 * with it); with no terminal, output never streamed is printed from the log.
 * Returns true when the call has ended. */
bool hold_after_attach(const struct hold_invocation *inv, const struct hold_store *store,
                       const struct hold_run_record *r, const char *label, bool on_terminal, bool unseen);
/* Is the call running right now? (An attach that ended while its call runs
 * on was a detach.) */
bool hold_call_still_running(const struct hold_store *store, const char *id);
/* An ended TTY call's last screen as plain rows (trailing blank rows left
 * out), from the frame its broker saved. -1 when there is none. */
int hold_print_final_screen(const struct hold_store *store, const char *id, FILE *f);
enum run_state hold_eval_state(const struct hold_run_record *r, const char *current_boot);
/* Running, but its pid is not visible from here (another PID namespace): act
 * through its supervisor - the broker, or a -d call's reaper. */
bool hold_call_elsewhere(const struct hold_run_record *r, const char *current_boot);
/* Does a callbox process record this call's exit code? Its broker (TTY) or a
 * waiter (-w, foreground, --rm, --restart) does; a plain -d call has none. */
bool hold_call_records_exit(const struct hold_run_record *r);
int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit);
int hold_print_run_log(const struct hold_run_record *r);
/* Follow a plain call this process just launched (or redialed) in the
 * foreground: Ctrl-C goes to the program, Ctrl-P Ctrl-Q leaves it running. */
int hold_follow_launched_call(const struct hold_invocation *inv, const struct hold_run_record *r, const char *label);
void hold_rollback_spawned_group(pid_t pid, pid_t pgid);
bool hold_wait_target_group_gone(const struct hold_run_record *r, int timeout_ms);
const char *hold_state_str(enum run_state s);
int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed);
/* As above; allow_unknown also removes a call this shell cannot judge (one
 * from another PID namespace whose end was never recorded) - only for a
 * targeted purge --force. */
int hold_prune_one_run_ex(const struct hold_store *store, const char *id, const char *boot, bool allow_stale,
                          bool allow_unknown, bool *removed);
/* --rm: wait (up to timeout_ms) for the call's reaper to stamp its exit,
 * then remove it, so the verb that ended or awaited it returns with the
 * name free. No-op for calls without --rm or already gone. */
void hold_auto_remove_settle(const struct hold_store *store, const char *id, int timeout_ms);
/* The exit code of an --rm call removed in the last minute, by id prefix or
 * name: removal leaves it behind so `callbox wait` can still report it. */
bool hold_auto_removed_exit(const struct hold_store *store, const char *token, int *code);
bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st);
int hold_report_not_found(const char *token);
int hold_report_requires_root(const char *token);
int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out);

/* Single-target prologue shared by logs/inspect/tail and the screen verbs:
 * resolve one token (*hit false: return rc), then the requires-root gate and
 * record load (*ok false: return rc). */
/* Whether token names a call (or is malformed, which the caller reports);
 * false only for a well-formed token that matches nothing. Prints nothing. */
bool hold_token_resolves(const struct hold_invocation *inv,
                         const struct hold_store *current_user_store,
                         const struct hold_store *system_store,
                         const char *command,
                         const char *token);
int hold_resolve_single(const struct hold_invocation *inv,
                        const struct hold_store *user_store,
                        const struct hold_store *system_store,
                        const char *command,
                        const char *verb,
                        const char *token,
                        struct hold_resolved_target *out,
                        bool *hit);
int hold_load_target_record(const struct hold_resolved_target *target,
                            bool need_log,
                            struct hold_run_record *r,
                            char *path,
                            size_t path_len,
                            bool *ok);

#endif /* HOLD_RUNTIME_INTERNAL_H */
