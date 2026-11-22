#pragma once
#ifndef HOLD_RUNTIME_H
#define HOLD_RUNTIME_H

#include "callbox/config.h"
#include "callbox/types.h"

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
    bool chat;                /* callbox shell: composited display, messages to the chat inbox */
    bool user_mode;           /* placed in user mode: recorded, so redial at a terminal gets chat
                                 even when this launch (from a script) could not draw it */
    bool shell;               /* placed with callbox shell */
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
/* list is Callbox's scoped ledger: which stores it draws from depends on the
 * requested scope, resolved against the caller's privilege. */
enum hold_list_scope {
    HOLD_LIST_SCOPE_DEFAULT, /* non-root: the user's own store; root: the global store */
    HOLD_LIST_SCOPE_USER,    /* -u/--user: personal calls only, even under sudo */
    HOLD_LIST_SCOPE_SYSTEM,  /* -s/--system: the global/system store only */
    HOLD_LIST_SCOPE_BOTH     /* -a/--all: user and system scopes together */
};

int hold_cmd_list(const struct hold_invocation *inv,
                    const struct hold_store *user_store,
                    const struct hold_store *system_store,
                    const char *name_filter,
                    enum hold_list_scope scope,
                    bool live_only);
/* ps is Docker's machine-wide view: running calls (plus ended with -a) across
 * both the caller's own store and the global store, no USER column. */
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
/* A top-level word that is no verb, no call, and no program: the start of
 * exactly one verb (1, *verb_out) or, for a lone word, of one call's name
 * (2, name_out); 0 when it is none of these (run it as a command); 6 when it
 * starts several (listed on stderr). */
int hold_shorten_word(const char *word, bool lone, const char *const *verbs, size_t nverbs,
                      const char **verb_out, char *name_out, size_t name_n);
int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token);
/* --tail <word>: when the word names a call, follow its log as tail does and
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
int hold_cmd_msg_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                        const struct hold_store *system_store, int argc, char **argv);
/* callbox config get|set|unset <setting> [value] (src/runtime/config_cmd.c). */
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
/* Call-record operations (src/runtime/call.c): redial a retained recipe, save or
 * rename a call, and assign a generated adjective_noun name. */
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
