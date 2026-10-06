#ifndef QTC_CHANNEL_H
#define QTC_CHANNEL_H
#include "qtc/qtc.h"

/* Full secret, normalized lowercase; never print this identity in logs. */
void qtc_channel_key(const uint8_t secret[16], char out[QTC_MAX_ID]);
qtc_channel *qtc_channel_find(qtc_state *state, const char *key);
int qtc_hashtag_secret(const char *name, uint8_t secret[16]);
#endif
