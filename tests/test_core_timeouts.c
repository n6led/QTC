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
    int fd = c->serial.fd;
    expire(c, 10, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_EQ_INT(c->serial.fd, fd); ASSERT_TRUE(c->state.radio_connected);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_READY);
    ASSERT_TRUE(!c->radio_pending); ASSERT_EQ_INT(c->pending.len, 0);
    ASSERT_EQ_INT(c->pending_since, 0); ASSERT_EQ_INT(c->reconnect_at, 0);
    ASSERT_TRUE(strstr(c->last_status, "session remains ready") != NULL);
    ASSERT_EQ_INT(c->command_timeout_failures, 1);

    /* An unrelated recognized response is health evidence, not just matched OK. */
    uint8_t unknown[] = {255}, malformed[] = {18}, valid[] = {0};
    radio_frame_cb(unknown, sizeof(unknown), c);
    radio_frame_cb(malformed, sizeof(malformed), c);
    ASSERT_EQ_INT(c->command_timeout_failures, 1);
    radio_frame_cb(valid, sizeof(valid), c);
    ASSERT_EQ_INT(c->command_timeout_failures, 0);
    uint8_t cmd[] = {31, 0}, wire[8];
    ASSERT_EQ_INT(queue_radio_ex(c, cmd, sizeof(cmd), QTC_RADIO_CHANNEL_INFO,
                                QTC_RADIO_ERROR, QTC_RADIO_UNKNOWN, "",
                                RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_NORMAL), 0);
    service_radio_queue(c);
    ASSERT_TRUE(c->radio_pending);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
    qtc_radio_event channel = {.type = QTC_RADIO_CHANNEL_INFO};
    /* Use OK for a completion without requiring the database fixture. */
    c->pending.expected_a = QTC_RADIO_OK; channel.type = QTC_RADIO_OK;
    radio_event(c, &channel);
    ASSERT_TRUE(!c->radio_pending); ASSERT_EQ_INT(c->serial.fd, fd);

    /* A generic transport retry is not an exhausted command. */
    expire(c, 31, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.transport_attempts, 1);
    ASSERT_EQ_INT(c->command_timeout_failures, 0);
    ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 4);
    expire(c, 31, RADIO_PURPOSE_GENERIC, 1);
    ASSERT_EQ_INT(c->command_timeout_failures, 1);
    c->clipboard_client = 0;
    expire(c, 17, RADIO_PURPOSE_EXPORT_SELF, 0);
    ASSERT_EQ_INT(c->clipboard_client, -1); ASSERT_EQ_INT(c->serial.fd, fd);
    ASSERT_EQ_INT(c->command_timeout_failures, 2);
    radio_frame_cb(valid, sizeof(valid), c);
    ASSERT_EQ_INT(c->command_timeout_failures, 0);
    expire(c, 7, RADIO_PURPOSE_ADVERT_ZERO_HOP, 0);
    ASSERT_TRUE(strstr(c->last_status, "timed out") != NULL);
    ASSERT_EQ_INT(c->serial.fd, fd);
    expire(c, 10, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_EQ_INT(c->serial.fd, fd);
    expire(c, 10, RADIO_PURPOSE_GENERIC, 0);
    ASSERT_EQ_INT(c->serial.fd, -1); ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    ASSERT_EQ_INT(c->command_timeout_failures, 0);
    ASSERT_TRUE(strstr(c->last_status, "Repeated") != NULL);
    close(peer); free(c);

    const radio_purpose startup[] = {RADIO_PURPOSE_STARTUP_APP, RADIO_PURPOSE_STARTUP_DEVICE};
    for (size_t i = 0; i < QTC_ARRAY_LEN(startup); i++) {
        c = ready_core(&peer);
        c->session_phase = i ? RADIO_SESSION_WAIT_DEVICE_INFO : RADIO_SESSION_WAIT_APP_START;
        expire(c, i ? 22 : 1, startup[i], 0);
        ASSERT_TRUE(c->serial.fd >= 0); ASSERT_EQ_INT(c->command_timeout_failures, 0);
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
