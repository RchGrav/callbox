#include "backmux/config.h"
#include "backmux/core.h"
#include "backmux/term.h"

/* THE spawn engine. Every backmux process that starts a program under a
 * console broker - on a PTY, or on pipes - goes through term_spawn; there is
 * no second copy of the fork/setsid/dup2/exec ladder or of the errno
 * handshake anywhere else. */

/* Open a PTY pair. The master is O_CLOEXEC; the slave is opened O_NOCTTY so
 * the calling process (often a session leader without a controlling terminal)
 * never adopts the PTY as its own — the spawned child claims it explicitly
 * via setsid + TIOCSCTTY, scoping terminal-generated signals (Ctrl-C, etc.)
 * and the foreground process group to the child. The PTY gets a real window
 * size before anyone execs on it: the kernel default of 0x0 makes
 * shells/readline and TUIs misrender and appear to drop keystrokes, so absent
 * dimensions fall back to 80x24 (e.g. a detached `-d -t` start). */
static int term_open_pty(int *master_out, int *slave_out,
                         unsigned short rows, unsigned short cols) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows ? rows : 24;
    ws.ws_col = cols ? cols : 80;
    (void)ioctl(master, TIOCSWINSZ, &ws);
    *master_out = master;
    *slave_out = slave;
    return 0;
}

int hold_cloexec_pipe(int fds[2]) {
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC) == 0) {
        return 0;
    }
#endif
    /* Non-Linux builds set FD_CLOEXEC after pipe(); unlike pipe2(O_CLOEXEC),
     * that is not atomic with respect to concurrent fork/exec in a
     * multi-threaded process. backmux is single-threaded at every spawn site, so
     * the fallback is acceptable. */
    if (pipe(fds) != 0) {
        return -1;
    }
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        fds[0] = fds[1] = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

/* Child side: any pre-exec failure rides the handshake pipe as an errno,
 * then _exit(127) — never exit(), never a return into the caller's stack. */
static _Noreturn void term_child_fail(int handshake_fd) {
    int e = errno;
    (void)hold_write_all(handshake_fd, &e, sizeof(e));
    _exit(127);
}

/* What the child sets up before exec: its stdio (a PTY slave, or pipes) and
 * its session. Returns -1 with errno set on failure. */
struct term_stdio {
    bool pty;
    int slave;          /* pty: the slave; becomes fds 0, 1, 2 */
    int in_r, out_w;    /* pipes: stdin's read end; stdout+stderr's write end */
};

static int term_child_stdio(const struct term_stdio *io) {
    /* Become a session leader: a call's process group is its own, so a
     * signal for the call reaches exactly it. On a PTY, also claim the slave
     * as the controlling terminal, so Ctrl-C interrupts the child (not its
     * parent) and interactive shells can run job control. */
    if (setsid() < 0) return -1;
    /* Default Ctrl-C and Ctrl-\ handling, whatever the broker inherited (a
     * shell starts background jobs with them ignored, and an ignored-at-start
     * signal cannot be trapped). */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    int fds[3] = {io->slave, io->slave, io->slave};
    if (io->pty) {
#ifdef TIOCSCTTY
        if (ioctl(io->slave, TIOCSCTTY, 0) != 0) return -1;
#endif
    } else {
        fds[0] = io->in_r;
        fds[1] = io->out_w;
        fds[2] = io->out_w;
    }
    for (int target = 0; target < 3; target++)
        if (dup2(fds[target], target) < 0) return -1;
    for (int target = 0; target < 3; target++) {
        /* dup2 onto itself keeps O_CLOEXEC: clear it, or exec closes this
         * stdio fd (when backmux was started with one closed). */
        int fl = fcntl(target, F_GETFD);
        if (fl >= 0) (void)fcntl(target, F_SETFD, fl & ~FD_CLOEXEC);
    }
    return 0;
}

/* THE fork/exec ladder with the errno handshake, for both kinds of stdio.
 * The caller's fds are all CLOEXEC, so exec sheds everything but stdio. */
static int term_spawn(const struct hold_term_spawn *spec, const struct term_stdio *io, pid_t *pid_out) {
    int hs[2];
    if (hold_cloexec_pipe(hs) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(hs[0]);
        close(hs[1]);
        errno = saved;
        return -1;
    }
    if (pid == 0) {
        if (spec->cwd && *spec->cwd && chdir(spec->cwd) != 0) term_child_fail(hs[1]);
        if (term_child_stdio(io) != 0) term_child_fail(hs[1]);
        if (spec->exec_path && *spec->exec_path) {
            execv(spec->exec_path, spec->argv);
        } else {
            execvp(spec->argv[0], spec->argv);
        }
        term_child_fail(hs[1]);
    }
    close(hs[1]);
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(hs[0], &child_errno);
    int read_errno = errno;
    close(hs[0]);
    if (handshake != 0) {
        /* handshake < 0: our read failed and the child's fate is unknown —
         * kill it. handshake > 0: the child reported errno and _exit(127)ed;
         * the kill is a no-op on the corpse and the waitpid reaps it. */
        kill(pid, SIGKILL);
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
        errno = handshake > 0 ? (child_errno ? child_errno : EIO) : read_errno;
        return -1;
    }
    *pid_out = pid;
    return 0;
}

int hold_term_pty_spawn(const struct hold_term_spawn *spec,
                        int *master_out, pid_t *pid_out) {
    if (!spec || !spec->argv || !spec->argv[0] || !master_out || !pid_out) {
        errno = EINVAL;
        return -1;
    }
    int master = -1;
    int slave = -1;
    if (term_open_pty(&master, &slave, spec->rows, spec->cols) != 0) {
        return -1;
    }
    struct term_stdio io = {.pty = true, .slave = slave};
    int rc = term_spawn(spec, &io, pid_out);
    int saved = errno;
    close(slave);
    if (rc != 0) {
        close(master);
        errno = saved;
        return -1;
    }
    *master_out = master;
    return 0;
}

int hold_term_pipe_spawn(const struct hold_term_spawn *spec,
                         int *in_w_out, int *out_r_out, pid_t *pid_out) {
    if (!spec || !spec->argv || !spec->argv[0] || !in_w_out || !out_r_out || !pid_out) {
        errno = EINVAL;
        return -1;
    }
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (hold_cloexec_pipe(in) != 0) return -1;
    if (hold_cloexec_pipe(out) != 0) {
        int saved = errno;
        close(in[0]);
        close(in[1]);
        errno = saved;
        return -1;
    }
    struct term_stdio io = {.pty = false, .in_r = in[0], .out_w = out[1]};
    int rc = term_spawn(spec, &io, pid_out);
    int saved = errno;
    close(in[0]);
    close(out[1]);
    if (rc != 0) {
        close(in[1]);
        close(out[0]);
        errno = saved;
        return -1;
    }
    *in_w_out = in[1];
    *out_r_out = out[0];
    return 0;
}
