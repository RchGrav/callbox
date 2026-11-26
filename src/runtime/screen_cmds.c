#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/access.h"
#include "backmux/screen.h"

#include <ctype.h>
#include <regex.h>

/* The screen verbs: backmux peek / send / wait / resize. Each resolves one process,
 * then talks to its console broker over the control protocol. An ended process's
 * last screen is read from <id>.screen beside its log. */

#define WAIT_EXIT_TIMEOUT 7
#define WAIT_EXIT_ENDED 8
#define WAIT_IDLE_DEFAULT_MS 500
#define WAIT_CONSOLE_CLOSED (-2) /* run_wait: the console closed; never an exit status */
#define SEND_EXIT_MOUSE_OFF 9 /* the program has mouse reporting off */

struct screen_target {
    struct hold_resolved_target target;
    struct hold_run_record r;
    enum run_state st;
    char label[ALIAS_MAX_LEN + 16];
};

static int open_screen_target(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *command,
                              const char *token,
                              struct screen_target *t) {
    bool hit = false, ok = false;
    memset(t, 0, sizeof(*t));
    int rc = hold_resolve_single(inv, user_store, system_store, command, command, token, &t->target, &hit);
    if (!hit) return rc == 0 ? 5 : rc;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&t->target, false, &t->r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    t->st = hold_eval_state(&t->r, hold_boot_id_or_null(boot));
    if (t->r.has_name && t->r.name[0]) snprintf(t->label, sizeof(t->label), "%s", t->r.name);
    else snprintf(t->label, sizeof(t->label), "%.12s", t->r.id);
    return 0;
}

static void close_screen_target(struct screen_target *t) {
    hold_free_run_record(&t->r);
}

static int no_screen(const struct screen_target *t) {
    fprintf(stderr, "backmux: %s has no terminal screen; start a terminal process with backmux detach <program>\n", t->label);
    return HOLD_EXIT_NO_SCREEN;
}

/* Unknown means the process cannot be observed here. Its broker may still
 * answer, especially across PID namespaces sharing a filesystem. */
static bool state_ended(enum run_state st) {
    return st == STATE_EXITED || st == STATE_STALE || st == STATE_FAILED;
}

/* One control request, retried while the broker answers "error busy" (it
 * serves a bounded number of clients and turns the rest away without acting,
 * so retrying is safe) or a running process's socket refuses the connection
 * (possibly a full accept queue); backs off for up to 10 s. -1 with errno when the
 * broker cannot be reached; *reply stays "error busy" if it never frees up. */
static int control_quiet(const struct screen_target *t, const char *req, size_t len, unsigned char **reply, size_t *reply_len) {
    long delay_ms = 20, waited_ms = 0;
    for (;;) {
        if (hold_console_request(t->r.console_sock, req, len, reply, reply_len) != 0) {
            /* Connection refusal may indicate a full accept queue. Retry
             * while the recorded process is running. */
            if (errno != ECONNREFUSED || t->st != STATE_RUNNING || waited_ms >= 10000) return -1;
            int saved = errno;
            struct timespec sl = {.tv_sec = delay_ms / 1000, .tv_nsec = (delay_ms % 1000) * 1000000L};
            nanosleep(&sl, NULL);
            waited_ms += delay_ms;
            delay_ms = delay_ms * 2 > 500 ? 500 : delay_ms * 2;
            errno = saved;
            continue;
        }
        if (strcmp((const char *)*reply, "error busy\n") != 0 || waited_ms >= 10000) return 0;
        free(*reply);
        *reply = NULL;
        struct timespec sl = {.tv_sec = delay_ms / 1000, .tv_nsec = (delay_ms % 1000) * 1000000L};
        nanosleep(&sl, NULL);
        waited_ms += delay_ms;
        delay_ms = delay_ms * 2 > 500 ? 500 : delay_ms * 2;
    }
}

/* The process's state now (t->st is from when the command started). */
static enum run_state current_state(const struct screen_target *t) {
    struct hold_run_record cur;
    char path[HOLD_PATH_MAX], boot[128];
    if (hold_load_record_by_id(t->target.store.record_dir, t->r.id, &cur, path, sizeof(path)) != 0) {
        int code;
        return hold_auto_removed_exit(&t->target.store, t->r.id, &code) ? STATE_EXITED : STATE_UNKNOWN;
    }
    enum run_state st = hold_eval_state(&cur, hold_boot_id_or_null(boot));
    hold_free_run_record(&cur);
    return st;
}

/* Send one control request; *reply is the malloc'd reply. rc 0 when the
 * broker answered, else a nonzero exit status (message printed). */
static int control(const struct screen_target *t, const char *req, size_t len, unsigned char **reply, size_t *reply_len) {
    if (control_quiet(t, req, len, reply, reply_len) != 0) {
        if (errno == ENOENT) {
            /* The broker stops listening once no more output can come. */
            enum run_state st = current_state(t);
            if (st == STATE_UNKNOWN) {
                fprintf(stderr, "backmux: cannot reach %s's console; its process state is unknown\n", t->label);
                return 3;
            }
            if (st == STATE_RUNNING)
                fprintf(stderr, "backmux: %s closed its terminal, so its console takes no more requests "
                                "(the program is still running; backmux peek shows its last screen)\n", t->label);
            else
                fprintf(stderr, "backmux: %s has ended\n", t->label);
            return WAIT_EXIT_ENDED;
        }
        if (errno == EMSGSIZE) {
            fprintf(stderr, "backmux: %s: too much text for one request (limit about %u KiB); split it across several sends\n",
                    t->label, HOLD_CONSOLE_MAX_REQUEST / 2 / 1024);
            return 1;
        }
        fprintf(stderr, "backmux: cannot reach %s's console: %s\n", t->label, strerror(errno));
        return 3;
    }
    return 0;
}

static int reply_error(const struct screen_target *t, const unsigned char *reply) {
    const char *msg = (const char *)reply;
    if (!strcmp(msg, "error busy\n")) msg = "busy: too many clients at once; try again\n";
    else if (!strncmp(msg, "error ", 6)) msg += 6;
    fprintf(stderr, "backmux: %s: %.*s\n", t->label, (int)strcspn(msg, "\n"), msg);
    return 1;
}

static int read_saved_frame(const struct screen_target *t, const char *suffix, struct hold_snapshot *snap) {
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s%s", t->target.store.log_dir, t->r.id, suffix) != 0) return -1;
    char *buf = NULL;
    size_t len = 0;
    if (hold_read_owned_file_capped(path, MAX_SCREEN_FILE_BYTES, &buf, &len) != 0 || !buf) return -1;
    int rc = hold_snapshot_parse((unsigned char *)buf, len, snap);
    free(buf);
    return rc;
}

static int read_saved_screen(const struct screen_target *t, struct hold_snapshot *snap) {
    return read_saved_frame(t, ".screen", snap);
}

int hold_print_final_screen(const struct hold_store *store, const char *id, FILE *f) {
    char path[HOLD_PATH_MAX];
    char *buf = NULL;
    size_t len = 0;
    struct hold_snapshot snap;
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.screen", store->log_dir, id) != 0 ||
        hold_read_owned_file_capped(path, MAX_SCREEN_FILE_BYTES, &buf, &len) != 0 || !buf)
        return -1;
    int rc = hold_snapshot_parse((unsigned char *)buf, len, &snap);
    free(buf);
    if (rc != 0) return -1;
    size_t cap = (size_t)snap.cols * 4 * HOLD_CELL_CHARS + 1;
    char *line = malloc(cap);
    int last = snap.rows - 1;
    for (; line && last >= 0; last--) {
        hold_snapshot_row_text(&snap, last, line, cap);
        if (line[0]) break;
    }
    /* Preserve terminal styles for terminal output; use plain text for pipes
     * and files. */
    bool colors = isatty(fileno(f));
    for (int r = 0; line && r <= last; r++) {
        if (colors) {
            char *row = NULL;
            size_t rl = 0, rc2 = 0;
            if (hold_snapshot_row_ansi(&snap, r, &row, &rl, &rc2) == 0 && row) fprintf(f, "%.*s\n", (int)rl, row);
            free(row);
        } else {
            hold_snapshot_row_text(&snap, r, line, cap);
            fprintf(f, "%s\n", line);
        }
    }
    free(line);
    hold_snapshot_free(&snap);
    fflush(f);
    return 0;
}

/* Live screen from the broker, else the saved final frame. With last_alt,
 * the last full-screen (alternate) frame the program left instead. */
static int load_snapshot(const struct screen_target *t, struct hold_snapshot *snap, bool last_alt) {
    if (!t->r.has_console) return no_screen(t);
    if (!state_ended(t->st)) {
        unsigned char *reply = NULL;
        size_t len = 0;
        const char *req = last_alt ? "peek alt=1" : "peek";
        int got = control_quiet(t, req, strlen(req), &reply, &len);
        int err = errno;
        if (got == 0) {
            int rc = 0;
            if (len >= 3 && !memcmp(reply, "ok\n", 3)) {
                if (hold_snapshot_parse(reply + 3, len - 3, snap) != 0) {
                    fprintf(stderr, "backmux: %s: malformed screen snapshot\n", t->label);
                    rc = 3;
                }
            } else {
                rc = reply_error(t, reply);
            }
            free(reply);
            return rc;
        }
        /* The console has closed: the program ended, or closed its terminal
         * and runs on. Either way its last screen is saved. Anything else is
         * a console we cannot reach: say so, then show what was saved. */
        if (err != ENOENT)
            fprintf(stderr, "backmux: cannot reach %s's console (%s); showing its last saved screen\n", t->label,
                    strerror(err));
        else {
            enum run_state st = current_state(t);
            if (st == STATE_RUNNING)
                fprintf(stderr, "backmux: note: %s closed its terminal; this is its last screen (the program is still running)\n",
                        t->label);
            else if (st == STATE_UNKNOWN)
                fprintf(stderr, "backmux: cannot reach %s's console; its process state is unknown; showing its last saved screen\n",
                        t->label);
        }
    }
    if (last_alt) {
        if (read_saved_frame(t, ".altscreen", snap) == 0) return 0;
        fprintf(stderr, "backmux: %s has no saved alternate-screen frame\n", t->label);
        return 5;
    }
    if (read_saved_screen(t, snap) == 0) {
        struct hold_snapshot alt;
        if (read_saved_frame(t, ".altscreen", &alt) == 0) {
            hold_snapshot_free(&alt);
            fprintf(stderr, "backmux: note: view %s's saved alternate-screen frame with backmux peek %s --last-alt\n",
                    t->label, t->label);
        }
        return 0;
    }
    enum run_state st = current_state(t);
    if (!state_ended(st))
        fprintf(stderr, "backmux: %s's console is not answering and left no saved screen - see 'backmux logs %s'\n", t->label,
                t->label);
    else
        fprintf(stderr, "backmux: %s has ended and left no saved screen - see 'backmux logs %s'\n", t->label, t->label);
    return st == STATE_UNKNOWN ? 3 : 5;
}

/* "300", "300ms", "2s", "1.5s", "1m" -> milliseconds; -1 on garbage. */
static long parse_duration_ms(const char *s) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno || v < 0) return -1;
    double mult = 1;
    if (!*end || !strcmp(end, "ms")) mult = 1;
    else if (!strcmp(end, "s")) mult = 1000;
    else if (!strcmp(end, "m")) mult = 60000;
    else return -1;
    double ms = v * mult;
    return ms > 86400000.0 ? -1 : (long)ms;
}

static int need_value(int i, int argc, const char *flag) {
    if (i + 1 >= argc) {
        fprintf(stderr, "backmux: error: %s needs a value\n", flag);
        return -1;
    }
    return 0;
}

/* "--flag=value" -> "--flag" "value", for the verb's own value flags only,
 * so both spellings work. Nothing after "--" and no flag's value is touched:
 * text, patterns, and pastes reach the verb as typed. Flags in OPTIONAL take
 * a value only sometimes (--idle, --received), so the word after one is
 * looked at as usual. Returns a new argv (caller frees the array only). */
static bool in_flag_list(const char *const *list, const char *a, size_t n) {
    for (; list && *list; list++)
        if (strlen(*list) == n && !strncmp(*list, a, n)) return true;
    return false;
}

static char **split_eq_flags(int argc, char **argv, int *out_argc, const char *const *values,
                             const char *const *optional) {
    char **out = calloc((size_t)argc * 2 + 1, sizeof(*out));
    int n = 0;
    if (!out) {
        *out_argc = argc;
        return NULL;
    }
    bool literal = false;
    for (int i = 0; i < argc; i++) {
        char *a = argv[i];
        if (literal || a[0] != '-' || a[1] != '-') {
            out[n++] = a;
            /* a short value flag (-o, -k): its value, as typed */
            if (!literal && a[0] == '-' && a[1] && in_flag_list(values, a, strlen(a)) && i + 1 < argc)
                out[n++] = argv[++i];
            continue;
        }
        if (!a[2]) {
            literal = true;
            out[n++] = a;
            continue;
        }
        char *eq = strchr(a, '=');
        size_t name_len = eq ? (size_t)(eq - a) : strlen(a);
        bool takes = in_flag_list(values, a, name_len), maybe = in_flag_list(optional, a, name_len);
        if (eq && (takes || maybe)) {
            *eq = '\0'; /* argv strings are writable */
            out[n++] = a;
            out[n++] = eq + 1;
        } else {
            out[n++] = a;
            if (takes && i + 1 < argc) out[n++] = argv[++i]; /* its value, as typed */
        }
    }
    *out_argc = n;
    return out;
}

static const char *const peek_value_flags[] = {"--image", "-o", "--scale", NULL};
static const char *const peek_optional_flags[] = {NULL};
static const char *const send_value_flags[] = {"-k", "--key", "--keys", "--text", "--paste", "--mouse",
                                               "--wait", "--timeout", NULL};
static const char *const send_optional_flags[] = {NULL};
static const char *const wait_value_flags[] = {"--timeout", "--match", "--regex", "--since", NULL};
static const char *const wait_optional_flags[] = {"--idle", NULL};
static const char *const msg_value_flags[] = {"--timeout", NULL};
static const char *const msg_optional_flags[] = {"--received", NULL};

/* ---- peek ----------------------------------------------------------------- */

static const char *mouse_mode_name(uint8_t m) {
    switch (m) {
    case HOLD_MOUSE_CLICK: return "click";
    case HOLD_MOUSE_DRAG: return "drag";
    case HOLD_MOUSE_MOVE: return "move";
    default: return "none";
    }
}

static void write_peek_json(const struct screen_target *t, const struct hold_snapshot *s, const char *image, FILE *f) {
    fprintf(f, "{\n  \"process\": \"%s\",\n  \"name\": ", t->r.id);
    if (t->r.has_name && t->r.name[0]) {
        fputc('"', f);
        hold_json_escape(f, t->r.name);
        fputc('"', f);
    } else {
        fputs("null", f);
    }
    fprintf(f, ",\n  \"running\": %s,\n  \"exit_code\": ", s->meta.running ? "true" : "false");
    if (s->meta.has_exit_code) fprintf(f, "%d", s->meta.exit_code);
    else fputs("null", f);
    fprintf(f, ",\n  \"rows\": %u,\n  \"cols\": %u,\n", s->rows, s->cols);
    fprintf(f, "  \"cursor\": {\"row\": %u, \"col\": %u, \"visible\": %s},\n", s->cursor_row, s->cursor_col,
            s->modes.cursor_visible ? "true" : "false");
    fputs("  \"title\": \"", f);
    hold_json_escape(f, s->title);
    fprintf(f, "\",\n  \"alt_screen\": %s,\n", s->modes.alt_screen ? "true" : "false");
    fprintf(f, "  \"mouse\": \"%s\",\n", mouse_mode_name(s->modes.mouse));
    fprintf(f, "  \"bracketed_paste\": %s,\n", s->modes.bracketed_paste ? "true" : "false");
    fprintf(f, "  \"cursor_keys\": \"%s\",\n", s->modes.app_cursor_keys ? "application" : "normal");
    fprintf(f, "  \"output_seq\": %" PRIu64 ",\n  \"idle_ms\": %u,\n", s->meta.output_seq, s->meta.idle_ms);
    fprintf(f, "  \"size_set_by\": \"%s\",\n", s->meta.size_by_agent ? "resize" : "terminal");
    if (s->meta.has_mouse) fprintf(f, "  \"last_mouse\": {\"row\": %u, \"col\": %u},\n", s->meta.mouse_row, s->meta.mouse_col);
    else fputs("  \"last_mouse\": null,\n", f);
    if (image) {
        fputs("  \"image\": \"", f);
        hold_json_escape(f, image);
        fputs("\",\n", f);
    }
    fputs("  \"lines\": [", f);
    size_t cap = (size_t)s->cols * 4 * HOLD_CELL_CHARS + 1;
    char *line = malloc(cap);
    for (int r = 0; line && r < s->rows; r++) {
        hold_snapshot_row_text(s, r, line, cap);
        fputs(r ? ",\n    \"" : "\n    \"", f);
        hold_json_escape(f, line);
        fputc('"', f);
    }
    free(line);
    fputs("\n  ]\n}\n", f);
}

static const char peek_usage[] =
    "usage: backmux peek <target> [--json] [--ruler] [--last-alt]\n"
    "                 [--image FILE [--no-ruler] [--cursor-ring] [--scale N]\n"
    "                               [--no-footer] [--no-mouse-marker] [--text]]\n";

static int peek_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL, *image = NULL;
    bool json = false, text_ruler = false, want_text = false, last_alt = false;
    struct hold_render_options ro;
    hold_render_options_init(&ro);
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json")) json = true;
        else if (!strcmp(a, "--text")) want_text = true;
        else if (!strcmp(a, "--last-alt")) last_alt = true;
        else if (!strcmp(a, "--ruler")) text_ruler = true, ro.ruler = true;
        else if (!strcmp(a, "--no-ruler")) ro.ruler = false;
        else if (!strcmp(a, "--cursor-ring")) ro.cursor_ring = true;
        else if (!strcmp(a, "--no-mouse-marker")) ro.mouse_marker = false;
        else if (!strcmp(a, "--no-footer")) ro.footer = false;
        else if (!strcmp(a, "--image") || !strcmp(a, "-o")) {
            if (need_value(i, argc, a) != 0) return 1;
            image = argv[++i];
        } else if (!strcmp(a, "--scale")) {
            if (need_value(i, argc, a) != 0) return 1;
            ro.scale = atoi(argv[++i]);
            if (ro.scale < 1 || ro.scale > 4) {
                fprintf(stderr, "backmux: error: --scale must be 1-4\n");
                return 1;
            }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            return HOLD_RC_SHOW_HELP;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "backmux: error: unknown flag '%s'\n%s", a, peek_usage);
            return 1;
        } else if (!token) token = a;
        else {
            fputs(peek_usage, stderr);
            return 1;
        }
    }
    if (!token) {
        fputs(peek_usage, stderr);
        return 1;
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "peek", token, &t);
    if (rc != 0) return rc;
    struct hold_snapshot snap;
    rc = load_snapshot(&t, &snap, last_alt);
    if (rc != 0) {
        close_screen_target(&t);
        return rc;
    }
    if (image) {
        FILE *f = !strcmp(image, "-") ? stdout : fopen(image, "wb");
        if (!f || hold_snapshot_write_png(&snap, &ro, f) != 0 || (f != stdout && fclose(f) != 0)) {
            fprintf(stderr, "backmux: failed to write image %s: %s\n", image, strerror(errno));
            rc = 3;
        }
    }
    if (rc == 0 && (!image || want_text || json) && !(image && !strcmp(image, "-"))) {
        if (json) write_peek_json(&t, &snap, image, stdout);
        else hold_snapshot_write_text(&snap, stdout, text_ruler);
        if (fflush(stdout) != 0) rc = 3;
    }
    hold_snapshot_free(&snap);
    close_screen_target(&t);
    return rc;
}

/* ---- send ----------------------------------------------------------------- */

struct reqbuf {
    char *p;
    size_t len, cap;
};

static int rb_add(struct reqbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int rb_str(struct reqbuf *b, const char *s) { return rb_add(b, s, strlen(s)); }

static int rb_hex(struct reqbuf *b, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char two[2] = {hx[(unsigned char)s[i] >> 4], hx[(unsigned char)s[i] & 15]};
        if (rb_add(b, two, 2) != 0) return -1;
    }
    return 0;
}

static char *slurp_stdin(size_t *n) {
    struct reqbuf b = {0};
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), stdin)) > 0)
        if (rb_add(&b, chunk, r) != 0) {
            free(b.p);
            return NULL;
        }
    *n = b.len;
    return b.p ? b.p : strdup("");
}

static int add_key(struct reqbuf *b, struct hold_screen *probe, const char *name) {
    if (hold_screen_send_key(probe, name) != 0) {
        fprintf(stderr, "backmux: error: unknown key '%s' (try Enter, Tab, Escape, Up, F5, C-c, M-x)\n", name);
        return -1;
    }
    hold_screen_discard_output(probe);
    return rb_str(b, "key ") || rb_str(b, name) || rb_str(b, "\n") ? -1 : 0;
}

static const char send_usage[] =
    "usage: backmux send <target> [TEXT]... [--text TEXT] [-k KEY]...\n"
    "                 [--keys \"K1 K2\"] [--enter] [--paste TEXT|-] [--stdin] [--seq]\n"
    "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--wait MS [--timeout MS]]\n";

static int run_wait(const struct screen_target *t, const char *args, bool print_exit, bool after_send);

static int send_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL;
    long wait_ms = -1, timeout_ms = 10000;
    bool print_seq = false, literal = false, any_op = false, mouse_op = false;
    struct reqbuf b = {0};
    struct hold_screen *probe = hold_screen_new(24, 80);
    int rc = 1;
    if (!probe || rb_str(&b, "send\n") != 0) goto out;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!token && !literal && (a[0] != '-' || !a[1])) {
            token = a;
            continue;
        }
        if (literal || a[0] != '-' || !a[1]) {
            if (!token) {
                token = a;
                continue;
            }
            if (rb_str(&b, "text ") || rb_hex(&b, a, strlen(a)) || rb_str(&b, "\n")) goto out;
            any_op = true;
        } else if (!strcmp(a, "--")) {
            literal = true;
        } else if (!strcmp(a, "-k") || !strcmp(a, "--key")) {
            if (need_value(i, argc, a) != 0 || add_key(&b, probe, argv[++i]) != 0) goto out;
            any_op = true;
        } else if (!strcmp(a, "--keys")) {
            if (need_value(i, argc, a) != 0) goto out;
            char *list = strdup(argv[++i]);
            if (!list) goto out;
            for (char *save = NULL, *k = strtok_r(list, " ,", &save); k; k = strtok_r(NULL, " ,", &save)) {
                if (add_key(&b, probe, k) != 0) {
                    free(list);
                    goto out;
                }
                any_op = true;
            }
            free(list);
        } else if (!strcmp(a, "--text")) {
            /* literal text that may itself start with a dash */
            if (need_value(i, argc, a) != 0) goto out;
            const char *v = argv[++i];
            if (rb_str(&b, "text ") || rb_hex(&b, v, strlen(v)) || rb_str(&b, "\n")) goto out;
            any_op = true;
        } else if (!strcmp(a, "--enter")) {
            if (add_key(&b, probe, "Enter") != 0) goto out;
            any_op = true;
        } else if (!strcmp(a, "--paste") || !strcmp(a, "--stdin")) {
            bool paste = a[2] == 'p';
            const char *v = NULL;
            if (paste) {
                if (need_value(i, argc, a) != 0) goto out;
                v = argv[++i];
            }
            size_t n = 0;
            char *data = (!paste || !strcmp(v, "-")) ? slurp_stdin(&n) : strdup(v);
            if (!data) goto out;
            if (data != v && paste && strcmp(v, "-")) n = strlen(data);
            int bad = rb_str(&b, paste ? "paste " : "text ") || rb_hex(&b, data, n) || rb_str(&b, "\n");
            free(data);
            if (bad) goto out;
            any_op = true;
        } else if (!strcmp(a, "--mouse")) {
            if (need_value(i, argc, a) != 0) goto out;
            char action[32];
            int col = -1, row = -1, button = 1;
            int got = sscanf(argv[++i], "%31[^,],%d,%d,%d", action, &col, &row, &button);
            if (got < 3 || col < 0 || row < 0) {
                fprintf(stderr, "backmux: error: --mouse requires ACTION,COL,ROW[,BUTTON] (for example, click,10,4)\n");
                goto out;
            }
            char line[96];
            snprintf(line, sizeof(line), "mouse %s %d %d %d\n", action, button, row, col);
            if (rb_str(&b, line) != 0) goto out;
            any_op = mouse_op = true;
        } else if (!strcmp(a, "--wait") || !strcmp(a, "--timeout")) {
            if (need_value(i, argc, a) != 0) goto out;
            long v = parse_duration_ms(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "backmux: error: bad duration '%s'\n", argv[i]);
                goto out;
            }
            if (a[2] == 'w') wait_ms = v;
            else timeout_ms = v;
        } else if (!strcmp(a, "--seq")) {
            print_seq = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            rc = HOLD_RC_SHOW_HELP;
            goto out;
        } else {
            fprintf(stderr, "backmux: error: unknown flag '%s' (send text starting with - via --text)\n%s", a, send_usage);
            goto out;
        }
    }
    if (!token || !any_op) {
        fputs(send_usage, stderr);
        goto out;
    }
    struct screen_target t;
    rc = open_screen_target(inv, user_store, system_store, "send", token, &t);
    if (rc != 0) goto out;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (state_ended(t.st)) {
        fprintf(stderr, "backmux: %s has ended - see 'backmux peek %s'\n", t.label, t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = control(&t, b.p, b.len, &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) {
            rc = reply_error(&t, reply);
        } else if (rc == 0) {
            const char *seq = strstr((char *)reply, "seq=");
            unsigned long long sv = seq ? strtoull(seq + 4, NULL, 10) : 0;
            bool mouse_off = mouse_op && strstr((char *)reply, "mouse=off");
            if (mouse_off)
                fprintf(stderr, "backmux: %s has mouse reporting off; the mouse event was not delivered\n", t.label);
            if (mouse_op && strstr((char *)reply, "mouse=clamped"))
                fprintf(stderr, "backmux: note: %s uses legacy mouse encoding; "
                                "coordinates above 222 were clamped to 222\n", t.label);
            if (print_seq) printf("%llu\n", sv);
            if (wait_ms >= 0) {
                char args[160];
                snprintf(args, sizeof(args), "wait idle=%ld since=%llu timeout=%ld", wait_ms ? wait_ms : 1, sv, timeout_ms);
                rc = run_wait(&t, args, false, true);
                if (rc == WAIT_CONSOLE_CLOSED) {
                    enum run_state st = current_state(&t);
                    if (st == STATE_UNKNOWN) {
                        fprintf(stderr, "backmux: input sent; cannot reach %s's console now; its process state is unknown\n",
                                t.label);
                        rc = 3;
                    } else {
                        fprintf(stderr, "backmux: input sent; %s then %s\n", t.label,
                                st == STATE_RUNNING ? "closed its terminal" : "ended");
                        rc = WAIT_EXIT_ENDED;
                    }
                }
            }
            /* An undelivered mouse event is a failed step for a script. */
            if (mouse_off && rc == 0) rc = SEND_EXIT_MOUSE_OFF;
        }
        free(reply);
    }
    close_screen_target(&t);
out:
    free(b.p);
    hold_screen_free(probe);
    return rc;
}

/* ---- wait ----------------------------------------------------------------- */

/* Map broker replies to exit statuses. After send --wait, diagnostics
 * distinguish successful input delivery from the subsequent wait result. */
static int run_wait(const struct screen_target *t, const char *args, bool print_exit, bool after_send) {
    unsigned char *reply = NULL;
    size_t len = 0;
    /* A closed console (the program ended, or closed its terminal) is not an
     * error here: the caller decides from the record or the last screen. */
    if (control_quiet(t, args, strlen(args), &reply, &len) != 0) {
        /* A broker that went away while we wrote the request breaks the
         * connection instead (EPIPE or ECONNRESET; the FIFO transport is
         * slow enough to show it after a send --wait ended the program). */
        if (errno == ENOENT || errno == EPIPE || errno == ECONNRESET) return WAIT_CONSOLE_CLOSED;
        fprintf(stderr, "backmux: cannot reach %s's console: %s\n", t->label, strerror(errno));
        return 3;
    }
    int rc = 0;
    const char *r = (const char *)reply;
    /* A broker shutting down answers a request it had not yet read with this
     * instead of closing: the console closed, as above. Why: a send --wait
     * whose wait arrived late (the FIFO transport is slower) said only "the
     * process has ended", exit 1, where it means input sent, then ended (8). */
    if (!strncmp(r, "error the process has ended", 24)) {
        free(reply);
        return WAIT_CONSOLE_CLOSED;
    }
    const char *ec = strstr(r, "exit_code=");
    int code = ec ? atoi(ec + 10) : 0;
    if (!strncmp(r, "ok", 2)) {
        if (print_exit && strstr(r, "reason=exit")) printf("%d\n", code);
        rc = 0;
    } else if (!strncmp(r, "timeout", 7)) {
        const char *to = after_send ? strstr(args, "timeout=") : NULL;
        if (to)
            fprintf(stderr, "backmux: input sent; %s did not meet the output-idle condition within %ld ms\n", t->label, atol(to + 8));
        else if (after_send)
            fprintf(stderr, "backmux: input sent; %s did not meet the output-idle condition before the timeout\n", t->label);
        else
            fprintf(stderr, "backmux: %s: wait timed out\n", t->label);
        rc = WAIT_EXIT_TIMEOUT;
    } else if (!strncmp(r, "ended", 5)) {
        if (after_send)
            fprintf(stderr, "backmux: input sent; %s ended (exit code %d) before meeting the output-idle condition\n", t->label, code);
        else
            fprintf(stderr, "backmux: %s ended (exit code %d) before the wait condition held\n", t->label, code);
        rc = WAIT_EXIT_ENDED;
    } else {
        rc = reply_error(t, reply);
    }
    free(reply);
    return rc;
}

/* docker wait for any process (no console needed): poll the record. */
static int wait_record_exit(const struct screen_target *t, long timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct timespec unstamped_since = {0, 0};
    for (;;) {
        struct hold_run_record cur;
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(t->target.store.record_dir, t->r.id, &cur, path, sizeof(path)) != 0) {
            /* --rm removed it as it ended: report the exit it left behind. */
            int code;
            if (hold_auto_removed_exit(&t->target.store, t->r.id, &code)) {
                printf("%d\n", code);
                return 0;
            }
            fprintf(stderr, "backmux: %s was removed while waiting\n", t->label);
            return 5;
        }
        char boot[128];
        enum run_state st = hold_eval_state(&cur, hold_boot_id_or_null(boot));
        bool has = cur.has_exit_code;
        int code = cur.exit_code;
        bool records = hold_call_records_exit(&cur);
        hold_free_run_record(&cur);
        if (st != STATE_RUNNING && (has || st == STATE_STALE || st == STATE_FAILED)) {
            printf("%d\n", has ? code : -1);
            return 0;
        }
        /* A plain -d process leaves nothing behind to record its exit code. */
        if (st == STATE_EXITED && !records) {
            fprintf(stderr, "backmux: %s has ended; a plain -d process records no exit code (launch it with -dw to "
                            "record one)\n", t->label);
            printf("-1\n");
            return 0;
        }
        /* A confirmed exit can precede its stamp. An unknown state supplies
         * no such evidence: keep waiting for an exit or the requested timeout. */
        if (st == STATE_EXITED && !has) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (unstamped_since.tv_sec == 0) unstamped_since = now;
            long gone_ms = (now.tv_sec - unstamped_since.tv_sec) * 1000 + (now.tv_nsec - unstamped_since.tv_nsec) / 1000000;
            if (gone_ms >= 2000) {
                fprintf(stderr, "backmux: %s has ended; its exit code was not recorded within 2 seconds of detecting the exit\n", t->label);
                printf("-1\n");
                return 0;
            }
        } else {
            unstamped_since = (struct timespec){0, 0};
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            fprintf(stderr, "backmux: %s: wait timed out\n", t->label);
            return WAIT_EXIT_TIMEOUT;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
    }
}

/* Does the saved final screen show TEXT (or match RE)? */
static bool saved_screen_matches(const struct screen_target *t, const char *match, bool regex) {
    struct hold_snapshot snap;
    if (read_saved_screen(t, &snap) != 0) return false;
    bool hit = false;
    char *text = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&text, &n);
    if (f) {
        hold_snapshot_write_text(&snap, f, false);
        fclose(f);
        if (!regex && text && strstr(text, match)) hit = true;
        if (regex && text) {
            regex_t re;
            if (regcomp(&re, match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE) == 0) {
                hit = regexec(&re, text, 0, NULL, 0) == 0;
                regfree(&re);
            }
        }
    }
    free(text);
    hold_snapshot_free(&snap);
    return hit;
}

static const char wait_usage[] =
    "usage: backmux wait <target> [--idle [MS]] [--match TEXT | --regex RE]\n"
    "                 [--since SEQ] [--exit] [--timeout MS]\n";

static int wait_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL, *match = NULL, *since = NULL;
    bool regex = false, want_exit = false;
    long idle = -1, timeout = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--idle") &&
            (i + 1 >= argc || argv[i + 1][0] == '-' || (!token && parse_duration_ms(argv[i + 1]) < 0))) {
            /* Bare --idle (next is a flag, the target, or nothing): the
             * documented 500 ms settle time. Once the target is known, the
             * next word must be a duration, so a typo still says so. */
            idle = WAIT_IDLE_DEFAULT_MS;
        } else if (!strcmp(a, "--idle") || !strcmp(a, "--timeout")) {
            if (need_value(i, argc, a) != 0) return 1;
            long v = parse_duration_ms(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "backmux: error: bad duration '%s'\n", argv[i]);
                return 1;
            }
            if (a[2] == 'i') idle = v;
            else timeout = v;
        } else if (!strcmp(a, "--match") || !strcmp(a, "--regex")) {
            if (need_value(i, argc, a) != 0) return 1;
            match = argv[++i];
            regex = a[2] == 'r';
        } else if (!strcmp(a, "--since")) {
            if (need_value(i, argc, a) != 0) return 1;
            since = argv[++i];
            if (strspn(since, "0123456789") != strlen(since) || !*since) {
                fprintf(stderr, "backmux: error: --since requires the sequence number printed by backmux send --seq\n");
                return 1;
            }
        } else if (!strcmp(a, "--exit")) {
            want_exit = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            return HOLD_RC_SHOW_HELP;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "backmux: error: unknown flag '%s'\n%s", a, wait_usage);
            return 1;
        } else if (!token) token = a;
        else {
            fputs(wait_usage, stderr);
            return 1;
        }
    }
    if (!token) {
        fputs(wait_usage, stderr);
        return 1;
    }
    bool screen_cond = idle >= 0 || match;
    if (!screen_cond) want_exit = true;
    /* Screen conditions default to a 30-second timeout. Exit-only waits
     * have no default timeout. */
    if (timeout < 0) timeout = screen_cond ? 30000 : 0;

    /* docker run --rm + docker wait: an --rm process that already ended and
     * was removed still reports how it exited (for a minute). */
    if (!screen_cond && !hold_token_resolves(inv, user_store, system_store, "wait", token)) {
        int code;
        if (hold_auto_removed_exit(user_store, token, &code) ||
            (system_store && hold_auto_removed_exit(system_store, token, &code))) {
            printf("%d\n", code);
            return 0;
        }
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "wait", token, &t);
    if (rc != 0) return rc;
    if (!screen_cond) {
        rc = (t.r.has_console && !state_ended(t.st)) ? -1 : wait_record_exit(&t, timeout);
        if (rc != -1) {
            /* --rm: removed before we return, so the name is free again. */
            if (rc == 0) hold_auto_remove_settle(&t.target.store, t.r.id, 2000);
            close_screen_target(&t);
            return rc;
        }
    } else if (!t.r.has_console) {
        rc = no_screen(&t);
        close_screen_target(&t);
        return rc;
    }
    if (state_ended(t.st)) {
        /* Ended: a match may still hold on the final frame. */
        rc = match && saved_screen_matches(&t, match, regex) ? 0 : WAIT_EXIT_ENDED;
        if (rc != 0) fprintf(stderr, "backmux: %s has already ended\n", t.label);
        close_screen_target(&t);
        return rc;
    }
    struct reqbuf b = {0};
    char num[64];
    rc = rb_str(&b, "wait");
    if (idle >= 0) {
        snprintf(num, sizeof(num), " idle=%ld", idle ? idle : 1);
        rc |= rb_str(&b, num);
    }
    if (since) rc |= rb_str(&b, " since=") | rb_str(&b, since);
    if (match) {
        rc |= rb_str(&b, regex ? " regex=1 match=" : " match=");
        rc |= rb_hex(&b, match, strlen(match));
    }
    if (want_exit) rc |= rb_str(&b, " exit=1");
    snprintf(num, sizeof(num), " timeout=%ld", timeout);
    rc |= rb_str(&b, num);
    rc = rc ? 1 : run_wait(&t, b.p, !screen_cond, false);
    free(b.p);
    if (rc == WAIT_CONSOLE_CLOSED) {
        if (!screen_cond) {
            /* The record (or an --rm process's exit note) says when it ends. */
            rc = wait_record_exit(&t, timeout);
        } else if (match && saved_screen_matches(&t, match, regex)) {
            rc = 0; /* the last screen is final, and it shows the text */
        } else {
            enum run_state st = current_state(&t);
            if (st == STATE_RUNNING)
                fprintf(stderr, "backmux: %s closed its terminal before the condition held (the program is still running)\n",
                        t.label);
            else if (st == STATE_UNKNOWN)
                fprintf(stderr, "backmux: cannot reach %s's console before the condition held; its process state is unknown\n",
                        t.label);
            else
                fprintf(stderr, "backmux: %s ended before the condition held\n", t.label);
            rc = st == STATE_UNKNOWN ? 3 : WAIT_EXIT_ENDED;
        }
    } else if (!screen_cond && rc == 3) {
        /* The broker can vanish with its process before answering (an --rm
         * process removed as it ended): the record, or its exit note, says. */
        rc = wait_record_exit(&t, timeout);
    }
    if (!screen_cond && rc == 0) hold_auto_remove_settle(&t.target.store, t.r.id, 2000);
    close_screen_target(&t);
    return rc;
}

/* ---- resize --------------------------------------------------------------- */

static const char resize_usage[] = "usage: backmux resize <target> <cols> <rows>   (or <cols>x<rows>)\n";

int hold_cmd_resize_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           int argc,
                           char **argv) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return HOLD_RC_SHOW_HELP;
    int cols = 0, rows = 0;
    char why[160];
    if (argc < 2 || argc > 3) {
        fputs(resize_usage, stderr);
        return 1;
    }
    /* The one size grammar (--size and the saved detach-size share it). */
    int used = hold_parse_term_size(argv[1], argc == 3 ? argv[2] : NULL, &cols, &rows, why, sizeof(why));
    if (used != argc - 1) {
        if (used > 0) fputs(resize_usage, stderr);
        else fprintf(stderr, "backmux: error: %s\n", why);
        return 1;
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "resize", argv[0], &t);
    if (rc != 0) return rc;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (state_ended(t.st)) {
        fprintf(stderr, "backmux: %s has ended\n", t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        char req[64];
        snprintf(req, sizeof(req), "resize rows=%d cols=%d", rows, cols);
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = control(&t, req, strlen(req), &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) rc = reply_error(&t, reply);
        free(reply);
    }
    close_screen_target(&t);
    return rc;
}

/* ---- msg ------------------------------------------------------------------ */

static const char msg_usage[] =
    "usage: backmux msg <target> TEXT... [--stdin] [--no-enter] [-- TEXT...]\n"
    "                   [--wait-reply [--timeout MS]]\n"
    "       backmux msg [<target>] --received [ID]\n";

/* Message delivery to a managed process. The receiving broker selects raw
 * input or an agent header from the target's agent setting and resolves
 * BACKMUX_ID to the sender's current process name. */
static bool is_msg_id(const char *s) {
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return false;
    return true;
}

/* backmux msg [TARGET] --received [ID]: a process's message history (by default
 * the process we run in), from its <id>.msgs log: one line per message, or with
 * ID that message's sender and full text. */
static int msg_received(const struct hold_invocation *inv, const struct hold_store *user_store,
                        const struct hold_store *system_store, const char *token, const char *want) {
    if (!token || !*token) token = getenv("BACKMUX_ID");
    if (!token || !*token) {
        fprintf(stderr, "backmux: msg --received reads a process's messages: run it inside a backmux call, or name one "
                        "(backmux msg CALL --received)\n");
        return 1;
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "msg", token, &t);
    if (rc != 0) return rc;
    char path[HOLD_PATH_MAX];
    size_t ll = t.r.has_log ? strlen(t.r.log_path) : 0;
    /* The log only grows, so read it a line at a time rather than whole. */
    FILE *mf = NULL;
    if (ll > 4 && !strcmp(t.r.log_path + ll - 4, ".log") &&
        hold_checked_snprintf(path, sizeof(path), "%.*s.msgs", (int)(ll - 4), t.r.log_path) == 0) {
        int fd = hold_open_owned_file_no_symlink(path, NULL);
        if (fd >= 0 && !(mf = fdopen(fd, "r"))) {
            int saved = errno;
            close(fd);
            errno = saved;
        }
        if (!mf && errno != ENOENT) {
            fprintf(stderr, "backmux: %s: cannot read its message log %s: %s\n", t.label, path, strerror(errno));
            close_screen_target(&t);
            return 1;
        }
    }
    bool found = false;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t got;
    while (mf && (got = getline(&line, &line_cap, mf)) >= 0) {
        if (got > 0 && line[got - 1] == '\n') line[got - 1] = '\0';
        char id[8], from[ALIAS_MAX_LEN + ID_STR_LEN + 1];
        long long when;
        int from_call, off = 0; /* (an older backmux's verified flag: read, not shown) */
        if (sscanf(line, "%7s %lld %d %128s %n", id, &when, &from_call, from, &off) == 4 && off > 0 &&
            (!want || !strcmp(id, want))) {
            /* the text, hex-decoded in place */
            char *hex = line + off;
            size_t n = strlen(hex) / 2;
            for (size_t k = 0; k < n; k++) {
                unsigned v = 0;
                (void)sscanf(hex + 2 * k, "%2x", &v);
                hex[k] = (char)v;
            }
            hex[n] = '\0';
            time_t tt = (time_t)when;
            struct tm tmv;
            char at[32] = "?";
            if (localtime_r(&tt, &tmv)) strftime(at, sizeof(at), "%Y-%m-%d %H:%M:%S", &tmv);
            const char *who = strcmp(from, "-") ? from : "(sender not recorded)";
            (void)from_call;
            if (want) {
                printf("id:   %s\nfrom: %s\nat:   %s\n\n%s\n", id, who, at, hex);
            } else {
                for (char *q = hex; *q; q++)
                    if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
                printf("%s  %s  %-20s %.60s%s\n", id, at, strcmp(from, "-") ? from : "-", hex,
                       strlen(hex) > 60 ? "..." : "");
            }
            found = true;
        }
    }
    free(line);
    if (mf && ferror(mf)) {
        fprintf(stderr, "backmux: %s: error reading its message log %s\n", t.label, path);
        rc = 1;
    }
    if (mf) fclose(mf);
    if (want && !found) {
        fprintf(stderr, "backmux: %s: no message %s in its message history\n", t.label, want);
        rc = 1;
    }
    close_screen_target(&t);
    return rc;
}

static int msg_action_impl(const struct hold_invocation *inv,
                        const struct hold_store *user_store,
                        const struct hold_store *system_store,
                        int argc,
                        char **argv) {
    const char *token = NULL, *recv_id = NULL;
    bool submit = true, from_stdin = false, received = false, wait_reply = false;
    long reply_timeout_ms = 0;
    struct reqbuf text = {0};
    int rc = 1;
    bool literal = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (literal || a[0] != '-' || !a[1]) {
            if (!token) token = a;
            else if ((text.len && rb_str(&text, " ")) || rb_str(&text, a)) goto out;
        } else if (!strcmp(a, "--")) {
            literal = true; /* the rest is the message, even words that start with a dash */
        } else if (!strcmp(a, "--received")) {
            received = true;
            if (i + 1 < argc && is_msg_id(argv[i + 1])) recv_id = argv[++i];
        } else if (!strcmp(a, "--no-enter")) {
            submit = false;
        } else if (!strcmp(a, "--wait-reply")) {
            wait_reply = true;
        } else if (!strcmp(a, "--timeout")) {
            const char *v = i + 1 < argc ? argv[++i] : NULL;
            if (hold_parse_inbox_timeout_ms(v, &reply_timeout_ms) != 0) {
                fprintf(stderr, "backmux: error: --timeout takes milliseconds (0..%ld; 0 = never)\n%s",
                        LONG_MAX, msg_usage);
                goto out;
            }
        } else if (!strcmp(a, "--stdin")) {
            from_stdin = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            rc = HOLD_RC_SHOW_HELP;
            goto out;
        } else {
            fprintf(stderr, "backmux: error: unknown flag '%s'\n%s", a, msg_usage);
            goto out;
        }
    }
    if (received) {
        if (text.len || from_stdin) {
            fputs(msg_usage, stderr);
            goto out;
        }
        rc = msg_received(inv, user_store, system_store, token, recv_id);
        goto out;
    }
    if (from_stdin) {
        size_t n = 0;
        char *in = slurp_stdin(&n);
        if (!in) goto out;
        while (n > 0 && (in[n - 1] == '\n' || in[n - 1] == '\r')) n--;
        int bad = (text.len && rb_str(&text, " ")) || rb_add(&text, in, n);
        free(in);
        if (bad) goto out;
    }
    if (!token || text.len == 0) {
        fputs(msg_usage, stderr);
        goto out;
    }
    /* A sender outside any process answers to a name of its own, and replies to
     * it wait in an inbox (runtime/inbox.c): the receiver sees an ordinary
     * header either way. */
    const char *self_id = getenv("BACKMUX_ID"), *self_name = getenv("BACKMUX_NAME");
    bool outside = !(self_id && *self_id) && !(self_name && *self_name);
    char opname[ALIAS_MAX_LEN + 1] = "";
    bool made = false;
    if (outside && hold_operator_name(user_store, true, opname, sizeof(opname), &made) != 0) opname[0] = '\0';
    if (wait_reply && !outside) {
        fprintf(stderr, "backmux: --wait-reply requires a sender outside a managed process.\n"
                        "Inside a managed process, use backmux msg --received to read received-message history.\n");
        rc = HOLD_EXIT_USAGE;
        goto out;
    }
    if (wait_reply && !opname[0]) {
        fprintf(stderr, "backmux: cannot make an inbox for the reply: %s\n", strerror(errno));
        goto out;
    }
    off_t reply_from = opname[0] ? hold_inbox_size(user_store, opname) : 0;
    if (hold_valid_alias(token) && hold_inbox_live(user_store, token)) {
        /* To an operator: no broker, straight into its inbox. From a process,
         * its name as the record has it now: BACKMUX_NAME in a running
         * program is the name it started with, and a rename (backmux set
         * NAME name=...) would leave the reply going to a name that no
         * longer answers. */
        char current[ALIAS_MAX_LEN + 1] = "";
        if (self_id && hold_valid_id(self_id)) {
            struct hold_run_record me;
            char mp[HOLD_PATH_MAX];
            if (hold_load_record_by_id(user_store->record_dir, self_id, &me, mp, sizeof(mp)) == 0) {
                if (me.has_name && me.name[0]) snprintf(current, sizeof(current), "%s", me.name);
                hold_free_run_record(&me);
            }
        }
        const char *from = current[0] ? current : self_name && *self_name ? self_name : opname[0] ? opname : "outside backmux";
        if (hold_inbox_deliver(user_store, token, from, text.p, text.len) != 0) {
            fprintf(stderr, "backmux: cannot deliver to %s's inbox: %s\n", token, strerror(errno));
            goto out;
        }
        rc = 0;
        if (made) hold_operator_first_note(opname);
        if (wait_reply) rc = hold_inbox_wait(user_store, opname, reply_from, false, reply_timeout_ms);
        goto out;
    }
    struct screen_target t;
    rc = open_screen_target(inv, user_store, system_store, "msg", token, &t);
    if (rc != 0) goto out;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (state_ended(t.st)) {
        fprintf(stderr, "backmux: %s has ended\n", t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        struct reqbuf req = {0};
        char num[96];
        snprintf(num, sizeof(num), "msg%s", submit ? "" : " submit=0");
        int bad = rb_str(&req, num);
        /* The exact id names one process; the receiver shows its name. An
         * operator sends its inbox name, marked op=1. */
        const char *from = getenv("BACKMUX_ID");
        if (!from || !*from) from = getenv("BACKMUX_NAME");
        if (from && *from) bad |= rb_str(&req, " from=") | rb_hex(&req, from, strlen(from));
        else if (opname[0]) bad |= rb_str(&req, " op=1 from=") | rb_hex(&req, opname, strlen(opname));
        bad |= rb_str(&req, "\n") | rb_hex(&req, text.p, text.len);
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = bad ? 1 : control(&t, req.p, req.len, &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) rc = reply_error(&t, reply);
        if (rc == 0) {
            /* The delivered message's id, on stdout (machine data): what msg
             * --received looks it up by in the receiver's history. */
            const char *idp = strstr((char *)reply, " id=");
            if (idp) {
                idp += 4;
                size_t il = strcspn(idp, " \n");
                if (il > 0 && il < 16) printf("%.*s\n", (int)il, idp);
            }
            fflush(stdout);
            if (made) hold_operator_first_note(opname);
        }
        free(reply);
        free(req.p);
        if (rc == 0 && wait_reply) rc = hold_inbox_wait(user_store, opname, reply_from, false, reply_timeout_ms);
    }
    close_screen_target(&t);
out:
    free(text.p);
    return rc;
}

/* ---- entry points: accept --flag=value as well as --flag value ------------ */

#define SCREEN_VERB_ENTRY(verb)                                                              \
    int hold_cmd_##verb##_action(const struct hold_invocation *inv,                          \
                                 const struct hold_store *user_store,                        \
                                 const struct hold_store *system_store, int argc, char **argv) { \
        int n = 0;                                                                           \
        char **split = split_eq_flags(argc, argv, &n, verb##_value_flags, verb##_optional_flags); \
        int rc = verb##_action_impl(inv, user_store, system_store, split ? n : argc,         \
                                    split ? split : argv);                                   \
        free(split);                                                                         \
        return rc;                                                                           \
    }

SCREEN_VERB_ENTRY(peek)
SCREEN_VERB_ENTRY(send)
SCREEN_VERB_ENTRY(wait)
SCREEN_VERB_ENTRY(msg)
