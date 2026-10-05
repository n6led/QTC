/* Deterministic command ownership and mutation tests. */
#define qtc_now_millis test_now_millis
#include "../src/core.c"
#undef qtc_now_millis
#include "test.h"
static int64_t now_ms = 100000;
int64_t test_now_millis(void) { return now_ms; }

static void parser_tests(void) {
    uint8_t card[QTC_MAX_FRAME], cmd[QTC_MAX_FRAME]; size_t n;
    ASSERT_TRUE(!qtc_contact_card_decode("meshcore://00aAbBFf", card, sizeof(card), &n));
    ASSERT_EQ_INT(n, 4); ASSERT_EQ_INT(card[1], 0xaa); ASSERT_EQ_INT(card[3], 255);
    ASSERT_EQ_INT(qtc_cmd_import_contact(cmd, sizeof(cmd), card, n), 5);
    ASSERT_EQ_INT(cmd[0], 18); ASSERT_TRUE(!memcmp(cmd + 1, card, n));
    const char *bad[] = {"", "00", "http://00", "meshcore://", "meshcore://0", "meshcore://0g", "meshcore:// 0"};
    for (size_t i = 0; i < QTC_ARRAY_LEN(bad); i++) {
        memset(card, 0xcc, sizeof(card)); n = 123;
        ASSERT_TRUE(qtc_contact_card_decode(bad[i], card, sizeof(card), &n));
        ASSERT_EQ_INT(n, 0); ASSERT_EQ_INT(card[0], 0xcc);
    }
    char uri[QTC_CONTACT_CARD_URI_SIZE + 2]; strcpy(uri, "meshcore://");
    memset(uri + 11, 'a', sizeof(uri) - 12); uri[sizeof(uri) - 1] = 0;
    ASSERT_TRUE(qtc_contact_card_decode(uri, card, sizeof(card), &n)); ASSERT_EQ_INT(n, 0);
    uri[QTC_CONTACT_CARD_URI_SIZE - 1] = 0;
    ASSERT_TRUE(!qtc_contact_card_decode(uri, card, sizeof(card), &n));
    ASSERT_EQ_INT(n, QTC_MAX_FRAME - 1);
    ASSERT_EQ_INT(qtc_cmd_import_contact(cmd, sizeof(cmd), card, n), QTC_MAX_FRAME);
    ASSERT_TRUE(qtc_contact_card_decode(uri, card, 1, &n));
    ASSERT_EQ_INT(qtc_cmd_import_contact(cmd, sizeof(cmd), card, 0), 0);
    ASSERT_EQ_INT(qtc_cmd_import_contact(cmd, sizeof(cmd), card, QTC_MAX_FRAME), 0);
    ASSERT_EQ_INT(qtc_cmd_import_contact(cmd, 1, card, 1), 0);
}
static void silent(int peer) {
    struct pollfd p = {.fd = peer, .events = POLLIN}; ASSERT_EQ_INT(poll(&p, 1, 0), 0);
}
static core_ctx *setup(int *peer, int *client) {
    int radio[2], ipc[2];
    ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, radio), 0);
    ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, ipc), 0);
    core_ctx *c = calloc(1, sizeof(*c)); ASSERT_TRUE(c);
    c->serial.fd = radio[0]; *peer = radio[1]; *client = ipc[1];
    c->state.radio_connected = true; c->session_phase = RADIO_SESSION_READY;
    c->clients[0].fd = ipc[0]; c->clients[0].active = c->clients[0].hello_ok = true;
    c->clipboard_client = -1;
    ASSERT_EQ_INT(qtc_db_open(&c->db, ":memory:"), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&c->db), 0);
    return c;
}
static void submit(core_ctx *c) {
    qtc_ipc_frame f = {.type = QTC_IPC_IMPORT_CONTACT, .length = 4, .payload = {0, 0xaa, 0xbb, 255}};
    handle_client_frame(&f, c);
}
static void cleanup(core_ctx *c, int peer, int client) {
    if (c->serial.fd >= 0) close(c->serial.fd);
    qtc_db_close(&c->db);
    close(c->clients[0].fd); close(peer); close(client); free(c);
}
int main(void) {
    parser_tests();
    int peer, client; core_ctx *c = setup(&peer, &client);
    /* An import queued behind another request cannot take its OK response. */
    uint8_t name[] = {8, 'x'};
    ASSERT_EQ_INT(queue_radio(c, name, sizeof(name), QTC_RADIO_OK, QTC_RADIO_ERROR, QTC_RADIO_NONE, ""), 0);
    service_radio_queue(c); uint8_t wire[QTC_MAX_FRAME + 3];
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
    submit(c); submit(c); ASSERT_EQ_INT(c->queue_count, 1); silent(peer);
    uint8_t ok[] = {0}; radio_frame_cb(ok, sizeof(ok), c);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 8);
    ASSERT_EQ_INT(wire[3], 18); ASSERT_EQ_INT(wire[5], 0xaa);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.purpose, RADIO_PURPOSE_IMPORT_CONTACT);
    ASSERT_EQ_INT(radio_command_transport_retries(&c->pending), 0);
    ASSERT_TRUE(!c->contacts_sync_needed); ASSERT_EQ_INT(c->state.contact_count, 0);
    int64_t since = c->pending_since;
    submit(c); ASSERT_EQ_INT(c->queue_count, 0);
    uint8_t push[] = {0x83}; radio_frame_cb(push, sizeof(push), c);
    ASSERT_EQ_INT(c->pending.data[0], 18); ASSERT_EQ_INT(c->pending_since, since);
    now_ms = since + 1500; service_radio_queue(c); silent(peer);
    radio_frame_cb(ok, sizeof(ok), c);
    ASSERT_TRUE(strstr(c->last_status, "Contact imported")); ASSERT_TRUE(c->contacts_sync_needed);
    /* Drain the asynchronous inbox push, then use the ordinary contact refresh. */
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4); ASSERT_EQ_INT(wire[3], 10);
    uint8_t empty[] = {10}; radio_frame_cb(empty, sizeof(empty), c);
    now_ms = c->background_sync_after; service_background_sync(c); service_radio_queue(c);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 8); ASSERT_EQ_INT(wire[3], 4);
    ASSERT_EQ_INT(c->state.contact_count, 0); /* No locally invented contact. */
    qtc_radio_event contact = {.type = QTC_RADIO_CONTACT, .value = 123};
    strcpy(contact.contact.id, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    strcpy(contact.contact.name, "Imported person"); contact.contact.node_type = QTC_NODE_PERSON;
    radio_event(c, &contact); ASSERT_EQ_INT(c->state.contact_count, 1);
    ASSERT_STREQ(c->state.contacts[0].name, "Imported person");
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.data[0], 4);
    contact.type = QTC_RADIO_CONTACT_END; radio_event(c, &contact);
    ASSERT_TRUE(!c->radio_pending); ASSERT_EQ_INT(c->contact_since, 123);
    cleanup(c, peer, client);
    for (int error = 1; error <= 7; error++) {
        c = setup(&peer, &client); submit(c); service_radio_queue(c);
        ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 8);
        uint8_t err[] = {1, (uint8_t)error}; radio_frame_cb(err, sizeof(err), c);
        ASSERT_TRUE(!c->radio_pending); ASSERT_TRUE(!c->contacts_sync_needed);
        ASSERT_TRUE(strstr(c->last_status, "Contact import failed"));
        if (error == 3) ASSERT_TRUE(strstr(c->last_status, "table full"));
        silent(peer); cleanup(c, peer, client);
    }
    c = setup(&peer, &client); c->state.radio_connected = false; submit(c);
    qtc_ipc_frame reply; ASSERT_EQ_INT(qtc_ipc_recv_blocking(client, &reply, 1000), 0);
    ASSERT_EQ_INT(reply.type, QTC_IPC_ERROR); ASSERT_EQ_INT(c->queue_count, 0); silent(peer);
    cleanup(c, peer, client);
    c = setup(&peer, &client); submit(c); service_radio_queue(c);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 8);
    now_ms = c->pending_since + 1501; service_radio_queue(c);
    ASSERT_TRUE(!c->radio_pending); ASSERT_EQ_INT(c->serial.fd, -1);
    ASSERT_TRUE(strstr(c->last_status, "outcome unknown"));
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 0); cleanup(c, peer, client);
    puts("contact import tests passed"); return 0;
}
