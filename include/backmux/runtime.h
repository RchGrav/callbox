#pragma once
#ifndef HOLD_RUNTIME_H
#define HOLD_RUNTIME_H

#include "backmux/config.h"
#include "backmux/types.h"

bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv);
struct hold_start_options {
    bool tail;
    bool console_mode;
    bool auto_remove;
    bool wait; /* -w: keep a waiter that records the exit code */
    bool interactive_stdin;
    int argc;
    char **argv;
    const char *exec_path;
    const char *run_name;
    int envc;
    char **env;
    const char *restart_policy;
    int restart_delay_seconds; /* HOLD_RESTART_BACKOFF: none given, back off */
    int size_cols, size_rows; /* --size, or a redialed recipe's: 0 = resolve (see start.c) */
    bool chat;                /* broker-composited chat for a retained mode_chat profile */
    bool user_mode;           /* retain mode_chat even when no terminal can display it */
    bool shell;               /* placed with backmux shell */
    const char *working;      /* recorded working directory; NULL = current directory */
    bool no_working;          /* recall of a process whose working was unset: record none */
    unsigned char chat_key;   /* the key that opens chat; 0 = the default */
    const char *existing_id;
    const char *existing_log_path;
    const char *existing_run_name;
    int64_t existing_created_unix_ns;
    const char *existing_created_at;
};
int hold_perform_start_options(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_start_options *opts);
/* list is Backmux's scoped ledger: which stores it draws from depends on the
 * requested scope, resolved against the caller's privilege. */
enum hold_list_scope {
    HOLD_LIST_SCOPE_DEFAULT, /* non-root: the user's own store; root: the global store */
    HOLD_LIST_SCOPE_USER,    /* -u/--user: personal processes only, even under sudo */
    HOLD_LIST_SCOPE_SYSTEM,  /* -s/--system: the global/system store only */
    HOLD_LIST_SCOPE_BOTH     /* -a/--all: user and system scopes together */
};

int hold_cmd_list(const struct hold_invocation *inv,
                    const struct hold_store *user_store,
                    const struct hold_store *system_store,
                    const char *name_filter,
                    enum hold_list_scope scope,
                    bool live_only);
/* ps shows running processes, plus ended records with -a, across the user and
 * system stores available to the caller. It omits the USER column. */
int hold_cmd_ps(const struct hold_invocation *inv,
                  const struct hold_store *user_store,
                  const struct hold_store *system_store,
                  const char *name_filter,
                  bool all);
enum id_token_scope hold_parse_id_token(const char *token, const char **id_out);
int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd);
/* A top-level word that is no verb, no process, and no program: the start of
 * exactly one verb (1, *verb_out) or, for a lone word, of one process's name
 * (2, name_out); 0 when it is none of these (run it as a command); 6 when it
 * starts several (listed on stderr). */
int hold_shorten_word(const char *word, bool lone, const char *const *verbs, size_t nverbs,
                      const char **verb_out, char *name_out, size_t name_n);
int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token);
/* --tail <word>: when the word names a process, follow its log as tail does and
 * return the exit status; otherwise return -1 (the word is a command). */
int hold_cmd_tail_if_call(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *word);
int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token);
int hold_cmd_view_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           int argc,
                           char **argv);
/* Returned by a verb whose own parser met -h/--help (it alone can tell a
 * help flag from, say, send --text --help): the CLI prints the command's
 * help. Never an exit status. */
#define HOLD_RC_SHOW_HELP 1000

/* The screen verbs (src/runtime/screen_cmds.c): argv holds the target and
 * the verb's own flags. */
int hold_cmd_peek_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                         const struct hold_store *system_store, int argc, char **argv);
int hold_cmd_send_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                         const struct hold_store *system_store, int argc, char **argv);
int hold_cmd_wait_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                         const struct hold_store *system_store, int argc, char **argv);
int hold_cmd_resize_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                           const struct hold_store *system_store, int argc, char **argv);
/* Operator inboxes (inbox.c): the name of a sender outside any process (0, or 1
 * when it has none and create is false), the note printed with its first
 * message, waiting on an inbox (forever_each: print every message until
 * stopped; else return after one; exit 0, 7 timed out, 8 the inbox expired),
 * and backmux inbox. */
int hold_operator_name(const struct hold_store *store, bool create, char *name, size_t n, bool *made);
void hold_operator_first_note(const char *name);
int hold_inbox_wait(const struct hold_store *store, const char *name, off_t from_off, bool forever_each, long timeout_ms);
int hold_person_chat_name(const struct hold_store *store, const char *own, char *name, size_t n);
int hold_operator_name_peek(const struct hold_store *store, bool person, char *name, size_t n);
bool hold_inbox_target(const struct hold_store *store, const char **word);
/* Help for whoever runs it (runtime/help_reader.c): fills view for this
 * reader, or for the whole page when full. */
struct hold_help_view;
void hold_help_reader_view(struct hold_help_view *view, bool full);
/* Before an attach at a terminal: what the compositor needs (the store,
 * the person's chat name). */
void hold_prepare_mux(const struct hold_invocation *inv, const struct hold_store *store);
/* --agent: add the program's name to the person's agent list (the config's "agents"). */
void hold_remember_agent(const struct hold_invocation *inv, int argc, char **argv);
int hold_cmd_layout_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc, char **argv);
int hold_cmd_swap_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                         const struct hold_store *system_store, int argc, char **argv);
int hold_cmd_inbox_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc, char **argv);
int hold_cmd_monitor_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc, char **argv);
int hold_cmd_msg_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                        const struct hold_store *system_store, int argc, char **argv);
/* backmux config get|set|unset <setting> [value] (src/runtime/config_cmd.c). */
int hold_cmd_config_action(const struct hold_invocation *inv, int argc, char **argv);
/* The saved detach-size default: 0 with the size, 1 when unset, -1 (message
 * printed) when what is saved does not read as a size. */
int hold_saved_detach_size(const struct hold_invocation *inv, int *cols, int *rows);
int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token);
int hold_cmd_ports_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *id_token);
int hold_cmd_stats_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *id_token,
                            bool no_stream);
int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force,
                            bool system_scope);
/* A process's settings and the verbs around them (src/runtime/settings.c). */
int hold_cmd_set_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                        const struct hold_store *system_store, int argc, char **argv, bool unset);
int hold_cmd_info_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                         const struct hold_store *system_store, const char *token);
int hold_cmd_recall_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                           const struct hold_store *system_store, const char *token);
int hold_cmd_delete_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                           const struct hold_store *system_store, const char *token);
int hold_cmd_purge_all_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                              const struct hold_store *system_store, bool force, bool system_scope);
/* Process-record operations (src/runtime/call.c): redial a retained recipe, save or
 * rename a process, and assign a generated adjective_noun name. */
int hold_cmd_rename_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *target_token,
                             const char *new_name);
int hold_cmd_redial(const struct hold_invocation *inv,
                      const struct hold_store *user_store,
                      const struct hold_store *system_store,
                      bool tail,
                      bool console_mode,
                      bool auto_remove,
                      bool wait,
                      bool interactive_stdin,
                      bool explicit_session_mode,
                      const char *restart_policy,
                      int restart_delay_seconds,
                      int size_cols,
                      int size_rows,
                      const char *token,
                      bool *redialed);
int hold_cmd_save_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *target_token);
int hold_generate_run_name_for_id(const struct hold_store *store,
                                    const char *id,
                                    const char *requested,
                                    char out[ALIAS_MAX_LEN + 1]);
int hold_ensure_start_store_for_command(const struct hold_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct hold_store *store);
void hold_usage(void);

#endif /* HOLD_RUNTIME_H */
