#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"

/* Senders outside managed processes receive replies through named operator
 * inboxes (store/inbox.c). Recipients reply with the same backmux msg command
 * used for managed processes.
 *
 * The name belongs to the sender's harness: the first process above backmux
 * that is not a shell or a wrapper, identified by its name, pid, and start
 * time, when available. If no harness is found, the account UID identifies
 * the sender and "operator" is the preferred name if available. */

#define INBOX_POLL_MS 200

static bool is_wrapper(const char *comm) {
    static const char *const skip[] = {"sh",    "bash",  "zsh",    "dash",  "fish", "ksh",  "mksh",  "tcsh",
                                       "csh",   "env",   "timeout", "nohup", "nice", "stdbuf", "xargs", "time",
                                       "script", "sudo", "su", "runuser", "doas", "backmux", NULL};
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

static int operator_name_for(const struct hold_store *store, const char *key, const char *own, bool create,
                             char *name, size_t n, bool *made);

int hold_operator_name(const struct hold_store *store, bool create, char *name, size_t n, bool *made) {
    char key[128], comm[32] = "";
    if (!harness_key(key, sizeof(key), comm, sizeof(comm)))
        snprintf(key, sizeof(key), "operator uid %ld", (long)getuid());
    return operator_name_for(store, key, NULL, create, name, n, made);
}

/* The person's chat name: one per account, the same in every terminal (chat
 * follows them), their chosen chat-name when that is free, else generated.
 * It is an operator inbox like any sender's outside a process, so agents reply
 * to it with backmux msg. */
int hold_person_chat_name(const struct hold_store *store, const char *own, char *name, size_t n) {
    char key[64];
    snprintf(key, sizeof(key), "person uid %ld", (long)getuid());
    return operator_name_for(store, key, own && *own ? own : NULL, true, name, n, NULL);
}

static int operator_name_for(const struct hold_store *store, const char *key, const char *own, bool create,
                             char *name, size_t n, bool *made) {
    if (made) *made = false;
    /* One namespace with processes: keep lookup and claim together, so parallel
     * first sends share an inbox and a launch cannot take its name. Root's
     * personal store may not exist yet; make it before taking its lock. */
    char dir[HOLD_PATH_MAX];
    if (hold_inbox_dir(store, dir, sizeof(dir)) != 0) return -1;
    int lock_fd = hold_store_lock(store->record_dir);
    int rc = hold_inbox_find(store, key, name, n), saved;
    if (rc <= 0 || !create) goto out; /* 0: it has one; -1: error; 1: none */
    /* A generated name, as a process gets: from a hash of the key, stepping on
     * while the name is taken by a process or another inbox. */
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
        const char *want = salt == 0 && own ? own : salt == 0 && !strncmp(key, "operator uid ", 13) ? "operator" : NULL;
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
            "backmux: sender %s is outside any managed process. Replies use an operator inbox\n"
            "that expires after 60 minutes of inactivity:\n"
            "  backmux inbox                    Read retained messages\n"
            "  backmux inbox --wait             Wait for an unread message\n"
            "  backmux monitor                  Follow incoming messages\n"
            "  backmux msg <target> TEXT --wait-reply   Send and wait for a new message\n",
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
            fprintf(stderr, "backmux: cannot read the inbox of %s: %s\n", name, strerror(errno));
            return 1;
        }
        off = next;
        if (next > from_off) (void)hold_inbox_mark_seen(store, name, next);
        if (got > 0 && !forever_each) return 0;
        if (!hold_inbox_live(store, name)) {
            fprintf(stderr, "backmux: the inbox of %s has expired\n", name);
            return 8;
        }
        if (timeout_ms > 0 && mono_ms() - started >= timeout_ms) {
            if (forever_each)
                fprintf(stderr, "backmux: monitoring %s ended after %ld ms\n", name, timeout_ms);
            else
                fprintf(stderr, "backmux: no reply for %s within %ld ms\n", name, timeout_ms);
            return 7;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = INBOX_POLL_MS * 1000000L};
        nanosleep(&sl, NULL);
    }
}

/* ---- backmux inbox ---------------------------------------------------------- */

static const char inbox_usage[] = "usage: backmux inbox [<name>] [--wait | --monitor] [--timeout <ms>]\n";
static const char monitor_usage[] = "usage: backmux monitor [<name>] [--timeout <ms>]\n";

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
                fprintf(stderr, "backmux: error: --timeout takes milliseconds (0..%ld; 0 = never)\n%s",
                        LONG_MAX, usage);
                return HOLD_EXIT_USAGE;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "backmux: error: unknown flag '%s'\n%s", a, usage);
            return HOLD_EXIT_USAGE;
        } else if (!name_arg) name_arg = a;
        else {
            fputs(usage, stderr);
            return HOLD_EXIT_USAGE;
        }
    }
    if (wait && monitor) {
        fprintf(stderr, "backmux: error: --wait and --monitor cannot be combined\n%s", usage);
        return HOLD_EXIT_USAGE;
    }
    char name[ALIAS_MAX_LEN + 1];
    if (name_arg) {
        if (!hold_valid_alias(name_arg) || !hold_inbox_live(user_store, name_arg)) {
            fprintf(stderr, "backmux: %s has no inbox (inboxes expire an hour after their last use)\n", name_arg);
            return HOLD_EXIT_NO_TARGET;
        }
        snprintf(name, sizeof(name), "%s", name_arg);
    } else {
        const char *in_call = getenv("BACKMUX_ID");
        if (in_call && *in_call) {
            fprintf(stderr, "backmux: this command runs inside a managed process; use backmux msg --received\n"
                            "to read its received-message history\n");
            return HOLD_EXIT_USAGE;
        }
        int rc = hold_operator_name(user_store, false, name, sizeof(name), NULL);
        if (rc != 0) {
            fprintf(stderr, "backmux: no operator inbox exists for this sender; send with backmux msg "
                            "from outside a managed process to create one\n");
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
