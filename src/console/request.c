/* console/request.c - the text of a control request: hex-decoded payloads and
 * key=value arguments. Why its own file: parsing a request's text is not
 * serving it, and broker.c may not grow (the 500-line rule). */
#include "backmux/config.h"
#include "backmux/console_internal.h"

int hold_console_hex_value(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

ssize_t hold_console_hex_decode(char *s) {
    size_t n = strlen(s);
    if (n % 2) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hold_console_hex_value((unsigned char)s[i]), lo = hold_console_hex_value((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        s[i / 2] = (char)(hi << 4 | lo);
    }
    s[n / 2] = '\0';
    return (ssize_t)(n / 2);
}

const char *hold_console_kv_get(const char *line, const char *key) {
    size_t kl = strlen(key);
    for (const char *tok = line; tok && *tok;) {
        const char *sp = strchr(tok, ' ');
        if (!strncmp(tok, key, kl) && tok[kl] == '=') {
            return tok + kl + 1;
        }
        tok = sp ? sp + 1 : NULL;
    }
    return NULL;
}
