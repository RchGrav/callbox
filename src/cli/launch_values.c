/* cli/launch_values.c - the values launch options take, parsed and checked:
 * a restart policy and its delay, detach keys, environment assignments and
 * env files. Moved out of main.c, which keeps the parsing of the command
 * line itself. */
#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/console.h"
#include "backmux/store.h"
#include "backmux/platform.h"
#include "backmux/core.h"
#include "launch_values.h"

/* ---- launch-option value parsing ---------------------------------------- */

int cli_parse_restart_policy_arg(const char *arg, char out[64]) {
    if (!arg || !*arg) {
        fprintf(stderr, "backmux: error: --restart requires a policy\n");
        return HOLD_EXIT_USAGE;
    }
    if (!strcmp(arg, "no") || !strcmp(arg, "always") || !strcmp(arg, "unless-stopped")) {
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    if (!strncmp(arg, "on-failure", 10) && (arg[10] == '\0' || arg[10] == ':')) {
        if (arg[10] == ':') {
            char *end = NULL;
            long n = strtol(arg + 11, &end, 10);
            if (!end || *end || n < 0 || n > INT_MAX) {
                fprintf(stderr, "backmux: error: invalid --restart on-failure retry count '%s'\n", arg + 11);
                return HOLD_EXIT_USAGE;
            }
        }
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    fprintf(stderr, "backmux: error: invalid --restart policy '%s'\n", arg);
    return HOLD_EXIT_USAGE;
}

bool cli_restart_policy_arg_enabled(const char *arg) {
    return arg && *arg && strcmp(arg, "no") != 0;
}

int cli_parse_restart_delay_arg(const char *arg, int *out) {
    if (!arg || !*arg) {
        fprintf(stderr, "backmux: error: --restart-delay requires seconds\n");
        return HOLD_EXIT_USAGE;
    }
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) {
        fprintf(stderr, "backmux: error: invalid --restart-delay '%s'\n", arg);
        return HOLD_EXIT_USAGE;
    }
    *out = (int)n;
    return 0;
}

/* One detach-key token: a literal byte, ^X, ctrl-x / ctrl-X, or ctrl-space
 * (NUL, the default prefix). */
static int cli_detach_key_token_value(const char *token, size_t len, unsigned char *out) {
    if (!token || len == 0 || !out) return -1;
    if (len == 10 && !strncasecmp(token, "ctrl-space", 10)) {
        *out = 0x00;
        return 0;
    }
    if (len == 1) {
        *out = (unsigned char)token[0];
        return 0;
    }
    if (len == 2 && token[0] == '^' && token[1] >= '@' && token[1] <= '_') {
        *out = (unsigned char)(token[1] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-') {
        if (token[5] >= '@' && token[5] <= '_') {
            *out = (unsigned char)(token[5] - '@');
            return 0;
        }
        if (token[5] >= 'a' && token[5] <= 'z') {
            *out = (unsigned char)(token[5] - 'a' + 1);
            return 0;
        }
    }
    return -1;
}

int cli_configure_detach_keys_option(const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "backmux: error: --detach-keys requires a key sequence\n");
        return HOLD_EXIT_USAGE;
    }
    unsigned char keys[8];
    size_t nkeys = 0;
    const char *p = value;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        if (nkeys >= sizeof(keys) || cli_detach_key_token_value(start, (size_t)(p - start), &keys[nkeys]) != 0) {
            fprintf(stderr, "backmux: error: invalid --detach-keys sequence '%s'\n", value);
            return HOLD_EXIT_USAGE;
        }
        nkeys++;
    }
    if (nkeys == 0 || hold_console_set_detach_keys(keys, nkeys) != 0) {
        fprintf(stderr, "backmux: error: invalid --detach-keys sequence '%s'\n", value);
        return HOLD_EXIT_USAGE;
    }
    return 0;
}

int cli_append_env_assignment(const char *arg, char ***env_out, int *envc_out) {
    const char *eq = arg ? strchr(arg, '=') : NULL;
    if (!arg || !*arg || !eq || eq == arg) {
        fprintf(stderr, "backmux: error: expected KEY=VALUE after --env/-e\n");
        return HOLD_EXIT_USAGE;
    }
    size_t key_len = (size_t)(eq - arg);
    char key[256];
    if (key_len >= sizeof(key)) {
        fprintf(stderr, "backmux: error: environment key is too long\n");
        return HOLD_EXIT_USAGE;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; key[i]; i++) {
        if (key[i] == '=') {
            fprintf(stderr, "backmux: error: invalid environment key\n");
            return HOLD_EXIT_USAGE;
        }
    }
    char *copy = strdup(arg);
    if (!copy) return 1;
    char **next = realloc(*env_out, ((size_t)*envc_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 1;
    }
    next[*envc_out] = copy;
    *env_out = next;
    (*envc_out)++;
    return 0;
}

int cli_append_env_file(const char *path, char ***env_out, int *envc_out) {
    if (!path || !*path) {
        fprintf(stderr, "backmux: error: --env-file requires a path\n");
        return HOLD_EXIT_USAGE;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "backmux: error: cannot read env file '%s': %s\n", path, strerror(errno));
        return HOLD_EXIT_USAGE;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t nr;
    unsigned long lineno = 0;
    int rc = 0;
    while ((nr = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (nr > 0 && (line[nr - 1] == '\n' || line[nr - 1] == '\r')) {
            line[--nr] = '\0';
        }
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        rc = cli_append_env_assignment(p, env_out, envc_out);
        if (rc != 0) {
            fprintf(stderr, "backmux: error: invalid --env-file entry at %s:%lu\n", path, lineno);
            break;
        }
    }
    free(line);
    if (ferror(f) && rc == 0) {
        fprintf(stderr, "backmux: error: failed reading env file '%s': %s\n", path, strerror(errno));
        rc = HOLD_EXIT_USAGE;
    }
    fclose(f);
    return rc;
}

