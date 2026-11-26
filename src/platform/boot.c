#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/platform.h"
#include "backmux/core.h"

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
    /* No pseudo-file: kern.bootsessionuuid is macOS's boot id - new at each
     * boot, fixed until the next, whatever the clock does. Why not the boot
     * time (kern.boottime): macOS recomputes it when it adjusts the clock,
     * so the same boot read differently later and every process turned stale. */
    char uuid[64];
    size_t ulen = sizeof(uuid);
    if (sysctlbyname("kern.bootsessionuuid", uuid, &ulen, NULL, 0) == 0 && ulen > 1 && ulen <= sizeof(uuid)) {
        uuid[ulen - 1] = '\0';
        snprintf(buf, n, "macos-%s", uuid);
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
