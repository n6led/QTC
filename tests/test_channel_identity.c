#include "../src/core.c"
#include "test.h"

static void sql(qtc_db *db, const char *s) { ASSERT_EQ_INT(sqlite3_exec(db->db, s, NULL, NULL, NULL), SQLITE_OK); }
static void history_key(qtc_db *db, const char *text, const char *key) {
    sqlite3_stmt *st = NULL;
    ASSERT_EQ_INT(sqlite3_prepare_v2(db->db, "SELECT conversation_key FROM messages WHERE text=?", -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, text, -1, SQLITE_TRANSIENT);
    ASSERT_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    ASSERT_STREQ((const char *)sqlite3_column_text(st, 0), key);
    sqlite3_finalize(st);
}
static void add_history(qtc_db *db, int kind, const char *key, const char *text) {
    qtc_message m = {.conversation_kind = kind, .direction = QTC_MSG_INCOMING, .status = QTC_MSG_DELIVERED};
    strcpy(m.conversation_key, key); strcpy(m.message_key, text); strcpy(m.text, text);
    bool inserted; ASSERT_EQ_INT(qtc_db_insert_message(db, &m, &inserted), 0); ASSERT_TRUE(inserted);
}
static void migration_tests(void) {
    qtc_db db; ASSERT_EQ_INT(qtc_db_open(&db, ":memory:"), 0); ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    qtc_channel a = {.index = 6, .configured = true, .secret = {1}, .unread = 4}; strcpy(a.name, "same name");
    qtc_channel b = a; b.secret[15] = 2; /* Same name and initial key bytes, different full secret. */
    char ka[QTC_MAX_ID], kb[QTC_MAX_ID]; qtc_channel_key(a.secret, ka); qtc_channel_key(b.secret, kb);
    ASSERT_TRUE(strcmp(ka, kb)); ASSERT_EQ_INT(qtc_db_upsert_channel(&db, &a), 0);
    add_history(&db, QTC_CONV_CHANNEL, "6", "old A");
    add_history(&db, QTC_CONV_CHANNEL, "7", "orphan");
    add_history(&db, QTC_CONV_CONTACT, "6", "direct");
    sql(&db, "UPDATE schema_meta SET value='10' WHERE key='schema_version'");
    sql(&db, "CREATE TRIGGER fail_migration BEFORE UPDATE OF conversation_key ON messages BEGIN SELECT RAISE(ABORT,'test rollback'); END");
    ASSERT_TRUE(qtc_db_migrate(&db) != 0); history_key(&db, "old A", "6");
    sql(&db, "DROP TRIGGER fail_migration");
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0); ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    history_key(&db, "old A", ka); history_key(&db, "direct", "6"); history_key(&db, "orphan", "7");
    ASSERT_EQ_INT(qtc_db_remove_channel(&db, 6), 0); history_key(&db, "old A", ka);
    a.index = 7; ASSERT_EQ_INT(qtc_db_upsert_channel(&db, &a), 0);
    ASSERT_EQ_INT(qtc_db_upsert_channel(&db, &b), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0); history_key(&db, "orphan", "7");
    history_key(&db, "old A", ka);
    add_history(&db, QTC_CONV_CHANNEL, kb, "new B"); history_key(&db, "new B", kb);
    qtc_state *state = calloc(1, sizeof(*state)); ASSERT_TRUE(state);
    ASSERT_EQ_INT(qtc_db_load_state(&db, state), 0);
    ASSERT_EQ_INT(qtc_channel_find(state, ka)->index, 7);
    ASSERT_EQ_INT(qtc_channel_find(state, kb)->index, 6);
    ASSERT_TRUE(!qtc_channel_find(state, "6"));
    /* Replacing a slot must not inherit the previous identity's unread count. */
    b.index = 7; b.unread = 0; ASSERT_EQ_INT(qtc_db_upsert_channel(&db, &b), 0);
    ASSERT_EQ_INT(qtc_db_load_state(&db, state), 0);
    for (size_t i = 0; i < state->channel_count; i++) if (state->channels[i].index == 7) ASSERT_EQ_INT(state->channels[i].unread, 0);
    free(state); qtc_db_close(&db);
}
static core_ctx *setup(int *peer) {
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    core_ctx *c = calloc(1, sizeof(*c)); ASSERT_TRUE(c);
    c->serial.fd = pair[0]; *peer = pair[1]; c->clipboard_client = -1;
    c->session_phase = RADIO_SESSION_READY; c->state.radio_connected = true;
    ASSERT_EQ_INT(qtc_db_open(&c->db, ":memory:"), 0); ASSERT_EQ_INT(qtc_db_migrate(&c->db), 0);
    c->state.channel_count = 1;
    c->state.channels[0] = (qtc_channel){.index = 6, .configured = true, .secret = {1}};
    strcpy(c->state.channels[0].name, "A");
    ASSERT_EQ_INT(qtc_db_upsert_channel(&c->db, &c->state.channels[0]), 0);
    return c;
}
static void done(core_ctx *c, int peer) { close(c->serial.fd); close(peer); qtc_db_close(&c->db); free(c); }
static void silent(int peer) { struct pollfd p = {.fd = peer, .events = POLLIN}; ASSERT_EQ_INT(poll(&p, 1, 0), 0); }
static void transport_tests(void) {
    int peer; core_ctx *c = setup(&peer); uint8_t wire[512];
    char ka[QTC_MAX_ID]; qtc_channel_key(c->state.channels[0].secret, ka);
    c->channel_verified[6] = true;
    /* Queue while another command owns the response, then move A to slot 7. */
    c->radio_pending = true; c->pending_since = qtc_now_millis();
    c->pending.data[0] = 20; c->pending.len = 1; c->pending.expected_a = QTC_RADIO_BATTERY;
    ASSERT_EQ_INT(send_channel_action(c, ka, "sent to A"), 0); silent(peer);
    c->state.channels[0].index = 7; c->channel_verified[7] = true;
    qtc_radio_event battery = {.type = QTC_RADIO_BATTERY}; radio_event(c, &battery);
    ASSERT_TRUE(read(peer, wire, sizeof(wire)) > 6); ASSERT_EQ_INT(wire[3], 3); ASSERT_EQ_INT(wire[5], 7);
    history_key(&c->db, "sent to A", ka);
    qtc_radio_event ok = {.type = QTC_RADIO_OK}; radio_event(c, &ok);
    /* A stale draft cannot send to B just because it now occupies A's slot. */
    c->state.channels[0].secret[15] = 2;
    ASSERT_TRUE(send_channel_action(c, ka, "must not send") != 0); silent(peer);
    done(c, peer);

    c = setup(&peer); c->channel_verified[6] = true;
    c->radio_pending = true; c->pending_since = qtc_now_millis();
    c->pending.data[0] = 20; c->pending.len = 1; c->pending.expected_a = QTC_RADIO_BATTERY;
    ASSERT_EQ_INT(send_channel_action(c, ka, "queued A"), 0);
    c->state.channels[0].secret[15] = 2;
    radio_event(c, &battery); silent(peer);
    ASSERT_EQ_INT(c->state.messages[0].status, QTC_MSG_FAILED);
    done(c, peer);

    c = setup(&peer);
    /* Join writes BOTH the hashtag name and its standard secret to the radio. */
    c->clients[0].hello_ok = true; c->state.radio_max_channels = 8;
    qtc_ipc_channel_action_payload join = {0}; strcpy(join.uri, "#hamradio");
    qtc_ipc_frame request = {.type = QTC_IPC_CHANNEL_JOIN, .length = sizeof(join)};
    memcpy(request.payload, &join, sizeof(join)); handle_client_frame(&request, c);
    service_radio_queue(c); ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 53);
    ASSERT_EQ_INT(wire[3], 32); ASSERT_EQ_INT(wire[4], 1); ASSERT_STREQ((char *)wire + 5, "#hamradio");
    uint8_t secret[16]; ASSERT_EQ_INT(qtc_hashtag_secret("#hamradio", secret), 0);
    ASSERT_TRUE(!memcmp(wire + 37, secret, 16));
    done(c, peer);

    c = setup(&peer);
    /* Cached A is not authoritative at startup. Resolve slot 6 before storage. */
    qtc_radio_event rx = {.type = QTC_RADIO_CHANNEL_MESSAGE};
    rx.message.conversation_kind = QTC_CONV_CHANNEL; rx.message.direction = QTC_MSG_INCOMING;
    strcpy(rx.message.conversation_key, "6"); strcpy(rx.message.text, "received B");
    radio_event(c, &rx); ASSERT_TRUE(c->channel_message_pending); ASSERT_EQ_INT(c->state.message_count, 0);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5); ASSERT_EQ_INT(wire[3], 31); ASSERT_EQ_INT(wire[4], 6);
    uint8_t waiting[] = {0x83}; radio_frame_cb(waiting, sizeof(waiting), c);
    ASSERT_EQ_INT(c->queue_count, 0); silent(peer);
    qtc_radio_event info = {.type = QTC_RADIO_CHANNEL_INFO, .channel = {.index = 6, .configured = true, .secret = {2}}};
    strcpy(info.channel.name, "B"); radio_event(c, &info);
    char kb[QTC_MAX_ID]; qtc_channel_key(info.channel.secret, kb);
    history_key(&c->db, "received B", kb); ASSERT_TRUE(!c->channel_message_pending);
    ASSERT_EQ_INT(c->state.channels[0].unread, 1);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4); ASSERT_EQ_INT(wire[3], 10);
    done(c, peer);

    c = setup(&peer); radio_event(c, &rx); ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
    qtc_radio_event error = {.type = QTC_RADIO_ERROR, .error_code = 2}; radio_event(c, &error);
    ASSERT_EQ_INT(c->state.message_count, 1);
    ASSERT_TRUE(!strncmp(c->state.messages[0].conversation_key, "unresolved:", 11));
    char orphan[QTC_MAX_ID]; strcpy(orphan, c->state.messages[0].conversation_key);
    radio_event(c, &info); history_key(&c->db, "received B", orphan);
    done(c, peer);
}
int main(void) { migration_tests(); transport_tests(); puts("channel identity tests passed"); return 0; }
