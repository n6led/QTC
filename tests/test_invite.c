#include "test.h"
#include "qtc/invite.h"
#include "qtc/util.h"
#include <errno.h>

int main(void) {
    uint8_t key[16], parsed[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    char uri[QTC_MAX_URI], name[33], message[QTC_MAX_TEXT], parsed_uri[QTC_MAX_URI];
    ASSERT_EQ_INT(qtc_channel_uri_build("Family & Friends", key, uri, sizeof(uri)), 0);
    ASSERT_TRUE(strstr(uri, "name=Family%20%26%20Friends") != NULL);
    ASSERT_EQ_INT(qtc_channel_uri_parse(uri, name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Family & Friends"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_EQ_INT(qtc_invite_message_build(uri, message, sizeof(message)), 0);
    ASSERT_EQ_INT(qtc_invite_message_parse(message, parsed_uri, sizeof(parsed_uri), name, sizeof(name), parsed), 0);
    ASSERT_STREQ(parsed_uri, uri); ASSERT_STREQ(name, "Family & Friends");
    ASSERT_EQ_INT(qtc_channel_join_parse("000102030405060708090a0b0c0d0e0f", name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Private-00010203"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_EQ_INT(qtc_channel_join_parse("Workshop:000102030405060708090a0b0c0d0e0f", name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Workshop"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_TRUE(qtc_channel_join_parse("not-a-key", name, sizeof(name), parsed) != 0);
    ASSERT_TRUE(qtc_channel_uri_parse("meshcore://channel/add?name=x&secret=1234", name, sizeof(name), parsed) != 0);
    qtc_state state = {0}; state.channel_count = 2; state.channels[0].index = 0; state.channels[0].configured = true;
    state.channels[1].index = 1; state.channels[1].configured = true;
    ASSERT_EQ_INT(qtc_find_free_channel_slot(&state, 8), 2);
    ASSERT_EQ_INT(qtc_channel_join_parse("#test", name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "#test");
    const uint8_t known[16] = {0x9c,0xd8,0xfc,0xf2,0x2a,0x47,0x33,0x3b,0x59,0x1d,0x96,0xa2,0xb8,0x48,0xb7,0x3f};
    ASSERT_TRUE(memcmp(parsed, known, 16) == 0);
    const char *bad_tags[] = {"#", "#two words", "##test", "#bad\nname", "#123456789012345678901234567890123"};
    for (size_t i = 0; i < QTC_ARRAY_LEN(bad_tags); i++)
        ASSERT_TRUE(qtc_channel_join_parse(bad_tags[i], name, sizeof(name), parsed) != 0);
    const struct { const char *name; const char *hex; } vectors[] = {
    {"#hamradio", "83c8b01997654265938da8765cbc7db9"},
    {"#centralvalley", "4b6e593421c03d58713c77d5510d0850"},
    {"#HamRadio", "ea9d4728a29589b418e513c8e115d8b6"},
    {"#aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ce3224f05ad324b9838132b7d5c4bbde"},
    {"#café", "7dc4f56faecbda50558edb29c331db26"}
    };
    for (size_t i = 0; i < QTC_ARRAY_LEN(vectors); i++) {
        ASSERT_EQ_INT(qtc_channel_join_parse(vectors[i].name, name, sizeof(name), parsed), 0);
        ASSERT_STREQ(name, vectors[i].name);
        char hex[33]; qtc_hex_encode(parsed, 16, hex, sizeof(hex)); ASSERT_STREQ(hex, vectors[i].hex);
    }
    ASSERT_TRUE(qtc_channel_join_parse("#\xff", name, sizeof(name), parsed) != 0);
    puts("invite tests passed"); return 0;
}
