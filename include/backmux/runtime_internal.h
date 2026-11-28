#pragma once
#ifndef HOLD_RUNTIME_INTERNAL_H
#define HOLD_RUNTIME_INTERNAL_H

#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"

void hold_report_session_escapees(const struct hold_run_record *r);
/* Choose a name shared by processes and operator inboxes, honoring records,
 * pending launch reservations and live inboxes. The caller holds the store
 * lock and records its claim before releasing it. */
int hold_pick_run_name_locked(const struct hold_store *store, const char *id, const char *requested,
                              char out[ALIAS_MAX_LEN + 1]);
/* Parse a nonnegative, representable millisecond count: 0 succeeds, -1 is
 * invalid. Shared by inbox/monitor and msg --wait-reply. */
int hold_parse_inbox_timeout_ms(const char *text, long *value);
/* Before attaching: if this process runs inside a process (BACKMUX_ID) that
 * has a console in one of these stores, the attach is nested in that process's
 * terminal; the console layer is told where that process's broker listens. */
void hold_note_enclosing_call(const struct hold_store *a, const struct hold_store *b);
/* The lines a person reads when an attach is turned away (another client has
 * the console; by is its pid, 0 unknown) and after they detach. */
void hold_note_attached_elsewhere(const char *label, pid_t by, const struct hold_store *a, const struct hold_store *b);
/* An attach to a TTY process has come back (rc 0), or found its console already
 * gone (unseen). Says what that was - detached, or a program that closed its
 * terminal and runs on - and, when the screen is over for good, leaves the
 * process's last screen on the terminal (the attach's alternate screen went
 * with it); with no terminal, output never streamed is printed from the log.
 * Returns true when the process has ended. */
bool hold_after_attach(const struct hold_invocation *inv, const struct hold_store *store,
                       const struct hold_run_record *r, const char *label, bool on_terminal, bool unseen);
/* Is the process running right now? (An attach that ended while its process runs
 * on was a detach.) */
bool hold_call_still_running(const struct hold_store *store, const char *id);
/* Say that more than one process matches token (an id prefix or a name, or with
 * name_prefix the start of a name), listing them; returns 6. */
int hold_report_ambiguous(const struct hold_store *store, const char *token, bool name_prefix);
/* Processes in store whose name starts with prefix (that command can act on;
 * NULL: any). matched gets the last one's id. */
int hold_count_name_prefix(const struct hold_store *store, const char *prefix, const char *command,
                           char matched[ID_STR_LEN]);
/* Print the saved final screen, omitting trailing blank rows. Terminal output
 * retains styles; redirected output is plain text. -1 if no frame is available. */
int hold_print_final_screen(const struct hold_store *store, const char *id, FILE *f);
enum run_state hold_eval_state(const struct hold_run_record *r, const char *current_boot);
/* A place ("2C") on the terminal showing panes, as its process (mux_cmd.c). */
int hold_place_to_id(const struct hold_store *user_store, const char *word, char *id, size_t idn, char *name, size_t nn);
/* Whether a running process was launched in a different PID namespace.
 * Brokered processes can receive control requests through their console. */
bool hold_call_elsewhere(const struct hold_run_record *r, const char *current_boot);
/* Does a Backmux process record this target's exit code? A console broker or a
 * waiter (-w, foreground, --rm, --restart) does; a plain -d process has none. */
bool hold_call_records_exit(const struct hold_run_record *r);
int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit);
int hold_print_run_log(const struct hold_run_record *r);
/* Follow a plain process this process just launched (or redialed) in the
 * foreground: Ctrl-C goes to the program, Ctrl-Space q leaves it running. */
int hold_follow_launched_call(const struct hold_invocation *inv, const struct hold_run_record *r, const char *label);
void hold_rollback_spawned_group(pid_t pid, pid_t pgid);
bool hold_wait_target_group_gone(const struct hold_run_record *r, int timeout_ms);
const char *hold_state_str(enum run_state s);
int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed);
/* As above; allow_unknown also removes a process this shell cannot judge (one
 * from another PID namespace whose end was never recorded): for delete, rm -f
 * and purge all. op names the operation in diagnostics ("purge", "delete",
 * "remove"); NULL prints nothing, for a caller that reports in its own words.
 * Returns 0 removed, 2 refused (still running, unknown, being redialed), 4 the
 * record could not be unlinked (errno kept), 5 no such record. */
int hold_prune_one_run_ex(const struct hold_store *store, const char *id, const char *boot, bool allow_stale,
                          bool allow_unknown, const char *op, bool *removed);
/* --rm: wait (up to timeout_ms) for the process's reaper to stamp its exit,
 * then remove it, so the verb that ended or awaited it returns with the
 * name free. No-op for processes without --rm or already gone. */
void hold_auto_remove_settle(const struct hold_store *store, const char *id, int timeout_ms);
/* The exit code of an --rm process removed in the last minute, by id prefix or
 * name: removal leaves it behind so `backmux wait` can still report it. */
bool hold_auto_removed_exit(const struct hold_store *store, const char *token, int *code);
bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st);
int hold_report_not_found(const char *token);
int hold_report_requires_root(const char *token);
/* How recall rebuilds a terminal from its observed state (src/runtime/restore.c):
 * dir, where to start it ("" = as launched); input, what the broker types into
 * a rebuilt shell at its prompt (NULL = nothing); note, what could not be
 * rebuilt, for stderr. -1 on allocation failure. */
struct hold_restore {
    char dir[HOLD_PATH_MAX];
    char *input;
    char note[HOLD_PATH_MAX + 160]; /* room for a directory's path */
};
int hold_restore_plan(const struct hold_run_record *r, struct hold_restore *p);
void hold_restore_free(struct hold_restore *p);
/* Remove one named process (delete's removal, src/runtime/remove.c): saved
 * or not; one found running is ended first. Reports "Removed NAME." */
int hold_remove_target(const struct hold_invocation *inv, const struct hold_store *user_store,
                       const struct hold_store *system_store, const char *token);
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
/* Whether token names a process (or is malformed, which the caller reports);
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
