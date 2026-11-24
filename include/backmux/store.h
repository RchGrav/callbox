#pragma once
#ifndef HOLD_STORE_H
#define HOLD_STORE_H

#include "backmux/config.h"
#include "backmux/types.h"

int hold_init_user_store_from_home(const char *home, struct hold_store *store);
int hold_ensure_user_store_for_current_user(struct hold_store *store);
int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);
int hold_init_system_store(struct hold_store *store);
int hold_ensure_system_store(struct hold_store *store);
int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r,
                                     const char *observed_ports_csv);
/* Stamp the exit of the run whose program is pid (0: whatever the record
 * names). -1 when the record is not there yet - or names another pid: this
 * run's launcher has not written it yet (a redial reuses the id), so the
 * caller retries; 1 when it was purged (final). */
int hold_mark_run_finished(const struct hold_store *store, const char *id, int status, pid_t pid);
/* Store-wide advisory lock (see atomic.c); -1 means proceed unlocked. */
int hold_store_lock(const char *record_dir);
void hold_store_unlock(int fd);

/* Two-phase creation, step 1: derive a collision-free hashed run id
 * (backmux-run-v1 material: exe, cwd, timestamp_ns, launcher pid, argc, argv[],
 * counter; <=1024 probes) and hold its .<id>.reserve marker (O_EXCL 0600).
 * A candidate colliding with ANY existing store material (record, .log,
 * .reserve, .sock, public projection) is skipped. Commit is
 * hold_write_record_atomic, which unlinks the reserve after the record
 * rename; abort with hold_abort_run_reservation. */
int hold_reserve_run_id(const struct hold_store *store,
                        const char *resolved_exec_path,
                        int argc, char **argv,
                        const char *cwd,
                        int64_t start_unix_ns,
                        char out_id[ID_STR_LEN]);
void hold_abort_run_reservation(const struct hold_store *store, const char *id);
/* Redial's claim on an existing call: the same .<id>.reserve, held under an
 * exclusive flock for the whole launch. A second redial of the call waits
 * here, then sees the first one's record, so one id never gets two
 * programs (checking "not running" and launching are otherwise separate
 * steps, and a burst of redials all passed the check). A lock, not O_EXCL
 * alone, so a launcher that died mid-launch leaves no claim behind. Commit
 * is hold_write_record_atomic (it unlinks the reserve), then
 * hold_release_existing_run; abort is hold_release_existing_run alone.
 * Returns the locked fd, or -1 with errno. */
int hold_claim_existing_run(const struct hold_store *store, const char *id);
void hold_release_existing_run(const struct hold_store *store, const char *id, int fd);
/* For purge, under the store lock: true while a redial is launching the
 * call (its claim is held). Why: the redial wrote its running record only at
 * the end of the launch, so a purge in between removed a call that was
 * starting. An unheld claim is a dead launcher's litter, removed here, since
 * the record it belongs to is about to go and nothing else would. */
bool hold_existing_run_claimed(const struct hold_store *store, const char *id);
/* The running-record base builder: stamps every field the launch and adoption
 * paths set identically (ids, group identity, timestamps, state, owner, log).
 * Callers add their own provenance (recipe/observed/boot/name/console). */
void hold_record_init_running(struct hold_run_record *r,
                              const char *id,
                              const char *log_path,
                              pid_t pid, pid_t pgid, pid_t sid,
                              int64_t start_unix_ns,
                              int64_t created_unix_ns);
int hold_load_record(const char *path, struct hold_run_record *r);
void hold_free_run_record(struct hold_run_record *r);
int hold_load_public_index(const char *path, struct hold_public_index *pi);
int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi);
int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n);

/* THE id resolver: exact 64-hex id if its file exists, else a prefix matching
 * exactly one <id>.json in dir. Works on record and public-index directories. */
/* 0 one match, -1 none (or an error), -2 several. */
int hold_resolve_record_id(const char *dir, const char *token, char *resolved, size_t n);

/* Record iteration: fn runs per <id>.json with the loaded record, or r == NULL
 * when the file is corrupt (unloadable, invalid, or embedded id != filename).
 * The record is freed after fn returns; a nonzero return stops the walk and
 * becomes hold_for_each_record's return value. An unopenable dir walks empty. */
typedef int (*hold_record_fn)(const char *id, const char *path, struct hold_run_record *r, void *ctx);
int hold_for_each_record(const char *record_dir, hold_record_fn fn, void *ctx);

/* Per-user settings (settings.c): "<home>/.config/backmux/config", one
 * "name value" line per setting; the person's own symlinks along that path
 * are followed. get: 0 with the value, 1 when unset, -1 on error. put
 * rewrites the file atomically, 0600 (value NULL removes the setting); it
 * runs with the caller's own rights only - never as root in another user's
 * home. */
int hold_settings_path(const char *home, char *out, size_t n);
int hold_settings_get(const char *home, const char *name, char *value, size_t n);
int hold_settings_put(const char *home, const char *name, const char *value);

/* Operator inboxes (inbox.c): replies for a sender outside any call, under a
 * generated name, in <store base>/inbox/. A name and its messages expire after
 * HOLD_INBOX_TTL_S unused. find: 0 and the name already given to key, 1 none
 * (expired names are removed on the way). claim: give name to key (fails with
 * EEXIST if it is taken). deliver: append one message (ENOENT: no live inbox).
 * read: call fn for each live message past from_off, in order; a nonzero
 * return from fn stops after that message; *end_off is where to read from
 * next. Returns 1 if fn saw a message, 0 if none, -1 on error. Text handed to
 * fn has had control characters removed. */
typedef int (*hold_inbox_fn)(time_t at, const char *from, const char *text, size_t len, void *ctx);
int hold_inbox_dir(const struct hold_store *s, char *out, size_t n);
bool hold_inbox_live(const struct hold_store *s, const char *name);
void hold_inbox_touch(const struct hold_store *s, const char *name);
int hold_inbox_find(const struct hold_store *s, const char *key, char *name, size_t n);
int hold_inbox_claim(const struct hold_store *s, const char *name, const char *key);
int hold_inbox_deliver(const struct hold_store *s, const char *name, const char *from, const char *text, size_t len);
off_t hold_inbox_size(const struct hold_store *s, const char *name);
int hold_inbox_read(const struct hold_store *s, const char *name, off_t from_off, off_t *end_off, hold_inbox_fn fn, void *ctx);
/* How far into the inbox has been shown, and moving that mark forward. */
off_t hold_inbox_seen(const struct hold_store *s, const char *name);
int hold_inbox_mark_seen(const struct hold_store *s, const char *name, off_t off);

#endif /* HOLD_STORE_H */
