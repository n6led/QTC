#include "qtc/channel.h"
#include "qtc/util.h"
#include <string.h>

void qtc_channel_key(const uint8_t secret[16], char out[QTC_MAX_ID]) {
    memcpy(out, "channel:", 8);
    qtc_hex_encode(secret, 16, out + 8, QTC_MAX_ID - 8);
}

qtc_channel *qtc_channel_find(qtc_state *state, const char *key) {
    for (size_t i = 0; i < state->channel_count; i++) {
        qtc_channel *ch = &state->channels[i];
        char candidate[QTC_MAX_ID]; qtc_channel_key(ch->secret, candidate);
        if (ch->configured && strcmp(candidate, key) == 0) return ch;
    }
    return NULL;
}

static uint32_t rotr(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }

int qtc_hashtag_secret(const char *name, uint8_t secret[16]) {
    size_t n = strlen(name);
    if (n < 2 || n > 32 || name[0] != '#') return -1;
    for (size_t i = 1; i < n;) {
        uint32_t cp = (unsigned char)name[i++];
        if (cp < 128) {
            if (cp <= 32 || cp == 127 || cp == '#' || cp == ':' || cp == '/' || cp == '?') return -1;
            continue;
        }
        unsigned count; uint32_t minimum;
        if (cp >= 0xc2 && cp <= 0xdf) { count = 1; minimum = 0x80; cp &= 0x1f; }
        else if (cp >= 0xe0 && cp <= 0xef) { count = 2; minimum = 0x800; cp &= 0x0f; }
        else if (cp >= 0xf0 && cp <= 0xf4) { count = 3; minimum = 0x10000; cp &= 7; }
        else return -1;
        if (i + count > n) return -1;
        while (count--) {
            unsigned char c = (unsigned char)name[i++];
            if ((c & 0xc0) != 0x80) return -1;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f) || cp == 0xa0 || cp == 0x2028 || cp == 0x2029) return -1;
    }
    /* SHA-256, first 16 digest bytes. A <=32-byte name needs one padded block.
     * This is deliberately not a general-purpose hashing API. */
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    const uint32_t initial[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t block[64] = {0}; memcpy(block, name, n); block[n] = 0x80;
    block[62] = (uint8_t)((n * 8) >> 8); block[63] = (uint8_t)(n * 8);
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[4*i] << 24) | ((uint32_t)block[4*i+1] << 16) |
               ((uint32_t)block[4*i+2] << 8) | block[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (rotr(x,7) ^ rotr(x,18) ^ (x >> 3)) + w[i-7] +
               (rotr(y,17) ^ rotr(y,19) ^ (y >> 10));
    }
    uint32_t h[8]; memcpy(h, initial, sizeof(h));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h[7] + (rotr(h[4],6) ^ rotr(h[4],11) ^ rotr(h[4],25)) +
                      ((h[4] & h[5]) ^ (~h[4] & h[6])) + k[i] + w[i];
        uint32_t t2 = (rotr(h[0],2) ^ rotr(h[0],13) ^ rotr(h[0],22)) +
                      ((h[0] & h[1]) ^ (h[0] & h[2]) ^ (h[1] & h[2]));
        for (int j = 7; j > 0; j--) h[j] = h[j-1];
        h[4] += t1; h[0] = t1 + t2;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t v = h[i] + initial[i];
        for (int j = 0; j < 4; j++) secret[4*i+j] = (uint8_t)(v >> (24 - 8*j));
    }
    return 0;
}
