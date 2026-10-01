#include "../src/core.c"
#include "test.h"

static core_ctx *ready_core(int *peer) {
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    core_ctx *c = calloc(1, sizeof(*c)); ASSERT_TRUE(c != NULL);
    c->serial.fd = pair[0]; *peer = pair[1];
    c->state.radio_connected = true; c->session_phase = RADIO_SESSION_READY;
    c->clipboard_client = -1;
    return c;
}

static void expire(core_ctx *c, uint8_t code, radio_purpose purpose, int retries) {
    memset(&c->pending, 0, sizeof(c->pending));
    c->pending.data[0] = code; c->pending.len = 1;
    c->pending.purpose = purpose; c->pending.transport_attempts = retries;
    c->pending.expected_a = QTC_RADIO_OK;
    c->radio_pending = true;
    c->pending_since = qtc_now_millis() - radio_command_timeout_ms(&c->pending) - 1;
    service_radio_queue(c);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int peer;
    core_ctx *c = ready_core(&peer);
    uint8_t wire[8], cmd[] = {31, 0};
    request_inbox_drain(c, true);
    queue_next_inbox_message(c, RADIO_PRIORITY_INBOX);
    service_radio_queue(c);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4);
    ASSERT_EQ_INT(radio_command_timeout_ms(&c->pending), 1500);
    ASSERT_EQ_INT(radio_command_transport_retries(&c->pending), 0);
    ASSERT_EQ_INT(queue_radio_ex(c, cmd, sizeof(cmd), QTC_RADIO_OK,
                                QTC_RADIO_ERROR, QTC_RADIO_UNKNOWN, "",
                                RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_NORMAL), 0);
    /* Past the old deadline, retain ownership and do not transmit the queue. */
    c->pending_since = qtc_now_millis() - 650;
    service_radio_queue(c);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.data[0], 10);
    ASSERT_EQ_INT(c->queue_count, 1);
    struct pollfd check = {.fd = peer, .events = POLLIN};
    ASSERT_EQ_INT(poll(&check, 1, 0), 0);
    /* An empty reply for an older generation must not erase a newer push. */
    request_inbox_drain(c, true);
    qtc_radio_event empty = {.type = QTC_RADIO_NO_MORE_MESSAGES};
    radio_event(c, &empty);
    ASSERT_EQ_INT(c->inbox_empty_generation, 1);
    ASSERT_TRUE(inbox_needs_drain(c));
    ASSERT_EQ_INT(c->pending.data[0], 10);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4);
    radio_event(c, &empty);
    ASSERT_TRUE(!inbox_needs_drain(c));
    ASSERT_EQ_INT(c->pending.data[0], 31);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
    /* An unsolicited late empty cannot complete an unrelated command. */
    radio_event(c, &empty);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.data[0], 31);
    qtc_radio_event ok = {.type = QTC_RADIO_OK};
    radio_event(c, &ok);
    ASSERT_TRUE(!c->radio_pending);
    expire(c, 10, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_EQ_INT(c->serial.fd, -1);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    ASSERT_TRUE(!c->state.radio_connected && !c->radio_pending);
    ASSERT_EQ_INT(c->queue_count, 0); ASSERT_EQ_INT(c->pending.len, 0);
    ASSERT_TRUE(!inbox_needs_drain(c));
    ASSERT_TRUE(strstr(c->last_status, "reconnecting") != NULL);
    close(peer); free(c);

    c = ready_core(&peer);
    expire(c, 31, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.transport_attempts, 1);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4);
    expire(c, 31, RADIO_PURPOSE_GENERIC, 1);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    close(peer); free(c);

    const radio_purpose startup[] = {RADIO_PURPOSE_STARTUP_APP, RADIO_PURPOSE_STARTUP_DEVICE};
    for (size_t i = 0; i < QTC_ARRAY_LEN(startup); i++) {
        c = ready_core(&peer);
        c->session_phase = i ? RADIO_SESSION_WAIT_DEVICE_INFO : RADIO_SESSION_WAIT_APP_START;
        expire(c, i ? 22 : 1, startup[i], 0);
        ASSERT_TRUE(c->serial.fd >= 0);
        ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4);
        expire(c, i ? 22 : 1, startup[i], 1);
        ASSERT_EQ_INT(c->serial.fd, -1);
        ASSERT_TRUE(strstr(c->last_status, "startup handshake timed out") != NULL);
        close(peer); free(c);
    }
    c = ready_core(&peer); close(peer);
    expire(c, 31, RADIO_PURPOSE_GENERIC, 0); /* retry write fails */
    ASSERT_EQ_INT(c->serial.fd, -1);
    ASSERT_TRUE(strstr(c->last_status, "write failed") != NULL);
    free(c);
    puts("core command timeout policy tests passed");
    return 0;
}
