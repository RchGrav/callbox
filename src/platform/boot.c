#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/platform.h"
#include "callbox/core.h"

/* Read a boot-id pseudo-file, strip trailing whitespace, demand non-empty. */
static bool read_id_trim(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t r;
    do r = read(fd, buf, n - 1); while (r < 0 && errno == EINTR);
    close(fd);
    if (r < 0) return false;
    buf[r] = '\0';
    while (r > 0 && isspace((unsigned char)buf[r - 1])) buf[--r] = '\0';
    return buf[0] != '\0';
}

bool hold_current_boot_id(char *buf, size_t n) {
    if (n == 0) return false;
    buf[0] = '\0';
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_BOOT_ID_PATH");
    if (override && *override) return read_id_trim(override, buf, n);
#endif
    if (read_id_trim(HOLD_BOOT_ID_PATH, buf, n)) return true;
#if defined(__APPLE__)
    /* No pseudo-file. kern.bootsessionuuid is exact for one boot; the boot
     * time rides along only so records from before it (macos-<sec>.<usec>,
     * see hold_same_boot) can still be matched. kern.boottime alone is the
     * fallback where the UUID cannot be read. */
    struct timeval boottime;
    size_t len = sizeof(boottime);
    bool have_time = sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime);
    char uuid[64];
    size_t ulen = sizeof(uuid);
    if (sysctlbyname("kern.bootsessionuuid", uuid, &ulen, NULL, 0) == 0 && ulen > 1 && ulen <= sizeof(uuid)) {
        uuid[ulen - 1] = '\0';
        if (have_time) snprintf(buf, n, "macos-uuid-%s/%lld", uuid, (long long)boottime.tv_sec);
        else snprintf(buf, n, "macos-uuid-%s", uuid);
        return true;
    }
    if (have_time) {
        snprintf(buf, n, "macos-%lld.%06d", (long long)boottime.tv_sec, boottime.tv_usec);
        return true;
    }
#endif
    return false;
}

/* The common caller shape: a boot id when one is knowable, else NULL for
 * "evaluate without a boot check" (hold_eval_state and friends accept NULL). */
const char *hold_boot_id_or_null(char buf[128]) {
    return hold_current_boot_id(buf, 128) ? buf : NULL;
}

/* Whether a recorded boot id names the current boot. A Linux boot id is a
 * kernel UUID: equal or not. On macOS:
 *   macos-uuid-<UUID>/<sec>  kern.bootsessionuuid, exact; <sec> is the boot
 *                            time, kept only for the next form
 *   macos-<sec>.<usec>       kern.boottime alone: records from before 0.9.4,
 *                            and the fallback when the UUID cannot be read
 * Two UUID ids are the same boot when the UUIDs are equal (the seconds are
 * not compared: they drift). Anything else is compared by boot time, within
 * 60 s. Why: macOS recomputes kern.boottime when it adjusts the clock (time
 * sync, wake), so an exact match on it made every call "stale" a few minutes
 * in (0.066 s apart on one Mac), and msg, send, end and list treated live
 * calls as ended. The UUID has no tolerance to tune; the 60 s rule remains
 * only for ids without one, so calls running across the upgrade keep
 * working. A real reboot moves the boot time by the whole previous uptime. */
#define HOLD_MACOS_BOOT_SLACK_S 60
struct macos_boot {
    char uuid[64]; /* "" = none */
    long long sec;
    bool has_sec;
};

static bool parse_macos_boot(const char *id, struct macos_boot *out) {
    memset(out, 0, sizeof(*out));
    const char *p;
    if (strncmp(id, "macos-uuid-", 11) == 0) {
        p = id + 11;
        const char *slash = strchr(p, '/');
        size_t ul = slash ? (size_t)(slash - p) : strlen(p);
        if (ul == 0 || ul >= sizeof(out->uuid)) return false;
        memcpy(out->uuid, p, ul);
        out->uuid[ul] = '\0';
        if (!slash) return true;
        p = slash + 1;
    } else if (strncmp(id, "macos-", 6) == 0) {
        p = id + 6;
    } else {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    /* Seconds that do not read leave a UUID id its UUID, and an old id nothing. */
    if (errno != 0 || end == p || (*end != '.' && *end != '\0')) return out->uuid[0] != '\0';
    out->sec = v;
    out->has_sec = true;
    return true;
}

bool hold_same_boot(const char *recorded, const char *current) {
    if (!recorded || !current) return true; /* nothing to compare: no boot check */
    if (strcmp(recorded, current) == 0) return true;
    struct macos_boot a, b;
    if (!parse_macos_boot(recorded, &a) || !parse_macos_boot(current, &b)) return false;
    if (a.uuid[0] && b.uuid[0]) return strcmp(a.uuid, b.uuid) == 0;
    if (a.has_sec && b.has_sec) return (a.sec > b.sec ? a.sec - b.sec : b.sec - a.sec) <= HOLD_MACOS_BOOT_SLACK_S;
    return false;
}
