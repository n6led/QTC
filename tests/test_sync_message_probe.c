/* Control the core's existing wall-clock timer without sleeping at boundaries. */
#define qtc_now_millis probe_now_millis
#include "../src/core.c"
#undef qtc_now_millis
#include "test.h"

static int64_t now_ms = 100000;
int64_t probe_now_millis(void) { return now_ms; }

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
    ASSERT_EQ_INT(radio_command_transport_retries(&c->pending), 0);
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
    FILE *log = tmpfile(); ASSERT_TRUE(log != NULL);
    int saved = dup(STDERR_FILENO); ASSERT_TRUE(saved >= 0);
    ASSERT_EQ_INT(dup2(fileno(log), STDERR_FILENO), STDERR_FILENO);
    setenv("QTC_TRACE_RADIO", "1", 1);
    const int ages[] = {100, 249, 251, 1000, 4999};
    for (size_t i = 0; i < QTC_ARRAY_LEN(ages); i++) {
        int peer; core_ctx *c = start_inbox(&peer);
        int64_t since = c->pending_since;
        radio_command original = c->pending;
        if (ages[i] > 250) {
            now_ms = since + 250;
            service_radio_queue(c);
            ASSERT_TRUE(c->inbox_soft_timeout_logged);
            no_transmission(peer);
        }
        now_ms = since + ages[i];
        service_radio_queue(c);
        queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
        uint8_t log_rx[146] = {0x88}, deleted[33] = {0x8f};
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
        ASSERT_EQ_INT(c->pending.data[0], 31); /* only now may the queue advance */
        uint8_t wire[8]; ASSERT_EQ_INT(read(peer, wire, sizeof(wire)), 5);
        ASSERT_EQ_INT(wire[3], 31); no_transmission(peer);
        ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_READY);
        close(c->serial.fd); close(peer); free(c);
        now_ms += 10000;
    }
    int peer; core_ctx *c = start_inbox(&peer);
    int64_t since = c->pending_since;
    now_ms = since + 250; service_radio_queue(c);
    now_ms = since + 4999; service_radio_queue(c);
    no_transmission(peer);
    now_ms = since + 5000; service_radio_queue(c);
    ASSERT_EQ_INT(c->serial.fd, -1);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    ASSERT_TRUE(!c->state.radio_connected && !c->radio_pending);
    ASSERT_EQ_INT(c->queue_count, 0);
    ASSERT_TRUE(c->reconnect_at > 0);
    ASSERT_TRUE(strstr(c->last_status, "reconnecting") != NULL);
    close(peer); free(c);
    /* Existing non-inbox timeout/retry policy stays intact. */
    radio_command other = {.data = {31}, .len = 1, .purpose = RADIO_PURPOSE_GENERIC};
    ASSERT_EQ_INT(radio_command_timeout_ms(&other), 1500);
    ASSERT_EQ_INT(radio_command_transport_retries(&other), 1);
    other.data[0] = 4; ASSERT_EQ_INT(radio_command_timeout_ms(&other), 5000);
    fflush(stderr); ASSERT_EQ_INT(dup2(saved, STDERR_FILENO), STDERR_FILENO); close(saved);
    rewind(log); char output[65536]; size_t n = fread(output, 1, sizeof(output) - 1, log);
    output[n] = 0; ASSERT_TRUE(feof(log)); fclose(log);
    const char *late = strstr(output, "action=late-response"); ASSERT_TRUE(late != NULL);
    ASSERT_TRUE(strstr(late, "age_ms=251") != NULL);
    ASSERT_TRUE(strstr(output, "age_ms=4999") != NULL);
    ASSERT_TRUE(strstr(output, "action=hard-timeout") != NULL);
    ASSERT_TRUE(strstr(output, "age_ms=5000") != NULL);
    int soft_count = 0;
    for (const char *p = output; (p = strstr(p, "action=soft-timeout")) != NULL; p++) soft_count++;
    ASSERT_EQ_INT(soft_count, 4); /* once per overdue request */
    unsetenv("QTC_TRACE_RADIO");
    puts("sync-message timeout probe tests passed");
    return 0;
}
