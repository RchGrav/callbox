#include "backmux/config.h"
#include "backmux/core.h"
#include "backmux/platform.h"
#include "backmux/store.h"
#include "backmux/console.h"
#include "backmux/runtime.h"

/* backmux shell: the layer in the person's terminal.
 *
 * Their home shell runs on a PTY held by a broker, like any TTY process, but
 * it is theirs: no name, not listed. Ctrl-P Ctrl-Q in it asks its broker
 * what holds the terminal (the PTY's foreground process group, read from
 * the master - no help from the shell):
 *
 * - nothing but the shell, at its prompt: an ordinary new shell is started,
 *   detached, in the home shell's folder; the person stays home.
 * - a program the person started there: it keeps that PTY and its broker
 *   for good - a running program can never leave the terminal it started on
 *   - and its record becomes the program's, read from the process itself
 *   (executable, arguments, folder, environment). A new home starts in the
 *   same folder.
 *
 * exit in the home shell ends the layer. Why a loop here, not in the broker:
 * names, records and launches are this layer's business; the broker only
 * holds the PTY and says what is in its foreground. */

static int read_reply(const char *sock, const char *req, char *out, size_t n) {
    unsigned char *reply = NULL;
    size_t len = 0;
    if (hold_console_request(sock, req, strlen(req), &reply, &len) != 0) return -1;
    size_t k = 0;
    while (k < len && k + 1 < n && reply[k] != '\n') {
        out[k] = (char)reply[k];
        k++;
    }
    out[k] = '\0';
    free(reply);
    return 0;
}

/* The home shell's folder, from the process (where the person cd'd to). */
static void home_folder(const struct hold_run_record *r, char *out, size_t n) {
    struct hold_proc_launch l;
    if (hold_proc_launch_read(r->pid, &l) == 0) {
        snprintf(out, n, "%s", l.cwd);
        hold_proc_launch_free(&l);
        return;
    }
    if (!getcwd(out, n)) snprintf(out, n, "/");
}

/* "~/x" for a folder under $HOME: how the person thinks of it. */
static void tilde(const char *path, char *out, size_t n) {
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl]))
        snprintf(out, n, "~%s", path + hl);
    else
        snprintf(out, n, "%s", path);
}

/* The corner note, cut to fit the terminal: the folder loses its start
 * ("…/scratchpad/proj"), never the name - the broker draws no label wider
 * than the screen, so a long folder would have hidden the whole note. */
static void fit_note(char *note, size_t n, const char *lead, const char *where) {
    struct winsize ws;
    int cols = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
    int room = cols - 4 - (int)strlen(lead); /* the label's padding, a margin */
    int wl = (int)strlen(where);
    if (room >= wl) snprintf(note, n, "%s%s", lead, where);
    else if (room > 4) snprintf(note, n, "%s...%s", lead, where + wl - (room - 3));
    else snprintf(note, n, "%s", lead);
}

/* A new ordinary shell, detached, in folder: backmux detach -- SHELL, run
 * as its own launch (the same path as typing it). Its name, from the
 * launch's "name: X" line, goes in name. */
static int spawn_shell(const char *folder, const char *shell, char *name, size_t n) {
    char self[HOLD_PATH_MAX];
    if (hold_resolve_self_executable_path("backmux", self, sizeof(self)) != 0) return -1;
    /* Its stderr is a file, not a pipe: the new shell's broker inherits it
     * and lives on, so a pipe would never reach end-of-file. */
    FILE *errf = tmpfile();
    if (!errf) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        fclose(errf);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
        }
        dup2(fileno(errf), STDERR_FILENO);
        if (chdir(folder) != 0) _exit(127);
        execl(self, "backmux", "detach", "--", shell, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        continue;
    }
    char buf[512];
    rewind(errf);
    size_t got = fread(buf, 1, sizeof(buf) - 1, errf);
    buf[got] = '\0';
    fclose(errf);
    const char *at = strstr(buf, "name: ");
    if (!at || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    at += 6;
    size_t len = strcspn(at, "\r\n");
    snprintf(name, n, "%.*s", (int)len, at);
    return 0;
}

/* The home's record becomes the program's: its name, its identity (what
 * end and kill re-check before they signal), and what it is - read from the
 * process, so an alias or `cd x && codex` is recorded as what really runs,
 * and recall relaunches the real thing. */
static int adopt_record(const struct hold_store *store, const char *id, pid_t pgid, char *name, size_t n) {
    pid_t leader = pgid; /* the job's group leader: the program the shell started */
    struct hold_proc_launch l;
    if (hold_proc_launch_read(leader, &l) != 0) return -1;
    char state = 0;
    uint64_t ticks = 0, dev = 0, ino = 0;
    if (hold_read_proc_stat_tokens(leader, &state, &ticks) != 0 || hold_read_proc_exe(leader, &dev, &ino) != 0) {
        hold_proc_launch_free(&l);
        return -1;
    }
    char picked[ALIAS_MAX_LEN + 1];
    if (hold_generate_run_name_for_id(store, id, NULL, picked) != 0) {
        hold_proc_launch_free(&l);
        return -1;
    }
    int lock_fd = hold_store_lock(store->record_dir);
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    int rc = -1;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) == 0) {
        r.home = false;
        r.has_person = false;
        r.adopted = true;
        r.has_name = true;
        snprintf(r.name, sizeof(r.name), "%s", picked);
        r.pid = leader;
        r.pgid = pgid;
        r.proc_starttime_ticks = ticks;
        r.exe_dev = dev;
        r.exe_ino = ino;
        r.recipe.mode_shell = false;
        r.recipe.mode_chat = false;
        snprintf(r.recipe.working, sizeof(r.recipe.working), "%s", l.cwd);
        r.recipe.has_working = true;
        snprintf(r.observed_cwd, sizeof(r.observed_cwd), "%s", l.cwd); /* where it started */
        for (int i = 0; i < r.recipe.argc; i++) free(r.recipe.argv[i]);
        free(r.recipe.argv);
        for (int i = 0; i < r.recipe.envc; i++) free(r.recipe.env[i]);
        free(r.recipe.env);
        r.recipe.argv = l.argv, r.recipe.argc = l.argc;
        r.recipe.env = l.envp, r.recipe.envc = l.envc;
        l.argv = l.envp = NULL;
        l.argc = l.envc = 0;
        char rewritten[HOLD_PATH_MAX];
        rc = hold_write_record_atomic(store->record_dir, &r, r.recipe.argc, r.recipe.argv, rewritten, sizeof(rewritten));
        hold_free_run_record(&r);
    }
    hold_store_unlock(lock_fd);
    hold_proc_launch_free(&l);
    if (rc == 0) snprintf(name, n, "%s", picked);
    return rc;
}

static int reattach(const struct hold_invocation *inv, const struct hold_store *store, const char *sock) {
    hold_console_set_line_mode(false);
    hold_console_set_stream(false);
    hold_console_set_forward_term(true);
    hold_prepare_mux(inv, store);
    /* inline, as the launch was: the screen already shows home, and its
     * broker has replayed it once - what comes now is only what is new */
    hold_console_set_inline(true);
    return hold_run_native_console(sock);
}

int hold_cmd_shell_layer(const struct hold_invocation *inv, const struct hold_store *store,
                         struct hold_start_options *opts) {
    char id[ID_STR_LEN + 1];
    const char *shell = opts->argc > 0 ? opts->argv[0] : "/bin/sh";
    /* --name names the person, not the shell (home has none): it is their
     * chat name from now on, the same as backmux config chat-name NAME. */
    if (opts->run_name && *opts->run_name) {
        const char *home = hold_person_home_dir(inv);
        if (!hold_valid_alias(opts->run_name) || !home || hold_settings_put(home, "chat-name", opts->run_name) != 0) {
            fprintf(stderr, "backmux: error: cannot take '%s' as your name\n", opts->run_name);
            return HOLD_EXIT_USAGE;
        }
        opts->run_name = NULL;
    }
    for (;;) {
        id[0] = '\0';
        opts->home = true;
        opts->out_id = id;
        int rc = hold_perform_start_options(inv, store, opts);
        if (!id[0]) return rc;
        for (;;) {
            /* exit (or anything but a detach) ends the layer */
            if (!hold_console_was_detached(NULL)) return rc;
            struct hold_run_record r;
            char path[HOLD_PATH_MAX];
            if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return rc;
            char sock[HOLD_PATH_MAX], folder[HOLD_PATH_MAX];
            snprintf(sock, sizeof(sock), "%s", r.console_sock);
            home_folder(&r, folder, sizeof(folder));
            hold_free_run_record(&r);
            /* Detached from another process (a split handed it back): that
             * one runs on; home comes back. */
            const char *last = hold_console_last_sock();
            if (last && last[0] && strcmp(last, sock) != 0) {
                rc = reattach(inv, store, sock);
                continue;
            }
            char reply[128];
            if (read_reply(sock, "adopt", reply, sizeof(reply)) != 0) return rc;
            if (!strcmp(reply, "none")) {
                char name[ALIAS_MAX_LEN + 1], where[HOLD_PATH_MAX], lead[ALIAS_MAX_LEN + 64];
                char note[HOLD_PATH_MAX + ALIAS_MAX_LEN + 64], req[sizeof(note) + 8];
                tilde(folder, where, sizeof(where));
                if (spawn_shell(folder, shell, name, sizeof(name)) == 0)
                    snprintf(lead, sizeof(lead), "started %s in ", name);
                else
                    snprintf(lead, sizeof(lead), "could not start a shell in ");
                fit_note(note, sizeof(note), lead, where);
                snprintf(req, sizeof(req), "note %s", note);
                (void)read_reply(sock, req, reply, sizeof(reply));
                rc = reattach(inv, store, sock);
                continue;
            }
            long pg = 0;
            if (sscanf(reply, "job %ld", &pg) != 1 || pg <= 1) {
                hold_sig_note(inv, "backmux: %s\n", reply);
                return rc;
            }
            char name[ALIAS_MAX_LEN + 1];
            if (adopt_record(store, id, (pid_t)pg, name, sizeof(name)) == 0)
                hold_sig_note(inv, "backmux: detached %s\n", name);
            else
                hold_sig_note(inv, "backmux: detached the program; its details could not be read\n");
            /* a new home, where the old one was */
            if (chdir(folder) != 0 && chdir("/") != 0) return rc;
            break;
        }
    }
}
