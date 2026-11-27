/* platform/procinfo.c - what a process runs and the machine it runs on: a
 * process's command line, its terminal's foreground group, its children and
 * its working directory, and the machine's CPU time and memory. The
 * compositor reads these to tell an agent harness from other programs, to
 * show each pane's load and the machine's, and to open a terminal where the
 * active pane is (Linux /proc, macOS sysctl and libproc, Mach). */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/platform.h"
#include "backmux/core.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#else
/* A small /proc file whole; empty reads as EIO. */
static ssize_t read_small_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r;
    do r = read(fd, buf, n - 1); while (r < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (r <= 0) { errno = r < 0 ? saved : EIO; return -1; }
    buf[r] = '\0';
    return r;
}

/* The fields after "(comm) " in /proc/<pid>/stat: 0 state, 1 ppid, 2 pgrp,
 * 3 session, 4 tty_nr, 5 tpgid. */
static const char *stat_field(pid_t pid, char *buf, size_t n, int idx) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_small_file(path, buf, n) < 0) return NULL;
    char *f = strrchr(buf, ')');
    if (!f || f[1] != ' ') return NULL;
    f += 2;
    for (; idx > 0 && f; idx--) {
        f = strchr(f, ' ');
        if (f) f++;
    }
    return f && *f ? f : NULL;
}
#endif


int hold_proc_cmdline(pid_t pid, char *out, size_t n, size_t *len) {
    *len = 0;
    if (pid <= 0 || n < 2) {
        errno = EINVAL;
        return -1;
    }
#if defined(__APPLE__)
    /* KERN_PROCARGS2: argc, the exec path, padding NULs, then argv. */
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t sz = 0;
    if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0 || sz < sizeof(int)) return -1;
    char *buf = malloc(sz);
    if (!buf) return -1;
    if (sysctl(mib, 3, buf, &sz, NULL, 0) != 0) {
        free(buf);
        return -1;
    }
    int argc = 0;
    memcpy(&argc, buf, sizeof(argc));
    size_t i = sizeof(int);
    while (i < sz && buf[i]) i++; /* the exec path */
    while (i < sz && !buf[i]) i++;
    size_t k = 0;
    for (int a = 0; a < argc && i < sz && k + 1 < n; a++) {
        while (i < sz && buf[i] && k + 1 < n) out[k++] = buf[i++];
        out[k++] = '\0';
        i++;
    }
    free(buf);
    *len = k;
    return k ? 0 : -1;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r;
    do r = read(fd, out, n - 1); while (r < 0 && errno == EINTR);
    close(fd);
    if (r <= 0) return -1; /* a kernel thread, or a zombie: no arguments */
    out[r] = '\0';
    *len = (size_t)r;
    return 0;
#endif
}

int hold_proc_foreground_group(pid_t pid, pid_t *pgid_out) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0) return -1;
    if (kp.kp_eproc.e_tpgid <= 0) return -1;
    *pgid_out = kp.kp_eproc.e_tpgid;
    return 0;
#else
    char buf[4096];
    const char *tp = stat_field(pid, buf, sizeof(buf), 5);
    long v = tp ? strtol(tp, NULL, 10) : -1;
    if (v <= 0) return -1;
    *pgid_out = (pid_t)v;
    return 0;
#endif
}

int hold_proc_for_each_child(pid_t pid, int (*cb)(pid_t child, void *ctx), void *ctx) {
#if defined(__APPLE__)
    pid_t kids[256];
    int got = proc_listchildpids(pid, kids, (int)sizeof(kids));
    for (int i = 0; got > 0 && i < got && i < 256; i++)
        if (kids[i] > 0 && cb(kids[i], ctx) != 0) return 0;
    return got < 0 ? -1 : 0;
#else
    char path[96], buf[4096];
    snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children", (long)pid, (long)pid);
    if (read_small_file(path, buf, sizeof(buf)) < 0) return errno == EIO ? 0 : -1; /* EIO: none */
    for (char *p = buf; *p;) {
        char *end = NULL;
        long c = strtol(p, &end, 10);
        if (end == p) break;
        if (c > 0 && cb((pid_t)c, ctx) != 0) return 0;
        p = end;
    }
    return 0;
#endif
}

int hold_system_cpu_times(uint64_t *busy, uint64_t *total) {
#if defined(__APPLE__)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS) return -1;
    uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
    uint64_t all = idle + info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
    *busy = all - idle;
    *total = all;
    return 0;
#else
    char buf[1024];
    if (read_small_file("/proc/stat", buf, sizeof(buf)) < 0) return -1;
    /* "cpu  user nice system idle iowait irq softirq steal ..." */
    unsigned long long v[8] = {0};
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
               &v[7]) < 4)
        return -1;
    uint64_t idle = v[3] + v[4], all = 0;
    for (int i = 0; i < 8; i++) all += v[i];
    *busy = all - idle;
    *total = all;
    return 0;
#endif
}

int hold_system_memory(uint64_t *total, uint64_t *available) {
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &sz, NULL, 0) != 0) return -1;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return -1;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;
    *total = mem;
    /* what Activity Monitor leaves out of "used": free, inactive, purgeable */
    *available = ((uint64_t)vm.free_count + vm.inactive_count + vm.purgeable_count) * (uint64_t)page;
    return 0;
#else
    char buf[4096];
    if (read_small_file("/proc/meminfo", buf, sizeof(buf)) < 0) return -1;
    unsigned long long t = 0, a = 0;
    const char *mt = strstr(buf, "MemTotal:"), *ma = strstr(buf, "MemAvailable:");
    if (!mt || sscanf(mt, "MemTotal: %llu", &t) != 1) return -1;
    if (!ma || sscanf(ma, "MemAvailable: %llu", &a) != 1) return -1;
    *total = (uint64_t)t * 1024;
    *available = (uint64_t)a * 1024;
    return 0;
#endif
}

int hold_proc_cwd(pid_t pid, char *out, size_t n) {
    if (pid <= 0 || n < 2) return -1;
#if defined(__APPLE__)
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) != (int)sizeof(vpi)) return -1;
    snprintf(out, n, "%s", vpi.pvi_cdir.vip_path);
    return out[0] == '/' ? 0 : -1;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cwd", (long)pid);
    ssize_t got = readlink(path, out, n - 1);
    if (got <= 0) return -1;
    out[got] = '\0';
    return out[0] == '/' ? 0 : -1;
#endif
}
