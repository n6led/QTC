#include "qtc/mention.h"
#include "qtc/qtc.h"
#include <stdio.h>
#include <string.h>

bool qtc_mention_name_valid(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= QTC_MAX_NAME || name[0] == ' ' || name[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || c == '[' || c == ']') return false;
        if (c >= 128) {
            size_t extra;
            unsigned value, minimum;
            if (c >= 0xc2 && c <= 0xdf) { extra = 1; value = c & 31U; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { extra = 2; value = c & 15U; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { extra = 3; value = c & 7U; minimum = 0x10000; }
            else return false;
            if (i + extra >= n) return false;
            for (size_t j = 0; j < extra; j++) {
                unsigned char next = (unsigned char)name[++i];
                if ((next & 0xc0U) != 0x80U) return false;
                value = (value << 6) | (next & 63U);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
                (value >= 0x80 && value <= 0x9f)) return false;
        }
    }
    return true;
}

size_t qtc_mention_length(const char *text) {
    if (text[0] != '@' || text[1] != '[') return 0;
    const char *end = strchr(text + 2, ']');
    if (!end || end - (text + 2) >= QTC_MAX_NAME) return 0;
    char name[QTC_MAX_NAME];
    size_t n = (size_t)(end - (text + 2));
    memcpy(name, text + 2, n); name[n] = 0;
    return qtc_mention_name_valid(name) ? n + 3 : 0;
}

int qtc_mention_prefix(const char *name, char *out, size_t capacity) {
    if (!qtc_mention_name_valid(name) || strlen(name) + 5 > capacity) return -1;
    (void)snprintf(out, capacity, "@[%s] ", name);
    return 0;
}

bool qtc_channel_sender(const char *text, char *out, size_t capacity) {
    const char *colon = strchr(text, ':');
    if (!colon || colon[1] != ' ') return false;
    size_t n = (size_t)(colon - text);
    /* Companion node names have a 32-byte buffer including NUL. */
    if (!n || n > 31 || n >= capacity) return false;
    char name[32];
    memcpy(name, text, n); name[n] = 0;
    if (!qtc_mention_name_valid(name)) return false;
    memcpy(out, name, n + 1);
    return true;
}
