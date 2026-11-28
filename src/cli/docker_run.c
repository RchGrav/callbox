/* cli/docker_run.c - backmux run: Docker's run grammar, on the native launcher.
 *
 * Why its own grammar: run is the Docker-style surface, for scripts written
 * for docker run, and it must read their words as docker run does. -w is the
 * working directory (natively -w is wait); the first operand is always the
 * program, never a backmux verb (backmux run list runs a program named list);
 * short flags combine (-dit, -dw /tmp); without -t there is no terminal; a
 * detached run keeps its exit status for wait; its own errors exit 125.
 * Why not a second launcher: the native one already does all of it, so this
 * only translates the words into native ones (the program after "--") and
 * changes to the working directory; nothing about launching is duplicated. */
#include "backmux/config.h"
#include "backmux/cli.h"

#define RUN_USAGE "backmux run [OPTIONS] PROGRAM [ARG...]"

struct docker_run {
    bool detach, interactive, tty, rm, help;
    const char *name, *restart, *detach_keys, *workdir;
    const char **env; /* -e and --env-file, in order, as native words */
    int nenv;
    char *made[64];   /* -e KEY values copied from the environment, freed on an error */
    int nmade;
};

static int run_error(const char *fmt, const char *arg) {
    fputs("backmux: ", stderr);
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\nSee 'backmux run --help'.\n");
    return 125;
}

static int push_env(struct docker_run *d, const char *flag, const char *value) {
    const char **e = realloc(d->env, (size_t)(d->nenv + 2) * sizeof(*e));
    if (!e) return -1;
    d->env = e;
    d->env[d->nenv++] = flag;
    d->env[d->nenv++] = value;
    return 0;
}

/* -e KEY (no value) is docker's pass-through: KEY as it is in this
 * environment, or nothing when it is not set. */
static int add_env(struct docker_run *d, const char *kv) {
    if (strchr(kv, '=')) return push_env(d, "-e", kv);
    const char *v = getenv(kv);
    if (!v) return 0;
    size_t n = strlen(kv) + strlen(v) + 2;
    char *full = d->nmade < 64 ? malloc(n) : NULL;
    if (!full) return -1;
    d->made[d->nmade++] = full;
    snprintf(full, n, "%s=%s", kv, v);
    return push_env(d, "-e", full);
}

/* Container-only flags: refused, never quietly ignored or remapped. */
static const char *container_only(const char *a) {
    static const char *const flags[] = {"-p", "--publish", "-P", "--publish-all", "-v", "--volume", "--mount",
                                        "--privileged", "--cap-add", "--cap-drop", "--network", "--net", NULL};
    for (int i = 0; flags[i]; i++) {
        size_t l = strlen(flags[i]);
        if (!strncmp(a, flags[i], l) && (a[l] == '\0' || a[l] == '=')) return flags[i];
    }
    return NULL;
}

/* A long option with a value: --name=X or --name X. */
static bool long_value(int argc, char **argv, int *i, const char *name, const char **out, int *rc) {
    size_t l = strlen(name);
    const char *a = argv[*i];
    if (strncmp(a, name, l) != 0) return false;
    if (a[l] == '=') {
        *out = a + l + 1;
        return true;
    }
    if (a[l] != '\0') return false;
    if (*i + 1 >= argc) {
        *rc = run_error("flag needs an argument: %s", name);
        return true;
    }
    *out = argv[++*i];
    return true;
}

static int parse(int argc, char **argv, int *i, struct docker_run *d) {
    for (; *i < argc; (*i)++) {
        const char *a = argv[*i];
        if (!strcmp(a, "--")) {
            (*i)++;
            return 0;
        }
        if (a[0] != '-' || !a[1]) return 0; /* the program */
        const char *c = container_only(a);
        if (c) {
            fprintf(stderr, "backmux: %s needs a container, and backmux runs host processes; "
                            "see backmux help aliases\n", c);
            return 125;
        }
        int rc = 0;
        const char *v = NULL;
        if (!strcmp(a, "--detach")) d->detach = true;
        else if (!strcmp(a, "--interactive")) d->interactive = true;
        else if (!strcmp(a, "--tty")) d->tty = true;
        else if (!strcmp(a, "--rm")) d->rm = true;
        else if (!strcmp(a, "--help")) d->help = true;
        else if (long_value(argc, argv, i, "--workdir", &d->workdir, &rc) ||
                 long_value(argc, argv, i, "--name", &d->name, &rc) ||
                 long_value(argc, argv, i, "--restart", &d->restart, &rc) ||
                 long_value(argc, argv, i, "--detach-keys", &d->detach_keys, &rc)) {
            if (rc) return rc;
        } else if (long_value(argc, argv, i, "--env-file", &v, &rc)) {
            if (rc || push_env(d, "--env-file", v) != 0) return rc ? rc : 125;
        } else if (long_value(argc, argv, i, "--env", &v, &rc)) {
            if (rc || add_env(d, v) != 0) return rc ? rc : 125;
        } else if (a[1] == '-') {
            return run_error("unknown flag: %s", a);
        } else {
            /* A group of short flags (-dit). A flag that takes a value takes
             * the rest of the group, or else the next word (-dw /tmp). */
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'd') d->detach = true;
                else if (*p == 'i') d->interactive = true;
                else if (*p == 't') d->tty = true;
                else if (*p == 'h') d->help = true;
                else if (*p == 'w' || *p == 'e') {
                    const char *val = p[1] ? p + 1 : (*i + 1 < argc ? argv[++*i] : NULL);
                    if (!val) {
                        char f[3] = {'-', *p, '\0'};
                        return run_error("flag needs an argument: %s", f);
                    }
                    if (*p == 'w') d->workdir = val;
                    else if (add_env(d, val) != 0) return 125;
                    break;
                } else {
                    char f[3] = {'-', *p, '\0'};
                    return run_error("unknown shorthand flag: '%s'", f + 1);
                }
            }
        }
    }
    return 0;
}

int hold_cli_docker_run_words(int argc, char **argv, int *argc_out, char ***argv_out, const char **workdir_out) {
    struct docker_run d = {0};
    int i = 2; /* argv[0] backmux, argv[1] run */
    int rc = parse(argc, argv, &i, &d);
    if (rc || d.help || i >= argc) { /* nothing will hold the words: free what was made */
        free(d.env);
        for (int k = 0; k < d.nmade; k++) free(d.made[k]);
        d.env = NULL;
        d.nmade = 0;
    }
    if (rc) return rc;
    if (d.help) {
        static char *help[] = {NULL, "help", "launch", NULL};
        help[0] = argv[0];
        *argc_out = 3;
        *argv_out = help;
        *workdir_out = NULL;
        return 0;
    }
    if (i >= argc) {
        fprintf(stderr, "backmux: 'backmux run' requires at least 1 argument\n\nUsage:  " RUN_USAGE
                        "\n\nSee 'backmux run --help' for more information\n");
        return 125;
    }
    /* Native words: the mode, then the options, then "--" and the program.
     * Without -t there is no terminal: the foreground is a plain waiter (-w)
     * and -i a pipe; detached, -w keeps the exit status (a waiter), as docker
     * keeps a container's. */
    char mode[8], *m = mode;
    *m++ = '-';
    if (d.detach) *m++ = 'd';
    if (d.tty) *m++ = 't';
    else if (d.interactive) *m++ = 'i';
    else *m++ = 'w';
    if (d.tty && d.interactive && !d.detach) *m++ = 'i';
    *m = '\0';
    /* The words live as long as the process (the launch reads them to its
     * end), so they are held here, where the process can still reach them. */
    static char mode_copy[8];
    static char **out;
    memcpy(mode_copy, mode, sizeof(mode_copy));
    free(out);
    out = calloc((size_t)(argc + d.nenv + 16), sizeof(char *));
    if (!out) return 125;
    int k = 0;
    out[k++] = argv[0];
    out[k++] = mode_copy;
    out[k++] = "--quiet"; /* docker run prints no notes of its own: -d prints the ID, nothing else */
    if (d.name) out[k++] = "--name", out[k++] = (char *)d.name;
    if (d.rm) out[k++] = "--rm";
    if (d.restart) out[k++] = "--restart", out[k++] = (char *)d.restart;
    if (d.detach_keys) out[k++] = "--detach-keys", out[k++] = (char *)d.detach_keys;
    for (int e = 0; e < d.nenv; e++) out[k++] = (char *)d.env[e];
    out[k++] = "--";
    while (i < argc) out[k++] = argv[i++];
    out[k] = NULL;
    free(d.env);
    *argc_out = k;
    *argv_out = out;
    *workdir_out = d.workdir;
    return 0;
}
