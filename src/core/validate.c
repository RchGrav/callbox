#include "backmux/config.h"
#include "backmux/types.h"
#include "backmux/core.h"

static bool all_lower_hex(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return false;
    }
    return true;
}

bool hold_valid_id(const char *id) {
    return id && strlen(id) == ID_HEX_LEN && all_lower_hex(id, ID_HEX_LEN);
}

bool hold_record_json_filename_id(const char *name, char *id, size_t n) {
    if (!name || !hold_has_suffix(name, ".json")) return false;
    size_t id_len = strlen(name) - 5;
    if (id_len + 1 > n) return false;
    memcpy(id, name, id_len);
    id[id_len] = '\0';
    return hold_valid_id(id);
}

bool hold_valid_id_prefix(const char *id) {
    if (!id) return false;
    size_t len = strlen(id);
    return len >= 1 && len <= ID_HEX_LEN && all_lower_hex(id, len);
}

/* Process and inbox names exclude full process IDs to avoid ambiguity with
 * ID resolution. A leading hyphen is also invalid because it denotes an option. */
bool hold_valid_alias(const char *alias) {
    if (!alias) return false;
    size_t len = strlen(alias);
    /* A leading dash would read as a flag wherever the name is a target. */
    if (len == 0 || len > ALIAS_MAX_LEN || alias[0] == '-' || hold_valid_id(alias)) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)alias[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

/* A name as help shows it ("Sleek Carbon") back to the name: lower case,
 * spaces for underscores. Why: help shows names in that polite form, so
 * whoever types one back - in a command, or in chat's recipient search -
 * must reach the process. True only when the word needed it (a space or a
 * capital) and the result is a valid name. */
bool hold_friendly_name(const char *word, char *out, size_t n) {
    if (!word || !out || n == 0) return false;
    size_t len = strlen(word);
    if (len >= n) return false;
    bool changed = false;
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = (unsigned char)word[i];
        if (c == ' ') c = '_', changed = true;
        else if (isupper(c)) c = (unsigned char)tolower(c), changed = true;
        out[i] = (char)c;
    }
    return changed && hold_valid_alias(out);
}

/* Whether s starts with the first n bytes of want, as names are searched:
 * any case, and a space matches an underscore, so a name typed as help
 * shows it ("sleek carbon") finds sleek_carbon (chat's recipient search). */
bool hold_name_starts_like(const char *s, const char *want, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int a = tolower((unsigned char)s[i]), b = tolower((unsigned char)want[i]);
        if (a == ' ') a = '_';
        if (b == ' ') b = '_';
        if (!s[i] || a != b) return false;
    }
    return true;
}

/* The polite form of a name, for people and agents to read: "sleek_carbon"
 * shows as "Sleek Carbon". The underscore form exists for the Docker
 * resemblance; help shows this one (hold_friendly_name reads it back). */
void hold_friendly_display(const char *name, char *out, size_t n) {
    if (!out || n == 0) return;
    size_t i = 0;
    bool start = true;
    for (; name && name[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '_') {
            out[i] = ' ';
            start = true;
            continue;
        }
        out[i] = (char)(start ? toupper(c) : c);
        start = c == '-';
    }
    out[i] = '\0';
}

bool hold_valid_record(const struct hold_run_record *r) {
    return r && r->pid > 0 && r->pgid > 1 && hold_valid_id(r->id) &&
           (r->run_id[0] == '\0' || hold_valid_id(r->run_id));
}

static int parse_id_number_env(const char *s, unsigned long long *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) return -1;
    *out = v;
    return 0;
}

int hold_parse_uid_env(const char *s, uid_t *out) {
    unsigned long long v = 0;
    if (parse_id_number_env(s, &v) != 0) return -1;
    uid_t narrowed = (uid_t)v;
    if ((unsigned long long)narrowed != v) return -1;
    *out = narrowed;
    return 0;
}

int hold_parse_gid_env(const char *s, gid_t *out) {
    unsigned long long v = 0;
    if (parse_id_number_env(s, &v) != 0) return -1;
    gid_t narrowed = (gid_t)v;
    if ((unsigned long long)narrowed != v) return -1;
    *out = narrowed;
    return 0;
}
