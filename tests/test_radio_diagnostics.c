#include "../src/core.c"
#include "test.h"

int main(void) {
    FILE *log = tmpfile(); ASSERT_TRUE(log != NULL);
    int saved = dup(STDERR_FILENO); ASSERT_TRUE(saved >= 0);
    ASSERT_EQ_INT(dup2(fileno(log), STDERR_FILENO), STDERR_FILENO);
    core_ctx *c = calloc(1, sizeof(*c)); ASSERT_TRUE(c != NULL);
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    c->serial.fd = pair[0]; c->state.radio_connected = true;
    c->session_phase = RADIO_SESSION_READY; c->clipboard_client = -1;
    radio_command example = {.data = {10}, .len = 1};
    const char *disabled[] = {"", "0", "true"};
    qtc_set_log_level(QTC_LOG_DEBUG);
    unsetenv("QTC_TRACE_RADIO");
    trace_command(c, "disabled", &example, 0);
    for (size_t i = 0; i < QTC_ARRAY_LEN(disabled); i++) {
        setenv("QTC_TRACE_RADIO", disabled[i], 1);
        trace_command(c, "disabled", &example, 0);
    }
    fflush(stderr); ASSERT_EQ_INT(ftell(log), 0);
    qtc_set_log_level(QTC_LOG_INFO);
    setenv("QTC_TRACE_RADIO", "1", 1);
    request_inbox_drain(c, true);
    queue_next_inbox_message(c, RADIO_PRIORITY_INBOX);
    service_radio_queue(c);
    uint8_t wire[8]; ASSERT_EQ_INT(read(pair[1], wire, sizeof(wire)), 4);
    ASSERT_TRUE(c->radio_pending);
    int64_t since = c->pending_since;
    /* These asynchronous pushes remain UNKNOWN and cannot complete inbox reads. */
    uint8_t rx_log[146] = {0x88}, deleted[33] = {0x8f}, empty[] = {10};
    memcpy(rx_log + 1, "PRIVATE_MESSAGE_SENTINEL", 24);
    radio_frame_cb(rx_log, sizeof(rx_log), c);
    radio_frame_cb(deleted, sizeof(deleted), c);
    ASSERT_TRUE(c->radio_pending);
    ASSERT_EQ_INT(c->pending_since, since);
    ASSERT_EQ_INT(c->pending.data[0], 10);
    radio_frame_cb(empty, sizeof(empty), c);
    ASSERT_TRUE(!c->radio_pending);
    ASSERT_TRUE(!inbox_needs_drain(c));
    request_inbox_drain(c, true);
    queue_next_inbox_message(c, RADIO_PRIORITY_INBOX);
    service_radio_queue(c);
    ASSERT_EQ_INT(read(pair[1], wire, sizeof(wire)), 4);
    c->pending_since = qtc_now_millis() - 5001;
    service_radio_queue(c);
    ASSERT_EQ_INT(c->session_phase, RADIO_SESSION_DOWN);
    close(pair[1]); free(c);
    c = calloc(1, sizeof(*c)); ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    c->serial.fd = pair[0]; c->state.radio_connected = true;
    c->session_phase = RADIO_SESSION_WAIT_APP_START; c->clipboard_client = -1;
    c->radio_pending = true; c->pending.data[0] = 1; c->pending.len = 1;
    c->pending.purpose = RADIO_PURPOSE_STARTUP_APP;
    c->pending_since = qtc_now_millis() - 1501;
    service_radio_queue(c);
    ASSERT_TRUE(c->radio_pending); ASSERT_EQ_INT(c->pending.transport_attempts, 1);
    ASSERT_EQ_INT(read(pair[1], wire, sizeof(wire)), 4);
    close(c->serial.fd); close(pair[1]); free(c);
    fflush(stderr); ASSERT_EQ_INT(dup2(saved, STDERR_FILENO), STDERR_FILENO); close(saved);
    rewind(log);
    char output[32768]; size_t n = fread(output, 1, sizeof(output) - 1, log);
    output[n] = 0; ASSERT_TRUE(feof(log)); fclose(log);
    const char *required[] = {
        "action=enqueue", "action=dequeue", "action=tx-begin", "action=tx-complete",
        "action=completed", "action=soft-timeout", "action=hard-timeout", "opcode=0x0a name=CMD_SYNC_MESSAGE",
        "timeout_ms=5000", "attempt=1", "wall_ms=", "mono_ms=", "deadline_ms=",
        "age_ms=", "expected=QTC_RADIO_CONTACT_MESSAGE|QTC_RADIO_CHANNEL_MESSAGE|QTC_RADIO_NO_MORE_MESSAGES",
        "name=PUSH_CODE_LOG_RX_DATA", "name=PUSH_CODE_CONTACT_DELETED",
        "event_name=QTC_RADIO_UNKNOWN", "name=RESP_CODE_NO_MORE_MESSAGES"
    };
    for (size_t i = 0; i < QTC_ARRAY_LEN(required); i++)
        ASSERT_TRUE(strstr(output, required[i]) != NULL);
    const char *tx = strstr(output, "action=tx-complete"); ASSERT_TRUE(tx != NULL);
    long long start = strtoll(strstr(tx, "since_ms=") + strlen("since_ms="), NULL, 10);
    long long deadline = strtoll(strstr(tx, "deadline_ms=") + strlen("deadline_ms="), NULL, 10);
    ASSERT_TRUE(start > 0); ASSERT_EQ_INT(deadline - start, 5000);
    const char *timeout = strstr(output, "action=hard-timeout"); ASSERT_TRUE(timeout != NULL);
    ASSERT_TRUE(strtoll(strstr(timeout, "age_ms=") + strlen("age_ms="), NULL, 10) >= 5000);
    ASSERT_TRUE(strstr(output, "attempt=2") != NULL);
    ASSERT_TRUE(strstr(output, "PRIVATE_MESSAGE_SENTINEL") == NULL);
    ASSERT_TRUE(strstr(output, "disabled") == NULL);
    unsetenv("QTC_TRACE_RADIO");
    puts("radio diagnostic tracing tests passed");
    return 0;
}
