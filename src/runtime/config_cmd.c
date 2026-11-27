#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/runtime.h"
#include "backmux/runtime_internal.h"
#include "backmux/core.h"
#include "backmux/store.h"
#include "backmux/console.h"

/* User defaults are validated before store/settings.c writes them. */

static const char config_usage[] =
    "usage: backmux config get <setting>\n"
    "       backmux config set <setting> <value>\n"
    "       backmux config unset <setting>\n"
    "settings: detach-size COLS ROWS, chat-name NAME, agents NAME..., slide on|off\n";

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
        fprintf(stderr, "backmux: error: %s\n", why);
        return -1;
    }
    snprintf(out, n, "%d %d", cols, rows);
    return 0;
}

/* Preferred name when creating the account's chat inbox. */
static int parse_name_value(int argc, char **argv, char *out, size_t n) {
    if (argc != 1 || !hold_valid_alias(argv[0])) {
        fprintf(stderr, "backmux: error: chat-name requires 1-%d letters, digits, underscores, or hyphens; "
                        "it cannot start with - or be a full process ID\n", ALIAS_MAX_LEN);
        return -1;
    }
    snprintf(out, n, "%s", argv[0]);
    return 0;
}

/* Program names that are agent harnesses besides the ones backmux knows
 * (claude, codex, ...): chat lists processes that run them. --agent adds
 * to this list; set replaces it. */
static int parse_agents_value(int argc, char **argv, char *out, size_t n) {
    size_t k = 0;
    out[0] = '\0';
    for (int i = 0; i < argc; i++)
        for (const char *p = argv[i]; *p;) {
            while (*p == ' ' || *p == ',') p++;
            size_t l = strcspn(p, " ,");
            if (!l) break;
            for (size_t j = 0; j < l; j++)
                if (!isalnum((unsigned char)p[j]) && p[j] != '-' && p[j] != '_' && p[j] != '.') {
                    fprintf(stderr, "backmux: error: agents are program names: letters, digits, - _ and .\n");
                    return -1;
                }
            if (l >= AGENT_NAME_MAX || k + l + 2 > n) {
                fprintf(stderr, "backmux: error: agent names are at most %d characters, the list 1000\n",
                        AGENT_NAME_MAX - 1);
                return -1;
            }
            k += (size_t)snprintf(out + k, n - k, "%s%.*s", k ? " " : "", (int)l, p);
            p += l;
        }
    if (!k) {
        fprintf(stderr, "backmux: error: agents needs at least one program name\n");
        return -1;
    }
    return 0;
}

/* on or off: whether the compositor's view slides when it moves (it does
 * unless this is off). */
static int parse_slide_value(int argc, char **argv, char *out, size_t n) {
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off"))) {
        fprintf(stderr, "backmux: error: slide is on or off\n");
        return -1;
    }
    snprintf(out, n, "%s", argv[0]);
    return 0;
}

static const struct setting {
    const char *name;
    setting_parse parse;
    const char *unset_note;
} settings[] = {
    {"detach-size", parse_size_value,
     "detach-size is unset: detached terminal processes use the invoking terminal's size, or 80 columns by 24 rows without one"},
    {"chat-name", parse_name_value, "chat-name is unset: new chat inboxes receive generated names"},
    {"agents", parse_agents_value, "agents is unset: chat lists only the harnesses backmux knows"},
    {"slide", parse_slide_value, "slide is unset: the view slides when it moves"},
};

static const struct setting *find_setting(const char *name) {
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
        if (!strcmp(settings[i].name, name)) return &settings[i];
    fprintf(stderr, "backmux: error: unknown setting '%s'\n%s", name, config_usage);
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
        fprintf(stderr, "backmux: error: the saved detach-size cannot be used%s%s\n"
                        "  set it again:  backmux config set detach-size COLS ROWS\n"
                        "  or clear it:   backmux config unset detach-size\n",
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
        fprintf(stderr, "backmux: error: HOME is not set\n");
        return 3;
    }
    char value[128];
    if (get) {
        int rc = hold_settings_get(home, s->name, value, sizeof(value));
        if (rc < 0) {
            fprintf(stderr, "backmux: error: cannot read settings: %s\n", strerror(errno));
            return 3;
        }
        if (rc == 1) {
            hold_sig_note(inv, "backmux: %s\n", s->unset_note);
            return 1;
        }
        printf("%s\n", value);
        return 0;
    }
    if (set && s->parse(argc - 2, argv + 2, value, sizeof(value)) != 0) return 1;
    /* Under sudo, config reads the invoking user's settings but does not
     * write them as root. */
    if (inv && inv->euid_root && inv->have_sudo_user) {
        fprintf(stderr, "backmux: error: settings belong to %s; run backmux config %s without sudo to change them\n",
                inv->invoking_user, verb);
        return 2;
    }
    if (hold_settings_put(home, s->name, set ? value : NULL) != 0) {
        fprintf(stderr, "backmux: error: cannot save settings: %s\n", strerror(errno));
        return 3;
    }
    if (set) hold_sig_note(inv, "backmux: %s set to %s\n", s->name, value);
    else hold_sig_note(inv, "backmux: %s cleared\n", s->name);
    return 0;
}
