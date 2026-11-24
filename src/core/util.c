#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"

void hold_sig_note(const struct hold_invocation *inv, const char *fmt, ...) {
    if (inv && inv->quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

_Noreturn void hold_die_errno(const char *msg) {
    int e = errno;
    fprintf(stderr, "%s: %s\n", msg, strerror(e));
    exit(1);
}

/* Truncation is ENAMETOOLONG, never a silently shortened path. */
int hold_checked_snprintf(char *dst, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

bool hold_has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + (sl - sufl), suffix) == 0;
}

int hold_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Detach a daemonizing process from the invoking terminal's stdio. */
void hold_close_stdio_to_devnull(void) {
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}

static int put_ch(char *dst, size_t n, size_t *off, char c) {
    if (*off + 1 >= n) return -1;
    dst[(*off)++] = c;
    dst[*off] = '\0';
    return 0;
}

static bool cmd_arg_needs_quotes(const char *arg) {
    if (!*arg) return true;
    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        if (isspace(*p) || strchr("'\"\\$`!*?[]{}()<>|&;#", (int)*p)) return true;
    }
    return false;
}

/* Appends arg shell-quoted (single quotes, '\'' for embedded quotes) when it
 * needs it, verbatim when it does not: the COMMAND column formatter. */
static int append_cmd_human(char *dst, size_t n, size_t *off, const char *arg) {
    if (!arg) arg = "";
    if (!cmd_arg_needs_quotes(arg)) {
        for (; *arg; arg++) {
            if (put_ch(dst, n, off, *arg) != 0) return -1;
        }
        return 0;
    }
    if (put_ch(dst, n, off, '\'') != 0) return -1;
    for (; *arg; arg++) {
        if (*arg == '\'') {
            for (const char *sq = "'\\''"; *sq; sq++) {
                if (put_ch(dst, n, off, *sq) != 0) return -1;
            }
        } else if (put_ch(dst, n, off, *arg) != 0) {
            return -1;
        }
    }
    return put_ch(dst, n, off, '\'');
}

int hold_format_argv_human(char *dst, size_t n, int argc, char **argv) {
    if (!dst || n == 0 || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    size_t off = 0;
    dst[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0 && put_ch(dst, n, &off, ' ') != 0) return -1;
        if (append_cmd_human(dst, n, &off, argv[i]) != 0) return -1;
    }
    return 0;
}

/* Reads the exec errno handshake: EOF = exec succeeded (returns 0), a full
 * int = child's errno (returns 1), anything else is a transport failure. */
int hold_read_exec_handshake(int fd, int *child_errno) {
    unsigned char *p = (unsigned char *)child_errno;
    size_t got = 0;
    *child_errno = 0;
    while (got < sizeof(*child_errno)) {
        ssize_t n = read(fd, p + got, sizeof(*child_errno) - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (got == 0) return 0;
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

void hold_format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n) {
    time_t sec = (time_t)(unix_ns / 1000000000LL);
    struct tm tm_utc;
    if (!gmtime_r(&sec, &tm_utc) || strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        snprintf(out, n, "-");
    }
}

/* Docker's go-units HumanDuration, character for character: the phrasing the
 * call table borrows for CREATED ("2 minutes") and STATUS ("Up 2 minutes",
 * "Exited (0) 2 days ago"). The caller appends " ago" where the reference is
 * a past instant. */
void hold_format_duration_human(int64_t seconds, char *out, size_t n) {
    if (seconds < 0) seconds = 0;
    if (seconds < 1) {
        snprintf(out, n, "Less than a second");
        return;
    }
    if (seconds < 60) {
        snprintf(out, n, "%" PRId64 " second%s", seconds, seconds == 1 ? "" : "s");
        return;
    }
    int64_t minutes = seconds / 60;
    if (minutes == 1) {
        snprintf(out, n, "About a minute");
        return;
    }
    if (minutes < 46) {
        snprintf(out, n, "%" PRId64 " minutes", minutes);
        return;
    }
    /* Docker rounds hours to the nearest whole: int(d.Hours() + 0.5). */
    int64_t hours = (seconds + 1800) / 3600;
    if (hours == 1) {
        snprintf(out, n, "About an hour");
    } else if (hours < 48) {
        snprintf(out, n, "%" PRId64 " hours", hours);
    } else if (hours < 24 * 7 * 2) {
        snprintf(out, n, "%" PRId64 " days", hours / 24);
    } else if (hours < 24 * 30 * 2) {
        snprintf(out, n, "%" PRId64 " weeks", hours / 24 / 7);
    } else if (hours < 24 * 365 * 2) {
        snprintf(out, n, "%" PRId64 " months", hours / 24 / 30);
    } else {
        snprintf(out, n, "%" PRId64 " years", hours / 24 / 365);
    }
}

/* Parse an RFC3339 UTC instant ("2026-07-03T01:23:45Z", fractional seconds and
 * the trailing Z ignored) into unix nanoseconds. Returns false on any garbage
 * or the zero-value placeholder Backmux writes for records without a timestamp. */
bool hold_parse_rfc3339_utc_to_ns(const char *s, int64_t *out_ns) {
    if (!s || !*s) return false;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) return false;
    time_t t = timegm(&tm);
    if (t == (time_t)-1 || t <= 0) return false;
    if (out_ns) *out_ns = (int64_t)t * 1000000000LL;
    return true;
}

const char *hold_run_id_display(const char *id, char out[ID_DISPLAY_HEX_LEN + 1]) {
    if (!out) return "";
    if (!id) {
        out[0] = '\0';
        return out;
    }
    size_t n = strlen(id);
    if (n > ID_DISPLAY_HEX_LEN) n = ID_DISPLAY_HEX_LEN;
    memcpy(out, id, n);
    out[n] = '\0';
    return out;
}

int hold_copy_argv(char ***out, int argc, char **argv) {
    *out = NULL;
    if (argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    char **copy = calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) return -1;
    for (int i = 0; i < argc; i++) {
        copy[i] = strdup(argv[i]);
        if (!copy[i]) {
            hold_free_argv_alloc(copy, i);
            return -1;
        }
    }
    copy[argc] = NULL;
    *out = copy;
    return 0;
}

/* ---- terminal sizes ------------------------------------------------------ */

bool hold_term_size_valid(long cols, long rows) {
    return cols >= 1 && rows >= 1 && cols <= HOLD_TERM_MAX_DIM && rows <= HOLD_TERM_MAX_DIM;
}

/* Digits only (no sign, no blanks); -1 when s[0..n) is not such a number.
 * Anything too long to matter reads as out of range rather than wrapping. */
static long size_word(const char *s, size_t n) {
    if (n == 0) return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        if (v <= HOLD_TERM_MAX_DIM) v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Meant as a number: a digit, or a sign before one ("-5" is a bad size, not
 * the next flag). */
static bool size_word_like(const char *s) {
    if (!s) return false;
    if (*s == '-' || *s == '+') s++;
    return isdigit((unsigned char)*s) != 0;
}

int hold_parse_term_size(const char *first, const char *second, int *cols, int *rows, char *why, size_t why_n) {
    long c = -1, r = -1;
    int used = 0;
    const char *x = first ? strpbrk(first, "xX") : NULL;
    if (x) {
        c = size_word(first, (size_t)(x - first));
        r = size_word(x + 1, strlen(x + 1));
        used = 1;
    } else if (size_word_like(first) && size_word_like(second)) {
        c = size_word(first, strlen(first));
        r = size_word(second, strlen(second));
        used = 2;
    }
    if (used == 0) {
        snprintf(why, why_n, "a size is COLS ROWS in character cells (for example 120 40)");
        return -1;
    }
    if (!hold_term_size_valid(c, r)) {
        snprintf(why, why_n, "invalid size '%s%s%s': columns and rows are whole numbers from 1 to %d", first,
                 used == 2 ? " " : "", used == 2 ? second : "", HOLD_TERM_MAX_DIM);
        return -1;
    }
    *cols = (int)c;
    *rows = (int)r;
    return used;
}

/* Message text (backmux msg, and an operator inbox) is text from a sender
 * backmux does not trust: keep newlines and
 * tabs, drop every other control - C0 bytes, DEL, and C1 (U+0080-U+009F) -
 * so it can never type Escape into the receiver, submit early with a carriage
 * return, end a bracketed paste, or drive the terminal (titles, clears).
 * An escape sequence goes whole, not just its ESC: "\e[31mRED" arrives as
 * "RED". Why: dropping only the control byte left "[31mRED", noise in the
 * receiver's prompt that no sender meant as text. */

/* Where the sequence that starts at t[i] ends (one past it). intro is the
 * byte that selects the kind: ESC's next byte, or the C1 code (0x80-0x9f). */
static size_t skip_sequence(const char *t, size_t n, size_t i, unsigned char intro) {
    unsigned char kind = intro >= 0x80 ? (unsigned char)(intro - 0x40) : intro; /* C1 0x9b == ESC [ */
    if (kind == '[') { /* CSI: parameters and intermediates, then one final byte */
        while (i < n && (unsigned char)t[i] >= 0x20 && (unsigned char)t[i] <= 0x3f) i++;
        while (i < n && (unsigned char)t[i] >= 0x20 && (unsigned char)t[i] <= 0x2f) i++;
        if (i < n && (unsigned char)t[i] >= 0x40 && (unsigned char)t[i] <= 0x7e) i++;
        return i;
    }
    if (kind == ']' || kind == 'P' || kind == 'X' || kind == '^' || kind == '_') {
        /* OSC, DCS, SOS, PM, APC: a string, to BEL or ST (ESC \ or C1 0x9c) */
        while (i < n) {
            unsigned char ch = (unsigned char)t[i];
            if (ch == 0x07) return i + 1;
            if (ch == 0x1b && i + 1 < n && t[i + 1] == '\\') return i + 2;
            if (ch == 0xc2 && i + 1 < n && (unsigned char)t[i + 1] == 0x9c) return i + 2;
            i++;
        }
        return i;
    }
    if (intro >= 0x80) return i; /* any other C1 control is one code */
    /* ESC with intermediates (ESC ( B), then a final byte */
    while (i < n && (unsigned char)t[i] >= 0x20 && (unsigned char)t[i] <= 0x2f) i++;
    if (i < n && (unsigned char)t[i] >= 0x30 && (unsigned char)t[i] <= 0x7e) i++;
    return i;
}

size_t hold_strip_controls(char *t, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        unsigned char ch = (unsigned char)t[i];
        if (ch == 0x1b) {
            if (i + 1 >= n) break;
            unsigned char intro = (unsigned char)t[i + 1];
            /* ESC then a control: the ESC alone, the control on its own turn */
            i = intro < 0x20 || intro == 0x7f ? i + 1 : skip_sequence(t, n, i + 2, intro);
            continue;
        }
        if (ch == 0xc2 && i + 1 < n && (unsigned char)t[i + 1] >= 0x80 && (unsigned char)t[i + 1] <= 0x9f) {
            i = skip_sequence(t, n, i + 2, (unsigned char)t[i + 1]);
            continue;
        }
        if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f) {
            i++;
            continue;
        }
        t[o++] = t[i++];
    }
    return o;
}
