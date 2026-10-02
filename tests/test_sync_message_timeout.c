/* Exercise timeout boundaries deterministically, without real-time sleeps. */
#define qtc_now_millis test_now_millis
#include "../src/core.c"
#undef qtc_now_millis
#include "test.h"

static int64_t now_ms = 100000;
int64_t test_now_millis(void) { return now_ms; }

static core_ctx *start_inbox(int *peer) {
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    core_ctx *c = calloc(1, sizeof(*c)); ASSERT_TRUE(c != NULL);
    c->serial.fd = pair[0]; *peer = pair[1];
    c->state.radio_connected = true; c->session_phase = RADIO_SESSION_READY;
    c->clipboard_client = -1;
    request_inbox_drain(c, true);
    queue_next_inbox_message(c, RADIO_PRIORITY_INBOX);
    service_radio_queue(c);
    uint8_t wire[8]; ASSERT_EQ_INT(read(*peer, wire, sizeof(wire)), 4);
    ASSERT_EQ_INT(wire[3], 10);
    ASSERT_EQ_INT(radio_command_timeout_ms(&c->pending), 2000);
    ASSERT_EQ_INT(radio_command_transport_retries(&c->pending), 0);
    ASSERT_EQ_INT(c->pending.expected_a, QTC_RADIO_CONTACT_MESSAGE);
    ASSERT_EQ_INT(c->pending.expected_b, QTC_RADIO_CHANNEL_MESSAGE);
    ASSERT_EQ_INT(c->pending.expected_c, QTC_RADIO_NO_MORE_MESSAGES);
    uint8_t other[] = {31, 0};
    ASSERT_EQ_INT(queue_radio(c, other, sizeof(other), QTC_RADIO_CHANNEL_INFO,
                             QTC_RADIO_ERROR, QTC_RADIO_NONE, ""), 0);
    return c;
}

static void no_transmission(int peer) {
    struct pollfd p = {.fd = peer, .events = POLLIN};
    ASSERT_EQ_INT(poll(&p, 1, 0), 0);
}

int main(void) {
    const int ages[] = {1, 300, 1100, 1999};
    for (size_t i = 0; i < QTC_ARRAY_LEN(ages); i++) {
        int peer; core_ctx *c = start_inbox(&peer);
        int64_t since = c->pending_since;
        radio_command original = c->pending;
        now_ms = since + ages[i];
        service_radio_queue(c);
        queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
        uint8_t log_rx[146] = {0x88}, deleted[33] = {0x8f}, waiting[] = {0x83};
        radio_frame_cb(log_rx, sizeof(log_rx), c);
        radio_frame_cb(deleted, sizeof(deleted), c);
        ASSERT_TRUE(c->radio_pending);
        ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_READY);
        ASSERT_EQ_INT(c->pending_since, since);
        ASSERT_TRUE(memcmp(&original, &c->pending, sizeof(original)) == 0);
        ASSERT_EQ_INT(c->queue_count, 1);
        no_transmission(peer);
        uint8_t empty[] = {10}; radio_frame_cb(empty, sizeof(empty), c);
        ASSERT_TRUE(!inbox_needs_drain(c));
        ASSERT_EQ_INT(c->inbox_empty_generation, original.inbox_generation);
        ASSERT_EQ_INT(c->pending.data[0], 31);
        uint8_t wire[8]; ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
        ASSERT_EQ_INT(wire[3], 31); no_transmission(peer);
        ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_READY);
        /* A recognized waiting push also cannot complete/reset an inbox read. */
        c->pending = original; c->pending_since = since; c->radio_pending = true;
        radio_frame_cb(waiting, sizeof(waiting), c);
        ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending_since, since);
        ASSERT_TRUE(memcmp(&original, &c->pending, sizeof(original)) == 0);
        no_transmission(peer);
        close(c->serial.fd); close(peer); free(c);
        now_ms += 10000;
    }
    int peer; core_ctx *c = start_inbox(&peer);
    int64_t since = c->pending_since;
    now_ms = since + 1999; service_radio_queue(c); no_transmission(peer);
    now_ms = since + 2000; service_radio_queue(c);
    ASSERT_EQ_INT(c->serial.fd, -1);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    ASSERT_TRUE(!c->state.radio_connected && !c->radio_pending);
    ASSERT_EQ_INT(c->queue_count, 0); ASSERT_TRUE(c->reconnect_at > 0);
    ASSERT_TRUE(strstr(c->last_status, "reconnecting") != NULL);
    /* No retry or queued command was written before the socket closed. */
    uint8_t wire[8]; ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 0);
    close(peer); free(c);
    radio_command other = {.len = 1, .purpose = RADIO_PURPOSE_GENERIC};
    const int codes[] = {1, 2, 3, 4, 7, 8, 11, 12, 13, 17, 20, 22, 31, 32};
    for (size_t i = 0; i < QTC_ARRAY_LEN(codes); i++) {
        other.data[0] = codes[i];
        ASSERT_EQ_INT(radio_command_timeout_ms(&other), codes[i] == 4 ? 5000 : 1500);
        ASSERT_EQ_INT(radio_command_transport_retries(&other), 1);
    }
    for (int purpose = RADIO_PURPOSE_STARTUP_APP; purpose <= RADIO_PURPOSE_ADVERT_FLOOD; purpose++) {
        other.purpose = (radio_purpose)purpose;
        ASSERT_EQ_INT(radio_command_transport_retries(&other),
                      purpose == RADIO_PURPOSE_STARTUP_APP || purpose == RADIO_PURPOSE_STARTUP_DEVICE);
    }
    puts("sync-message timeout tests passed");
    return 0;
}
