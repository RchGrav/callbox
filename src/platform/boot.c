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
    /* No pseudo-file: synthesize from the kernel boot time — stable within a
     * boot, distinct across boots. */
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime)) {
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
 * kernel UUID: equal or not. A macOS one is synthesized from kern.boottime,
 * which macOS recomputes when it adjusts the clock (time sync, wake), so the
 * same boot reads a little differently later: boot times within 60 s are the
 * same boot. Why: an exact match made every call "stale" a few minutes in
 * (0.066 s apart on one Mac), and msg, send, end and list then treated live
 * calls as ended. A real reboot moves the boot time by the whole previous
 * uptime, far past 60 s. */
#define HOLD_MACOS_BOOT_SLACK_S 60
static bool parse_macos_boot(const char *id, long long *sec) {
    if (strncmp(id, "macos-", 6) != 0) return false;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(id + 6, &end, 10);
    if (errno != 0 || end == id + 6 || (*end != '.' && *end != '\0')) return false;
    *sec = v;
    return true;
}

bool hold_same_boot(const char *recorded, const char *current) {
    if (!recorded || !current) return true; /* nothing to compare: no boot check */
    if (strcmp(recorded, current) == 0) return true;
    long long a, b;
    if (parse_macos_boot(recorded, &a) && parse_macos_boot(current, &b))
        return (a > b ? a - b : b - a) <= HOLD_MACOS_BOOT_SLACK_S;
    return false;
}
