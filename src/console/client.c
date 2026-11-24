#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/console.h"
#include "backmux/core.h"
#include "backmux/term.h"

/* THE client loop: every way a person watches or is on a call - attached to
 * its console, or following its log - runs here, so the keys mean the same
 * thing everywhere and the terminal is restored the same way. The caller
 * chooses the output source (a console socket, or a log file read as it
 * grows) and the hooks; the caller owns the terminal's mode. */

/* A log file never reports "no more data" through poll: read it until EOF,
 * then look again after this long. */
#define CLIENT_FILE_POLL_MS 100
/* ...and ask whether the call has ended every this many quiet looks. */
#define CLIENT_FILE_DONE_EVERY 10
/* macOS poll() never reports end-of-file on a FIFO: probe this often. */
#define CLIENT_FIFO_PROBE_MS 250

struct key_sink_ctx {
    const struct hold_client *c;
};

static int key_sink(void *vctx, const unsigned char *bytes, size_t n) {
    const struct hold_client *c = ((struct key_sink_ctx *)vctx)->c;
    return c->on_input ? (c->on_input(c->ctx, bytes, n) < 0 ? -1 : 0) : 0;
}

static int deliver_output(const struct hold_client *c, const char *buf, size_t n) {
    if (c->on_output) return c->on_output(c->ctx, buf, n);
    return hold_write_all(STDOUT_FILENO, buf, n) == 0 ? 0 : -1;
}

/* A hook's answer: <0 error, >0 stop the loop, 0 go on. */
static bool hook_stops(int rc, enum hold_client_end *end) {
    if (rc < 0) {
        *end = HOLD_CLIENT_ERROR;
        return true;
    }
    if (rc > 0) {
        *end = HOLD_CLIENT_STOPPED;
        return true;
    }
    return false;
}

static int drain_file(const struct hold_client *c, bool *ended) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(c->out_fd, buf, sizeof(buf));
        if (n > 0) {
            int rc = deliver_output(c, buf, (size_t)n);
            if (rc != 0) return rc;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        *ended = true; /* at the end of what is there so far */
        return 0;
    }
}

enum hold_client_end hold_client_run(const struct hold_client *c) {
    struct hold_term_signals sig;
    hold_term_signals_install(&sig, c->want_sigint);
    enum hold_client_end end = HOLD_CLIENT_OUTPUT_ENDED;
    bool input_open = c->in_fd >= 0;
    int quiet_looks = 0;
    struct key_sink_ctx ks = {c};

    for (;;) {
        if (hold_term_stop_signal()) {
            end = HOLD_CLIENT_SIGNALED;
            break;
        }
        if (hold_term_take_resized() && c->on_resize) {
            struct winsize ws;
            if (hold_term_size(&ws) == 0 && hook_stops(c->on_resize(c->ctx, &ws), &end)) break;
        }
        if (hold_term_take_interrupt()) {
            if (!c->on_interrupt) {
                end = HOLD_CLIENT_STOPPED;
                break;
            }
            if (hook_stops(c->on_interrupt(c->ctx), &end)) break;
        }

        if (c->out_is_file) {
            bool at_end = false;
            int rc = drain_file(c, &at_end);
            if (rc != 0) {
                if (hook_stops(rc, &end)) break;
            }
            if (c->file_done && ++quiet_looks % CLIENT_FILE_DONE_EVERY == 0 && c->file_done(c->ctx)) {
                /* One final drain: output written between the last read and
                 * the end still lands. */
                (void)drain_file(c, &at_end);
                end = HOLD_CLIENT_OUTPUT_ENDED;
                break;
            }
            if (!c->file_done) {
                end = HOLD_CLIENT_OUTPUT_ENDED; /* not following: what is there is all */
                break;
            }
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int out_idx = -1, in_idx = -1;
        if (!c->out_is_file) {
            out_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = c->out_fd, .events = POLLIN};
        }
        if (input_open) {
            in_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = c->in_fd, .events = POLLIN};
        }
        int timeout = c->out_is_file ? CLIENT_FILE_POLL_MS : (c->probe_fifo ? CLIENT_FIFO_PROBE_MS : -1);
        int sr = nfds ? poll(pfds, nfds, timeout) : (nanosleep(&(struct timespec){0, (long)timeout * 1000000L}, NULL), 0);
        if (sr < 0) {
            if (errno == EINTR) continue;
            end = HOLD_CLIENT_ERROR;
            break;
        }

        /* POLLNVAL too: macOS poll() rejects character devices such as
         * /dev/null; reading one then reports its end, instead of spinning. */
        if (in_idx >= 0 && (pfds[in_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(c->in_fd, buf, sizeof(buf));
            bool stop = false;
            if (n > 0) {
                quiet_looks = 0;
                for (ssize_t i = 0; i < n && !stop; i++) {
                    if (!c->keys) {
                        stop = hook_stops(c->on_input ? c->on_input(c->ctx, buf, (size_t)n) : 0, &end);
                        break;
                    }
                    int completed = 0;
                    if (hold_term_detach_feed(c->keys, buf[i], key_sink, &ks, &completed) != 0) {
                        end = HOLD_CLIENT_ERROR;
                        stop = true;
                    } else if (completed == 1) {
                        stop = hook_stops(c->on_detach ? c->on_detach(c->ctx) : 1, &end);
                    } else if (completed == 2) {
                        stop = hook_stops(c->on_interrupt ? c->on_interrupt(c->ctx) : 1, &end);
                    } else if (completed == 3) {
                        stop = hook_stops(c->on_command ? c->on_command(c->ctx, c->keys->command) : 0, &end);
                    }
                }
            } else if (n == 0 || errno == EBADF) { /* end of input, or none at all */
                if (c->keys && hold_term_detach_flush(c->keys, key_sink, &ks) != 0) {
                    end = HOLD_CLIENT_ERROR;
                    stop = true;
                }
                input_open = false;
                if (!stop && c->on_input_end) stop = hook_stops(c->on_input_end(c->ctx), &end);
            } else if (errno != EINTR) {
                end = HOLD_CLIENT_ERROR;
                stop = true;
            }
            if (stop) break;
        }

        if (out_idx >= 0) {
            bool probe = c->probe_fifo && !(pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR));
            if ((pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR)) || probe) {
                char buf[4096];
                int flags = probe ? fcntl(c->out_fd, F_GETFL) : -1;
                if (flags >= 0) (void)fcntl(c->out_fd, F_SETFL, flags | O_NONBLOCK);
                ssize_t n = read(c->out_fd, buf, sizeof(buf));
                int read_errno = errno;
                if (flags >= 0) (void)fcntl(c->out_fd, F_SETFL, flags);
                errno = read_errno;
                if (n > 0) {
                    if (hook_stops(deliver_output(c, buf, (size_t)n), &end)) break;
                } else if (n == 0 || errno == ECONNRESET) {
                    /* (a reset is the source closing with our last bytes
                     * unread - it has ended all the same) */
                    end = HOLD_CLIENT_OUTPUT_ENDED;
                    break;
                } else if (errno != EINTR && errno != EAGAIN) {
                    end = HOLD_CLIENT_ERROR;
                    break;
                }
            }
        }
    }
    hold_term_signals_restore(&sig);
    return end;
}
