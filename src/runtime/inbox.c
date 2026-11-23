#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/core.h"
#include "callbox/platform.h"
#include "callbox/store.h"
#include "callbox/runtime.h"
#include "callbox/runtime_internal.h"

/* An agent outside any call (Claude Code in a plain terminal, a script) can
 * message a call, but has no call to type the reply into. It gets a name, made
 * like a call's, and an inbox under that name (store/inbox.c); the receiver
 * sees an ordinary header and replies with an ordinary callbox msg. Why: one
 * way of talking for everyone - the receiver needs to know nothing about
 * where the sender runs.
 *
 * The name belongs to the sender's harness: the first process above callbox
 * that is not a shell or a wrapper, identified by its name, pid, and start
 * time (so every command one Claude Code session runs shares one name, and a
 * reused pid is a new name). Where that walk finds nothing, the name is
 * "operator". */

#define INBOX_POLL_MS 200

static bool is_wrapper(const char *comm) {
    static const char *const skip[] = {"sh",    "bash",  "zsh",    "dash",  "fish", "ksh",  "mksh",  "tcsh",
                                       "csh",   "env",   "timeout", "nohup", "nice", "stdbuf", "xargs", "time",
                                       "script", "sudo", "su", "runuser", "doas", "callbox", NULL};
    for (int i = 0; skip[i]; i++)
        if (!strcmp(comm, skip[i])) return true;
    return false;
}

/* The harness's identity, as "<comm> <pid> <start>"; false if it cannot be
 * told (the walk reaches init, or the process table is hidden). */
static bool harness_key(char *key, size_t n, char *comm_out, size_t comm_n) {
    pid_t pid = getppid();
    for (int depth = 0; depth < 16 && pid > 1; depth++) {
        pid_t ppid = 0;
        char comm[32];
        if (hold_proc_parent_and_name(pid, &ppid, comm, sizeof(comm)) != 0) return false;
        if (!is_wrapper(comm)) {
            uint64_t start = 0;
            if (hold_read_proc_stat_tokens(pid, NULL, &start) != 0) start = 0;
            snprintf(comm_out, comm_n, "%s", comm);
            return hold_checked_snprintf(key, n, "%s %ld %llu", comm, (long)pid, (unsigned long long)start) == 0;
        }
        pid = ppid;
    }
    return false;
}

int hold_operator_name(const struct hold_store *store, bool create, char *name, size_t n, bool *made) {
    if (made) *made = false;
    char key[128], comm[32] = "";
    if (!harness_key(key, sizeof(key), comm, sizeof(comm)))
        snprintf(key, sizeof(key), "operator uid %ld", (long)getuid());
    /* One namespace with calls: keep lookup and claim together, so parallel
     * first sends share an inbox and a launch cannot take its name. Root's
     * personal store may not exist yet; make it before taking its lock. */
    char dir[HOLD_PATH_MAX];
    if (hold_inbox_dir(store, dir, sizeof(dir)) != 0) return -1;
    int lock_fd = hold_store_lock(store->record_dir);
    int rc = hold_inbox_find(store, key, name, n), saved;
    if (rc <= 0 || !create) goto out; /* 0: it has one; -1: error; 1: none */
    /* A generated name, as a call gets: from a hash of the key, stepping on
     * while the name is taken by a call or another inbox. */
    for (unsigned salt = 0; salt < 64; salt++) {
        struct sha256_ctx h;
        unsigned char digest[32];
        char hex[65];
        hold_sha256_init(&h);
        hold_sha256_update(&h, key, strlen(key));
        hold_sha256_update(&h, &salt, sizeof(salt));
        hold_sha256_final(&h, digest);
        hold_hex_encode(digest, sizeof(digest), hex, sizeof(hex));
        char cand[ALIAS_MAX_LEN + 1];
        const char *want = salt == 0 && !strncmp(key, "operator uid ", 13) ? "operator" : NULL;
        if (hold_pick_run_name_locked(store, hex, want, cand) != 0) continue;
        if (hold_inbox_claim(store, cand, key) == 0) {
            if (made) *made = true;
            rc = hold_checked_snprintf(name, n, "%s", cand) == 0 ? 0 : -1;
            goto out;
        }
        if (errno != EEXIST) {
            rc = -1;
            goto out;
        }
    }
    errno = EEXIST;
    rc = -1;
out:
    saved = errno;
    hold_store_unlock(lock_fd);
    errno = saved;
    return rc;
}

void hold_operator_first_note(const char *name) {
    fprintf(stderr,
            "callbox: you are %s, outside any call. Replies to you wait in your inbox\n"
            "for 60 minutes:\n"
            "  callbox inbox                    read them\n"
            "  callbox inbox --wait             wait for the next one\n"
            "  callbox monitor                  print each one as it arrives, until stopped\n"
            "  callbox msg <call> TEXT --wait-reply   wait for the next new message\n",
            name);
}

/* ---- reading -------------------------------------------------------------- */

struct print_ctx {
    bool stop_after_one;
};

static int print_one(time_t at, const char *from, const char *text, size_t len, void *vctx) {
    struct print_ctx *p = vctx;
    struct tm tmv;
    char when[16] = "";
    if (localtime_r(&at, &tmv)) strftime(when, sizeof(when), "%H:%M", &tmv);
    printf("[%s from %s] %.*s\n", when, from, (int)len, text);
    fflush(stdout);
    return p->stop_after_one ? 1 : 0;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int hold_parse_inbox_timeout_ms(const char *text, long *value) {
    if (!text || !*text) return -1;
    char *end = NULL;
    errno = 0;
    long ms = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || ms < 0) return -1;
    *value = ms;
    return 0;
}

int hold_inbox_wait(const struct hold_store *store, const char *name, off_t from_off, bool forever_each,
                    long timeout_ms) {
    struct print_ctx p = {.stop_after_one = !forever_each};
    int64_t started = mono_ms();
    off_t off = from_off;
    for (;;) {
        off_t next = off;
        int got = hold_inbox_read(store, name, off, &next, print_one, &p);
        if (got < 0) {
            fprintf(stderr, "callbox: cannot read the inbox of %s: %s\n", name, strerror(errno));
            return 1;
        }
        off = next;
        if (next > from_off) (void)hold_inbox_mark_seen(store, name, next);
        if (got > 0 && !forever_each) return 0;
        if (!hold_inbox_live(store, name)) {
            fprintf(stderr, "callbox: the inbox of %s has expired\n", name);
            return 8;
        }
        if (timeout_ms > 0 && mono_ms() - started >= timeout_ms) {
            if (forever_each)
                fprintf(stderr, "callbox: monitoring %s ended after %ld ms\n", name, timeout_ms);
            else
                fprintf(stderr, "callbox: no reply for %s within %ld ms\n", name, timeout_ms);
            return 7;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = INBOX_POLL_MS * 1000000L};
        nanosleep(&sl, NULL);
    }
}

/* ---- callbox inbox ---------------------------------------------------------- */

static const char inbox_usage[] = "usage: callbox inbox [<name>] [--wait | --monitor] [--timeout <ms>]\n";
static const char monitor_usage[] = "usage: callbox monitor [<name>] [--timeout <ms>]\n";

static int inbox_action_impl(const struct hold_store *user_store, int argc, char **argv, bool monitor_mode) {
    const char *usage = monitor_mode ? monitor_usage : inbox_usage;
    const char *name_arg = NULL;
    bool wait = false, monitor = monitor_mode;
    long timeout_ms = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) return HOLD_RC_SHOW_HELP;
        if (!monitor_mode && !strcmp(a, "--wait")) wait = true;
        else if (!monitor_mode && !strcmp(a, "--monitor")) monitor = true;
        else if (!strcmp(a, "--timeout") || !strncmp(a, "--timeout=", 10)) {
            const char *v = a[9] == '=' ? a + 10 : (i + 1 < argc ? argv[++i] : NULL);
            if (hold_parse_inbox_timeout_ms(v, &timeout_ms) != 0) {
                fprintf(stderr, "callbox: error: --timeout takes milliseconds (0..%ld; 0 = never)\n%s",
                        LONG_MAX, usage);
                return HOLD_EXIT_USAGE;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "callbox: error: unknown flag '%s'\n%s", a, usage);
            return HOLD_EXIT_USAGE;
        } else if (!name_arg) name_arg = a;
        else {
            fputs(usage, stderr);
            return HOLD_EXIT_USAGE;
        }
    }
    if (wait && monitor) {
        fprintf(stderr, "callbox: error: --wait and --monitor do not go together\n%s", usage);
        return HOLD_EXIT_USAGE;
    }
    char name[ALIAS_MAX_LEN + 1];
    if (name_arg) {
        if (!hold_valid_alias(name_arg) || !hold_inbox_live(user_store, name_arg)) {
            fprintf(stderr, "callbox: %s has no inbox (inboxes expire an hour after their last use)\n", name_arg);
            return HOLD_EXIT_NO_TARGET;
        }
        snprintf(name, sizeof(name), "%s", name_arg);
    } else {
        const char *in_call = getenv("CALLBOX_ID");
        if (in_call && *in_call) {
            fprintf(stderr, "callbox: you run inside a call: messages arrive as program input or chat,\n"
                            "not in an outside inbox\n");
            return HOLD_EXIT_USAGE;
        }
        int rc = hold_operator_name(user_store, false, name, sizeof(name), NULL);
        if (rc != 0) {
            fprintf(stderr, "callbox: you have no inbox yet: one is made the first time you send with callbox msg "
                            "from outside a call\n");
            return HOLD_EXIT_NO_TARGET;
        }
    }
    /* --wait and --monitor start at the first unread message, so one that
     * came while nobody was waiting is not missed. A plain listing shows
     * every message still kept, and marks them read. */
    if (wait || monitor) return hold_inbox_wait(user_store, name, hold_inbox_seen(user_store, name), monitor, timeout_ms);
    struct print_ctx p = {.stop_after_one = false};
    off_t end = 0;
    if (hold_inbox_read(user_store, name, 0, &end, print_one, &p) < 0) return 1;
    (void)hold_inbox_mark_seen(user_store, name, end);
    return 0;
}

int hold_cmd_inbox_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc,
                          char **argv) {
    (void)inv;
    return inbox_action_impl(user_store, argc, argv, false);
}

int hold_cmd_monitor_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc,
                            char **argv) {
    (void)inv;
    return inbox_action_impl(user_store, argc, argv, true);
}
