#include "callbox/config.h"
#include "callbox/types.h"
#include "callbox/runtime.h"
#include "callbox/runtime_internal.h"
#include "callbox/core.h"
#include "callbox/store.h"

/* callbox config: a person's saved defaults (store/settings.c keeps the file).
 * Each setting has one grammar, checked here before anything is written, so
 * a refused value never replaces a saved one. */

static const char config_usage[] =
    "usage: callbox config get <setting>\n"
    "       callbox config set <setting> <value>\n"
    "       callbox config unset <setting>\n"
    "settings: detach-size COLS ROWS\n";

/* The words after the setting's name -> the one-line form that is saved. */
typedef int (*setting_parse)(int argc, char **argv, char *out, size_t n);

static int parse_size_value(int argc, char **argv, char *out, size_t n) {
    static const char shape[] = "a size is COLS ROWS in character cells (for example 120 40)";
    int cols = 0, rows = 0;
    char why[160];
    snprintf(why, sizeof(why), "%s", shape);
    int used = argc >= 1 && argc <= 2
                   ? hold_parse_term_size(argv[0], argc > 1 ? argv[1] : NULL, &cols, &rows, why, sizeof(why))
                   : -1;
    if (used > 0 && used != argc) { /* a whole size, then a stray word */
        snprintf(why, sizeof(why), "%s", shape);
        used = -1;
    }
    if (used < 0) {
        fprintf(stderr, "callbox: error: %s\n", why);
        return -1;
    }
    snprintf(out, n, "%d %d", cols, rows);
    return 0;
}

static const struct setting {
    const char *name;
    setting_parse parse;
    const char *unset_note;
} settings[] = {
    {"detach-size", parse_size_value,
     "detach-size is not set: a detached PTY call takes the invoking terminal's size, or 80 24 without one"},
};

static const struct setting *find_setting(const char *name) {
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
        if (!strcmp(settings[i].name, name)) return &settings[i];
    fprintf(stderr, "callbox: error: unknown setting '%s'\n%s", name, config_usage);
    return NULL;
}

/* Whose settings: the person at the keyboard - under sudo the invoking user
 * (read only: see hold_cmd_config_action). */
static const char *settings_home(const struct hold_invocation *inv) {
    if (inv && inv->euid_root && inv->have_sudo_user && inv->invoking_home[0]) return inv->invoking_home;
    return getenv("HOME");
}

int hold_saved_detach_size(const struct hold_invocation *inv, int *cols, int *rows) {
    const char *home = settings_home(inv);
    char value[64], why[160];
    if (!home || !*home) return 1;
    int rc = hold_settings_get(home, "detach-size", value, sizeof(value));
    if (rc == 1) return 1;
    char *second = rc == 0 ? strchr(value, ' ') : NULL;
    if (second) *second++ = '\0';
    if (rc != 0 || hold_parse_term_size(value, second, cols, rows, why, sizeof(why)) != (second ? 2 : 1)) {
        /* A saved value that no longer reads as a size (a hand edit) is not
         * guessed at: nothing starts until it is set again or cleared. */
        fprintf(stderr, "callbox: error: the saved detach-size cannot be used%s%s\n"
                        "  set it again:  callbox config set detach-size COLS ROWS\n"
                        "  or clear it:   callbox config unset detach-size\n",
                rc != 0 ? ": " : "", rc != 0 ? strerror(errno) : "");
        return -1;
    }
    return 0;
}

int hold_cmd_config_action(const struct hold_invocation *inv, int argc, char **argv) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return HOLD_RC_SHOW_HELP;
    if (argc < 2) {
        fputs(config_usage, stderr);
        return 1;
    }
    const char *verb = argv[0];
    bool get = !strcmp(verb, "get"), set = !strcmp(verb, "set"), unset = !strcmp(verb, "unset");
    if ((!get && !set && !unset) || ((get || unset) && argc != 2) || (set && argc < 3)) {
        fputs(config_usage, stderr);
        return 1;
    }
    const struct setting *s = find_setting(argv[1]);
    if (!s) return 1;
    const char *home = settings_home(inv);
    if (!home || !*home) {
        fprintf(stderr, "callbox: error: HOME is not set\n");
        return 3;
    }
    char value[128];
    if (get) {
        int rc = hold_settings_get(home, s->name, value, sizeof(value));
        if (rc < 0) {
            fprintf(stderr, "callbox: error: cannot read settings: %s\n", strerror(errno));
            return 3;
        }
        if (rc == 1) {
            hold_sig_note(inv, "callbox: %s\n", s->unset_note);
            return 1;
        }
        printf("%s\n", value);
        return 0;
    }
    if (set && s->parse(argc - 2, argv + 2, value, sizeof(value)) != 0) return 1;
    /* A person's settings are changed as that person: root does not write
     * into the invoking user's home on their say-so. */
    if (inv && inv->euid_root && inv->have_sudo_user) {
        fprintf(stderr, "callbox: error: these are %s's own settings; run callbox config %s without sudo\n",
                inv->invoking_user, verb);
        return 2;
    }
    if (hold_settings_put(home, s->name, set ? value : NULL) != 0) {
        fprintf(stderr, "callbox: error: cannot save settings: %s\n", strerror(errno));
        return 3;
    }
    if (set) hold_sig_note(inv, "callbox: %s is %s for new calls; running calls keep their size\n", s->name, value);
    else hold_sig_note(inv, "callbox: %s cleared\n", s->name);
    return 0;
}
