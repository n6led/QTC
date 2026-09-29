#ifndef QTC_MENTION_H
#define QTC_MENTION_H
#include <stdbool.h>
#include <stddef.h>

bool qtc_mention_name_valid(const char *name);
/* Returns the byte length of a complete @[Name] at text, or zero. */
size_t qtc_mention_length(const char *text);
int qtc_mention_prefix(const char *name, char *out, size_t capacity);
/* Conventional, unverified channel display sender; never modifies text. */
bool qtc_channel_sender(const char *text, char *out, size_t capacity);
#endif
