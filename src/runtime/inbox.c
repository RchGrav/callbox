#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/core.h"
#include "callbox/platform.h"
#include "callbox/store.h"
#include "callbox/runtime.h"

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
    int found = hold_inbox_find(store, key, name, n);
    if (found <= 0) return found; /* 0: it has one; -1: error */
    if (!create) return 1;
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
        if (hold_generate_run_name_for_id(store, hex, want, cand) != 0) continue;
        if (hold_inbox_claim(store, cand, key) == 0) {
            if (made) *made = true;
            return hold_checked_snprintf(name, n, "%s", cand) == 0 ? 0 : -1;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

void hold_operator_first_note(const char *name) {
    fprintf(stderr,
            "callbox: you are %s, outside any call. Replies to you wait in your inbox\n"
            "for 60 minutes:\n"
            "  callbox inbox                    read them\n"
            "  callbox inbox --wait             wait for the next one\n"
            "  callbox inbox --monitor          print each one as it arrives, until stopped\n"
            "  callbox msg <call> TEXT --wait-reply   send, then wait for the answer\n",
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

int hold_inbox_wait(const struct hold_store *store, const char *name, off_t from_off, bool forever_each,
                    long timeout_ms) {
    struct print_ctx p = {.stop_after_one = !forever_each};
    int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
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
        if (deadline && mono_ms() >= deadline) {
            fprintf(stderr, "callbox: no reply for %s within %ld ms\n", name, timeout_ms);
            return 7;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = INBOX_POLL_MS * 1000000L};
        nanosleep(&sl, NULL);
    }
}

/* ---- callbox inbox ---------------------------------------------------------- */

static const char inbox_usage[] = "usage: callbox inbox [<name>] [--wait | --monitor] [--timeout <ms>]\n";

int hold_cmd_inbox_action(const struct hold_invocation *inv, const struct hold_store *user_store, int argc,
                          char **argv) {
    (void)inv;
    const char *name_arg = NULL;
    bool wait = false, monitor = false;
    long timeout_ms = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) return HOLD_RC_SHOW_HELP;
        if (!strcmp(a, "--wait")) wait = true;
        else if (!strcmp(a, "--monitor")) monitor = true;
        else if (!strcmp(a, "--timeout") || !strncmp(a, "--timeout=", 10)) {
            const char *v = a[9] == '=' ? a + 10 : (i + 1 < argc ? argv[++i] : NULL);
            char *end = NULL;
            timeout_ms = v ? strtol(v, &end, 10) : -1;
            if (!v || !*v || *end || timeout_ms < 0) {
                fprintf(stderr, "callbox: error: --timeout takes milliseconds (0 = never)\n%s", inbox_usage);
                return HOLD_EXIT_USAGE;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "callbox: error: unknown flag '%s'\n%s", a, inbox_usage);
            return HOLD_EXIT_USAGE;
        } else if (!name_arg) name_arg = a;
        else {
            fputs(inbox_usage, stderr);
            return HOLD_EXIT_USAGE;
        }
    }
    if (wait && monitor) {
        fprintf(stderr, "callbox: error: --wait and --monitor do not go together\n%s", inbox_usage);
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
            fprintf(stderr, "callbox: you run inside a call: messages for you are typed into your prompt, "
                            "not kept in an inbox\n");
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
