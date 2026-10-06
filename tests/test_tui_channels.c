#include "../src/tui.c"
#include "test.h"
static bool favorite_hint(tui_ctx *t) {
    screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
    render_messages(t, &s);
    char line[121];
    for (int i = 0; i < t->width; i++) line[i] = screen_at(&s, t->height - 2, i)->bytes[0];
    line[t->width] = 0;
    bool visible = strstr(line, "[f] Favorite") != NULL;
    screen_free(&s); return visible;
}
static void favorite_tests(void) {
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t); t->fd = pair[0]; t->width = 60; t->height = 24;
    t->state.contact_count = 1;
    qtc_contact *ct = &t->state.contacts[0]; ct->node_type = QTC_NODE_PERSON;
    strcpy(ct->id, "person"); strcpy(ct->name, "Person");
    move_menu(t, 0); ASSERT_TRUE(favorite_hint(t));
    for (int favorite = 1; favorite >= 0; favorite--) {
        normal_key(t, 'f'); qtc_ipc_frame f;
        ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &f, 1000), 0);
        ASSERT_EQ_INT(f.type, QTC_IPC_SET_FAVORITE);
        qtc_ipc_favorite_payload p; memcpy(&p, f.payload, sizeof(p));
        ASSERT_EQ_INT(p.favorite, favorite); ASSERT_STREQ(p.contact_id, "person");
        ct->favorite = p.favorite; /* Apply the authoritative core delta. */
        f.type = QTC_IPC_CONTACT; f.length = sizeof(*ct); memcpy(f.payload, ct, sizeof(*ct));
        ipc_frame(&f, t); move_menu(t, 0); ASSERT_TRUE(favorite_hint(t));
    }
    t->mode = MODE_COMPOSE; ASSERT_TRUE(!favorite_hint(t));
    t->mode = MODE_SEARCH; ASSERT_TRUE(!favorite_hint(t));
    t->mode = MODE_MESSAGE_SELECT; ASSERT_TRUE(!favorite_hint(t));
    t->mode = MODE_NORMAL; t->state.contact_count = 0;
    t->state.channel_count = 1; t->state.channels[0].configured = true;
    strcpy(t->state.channels[0].name, "Public"); move_menu(t, 0);
    ASSERT_TRUE(!favorite_hint(t));
    t->state.channel_count = 0; move_menu(t, 0); ASSERT_TRUE(!favorite_hint(t));
    close(pair[0]); close(pair[1]); free(t);
}
int main(void) {
    favorite_tests();
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t); t->fd = pair[0]; t->width = 80; t->height = 24;
    t->state.channel_count = 1;
    qtc_channel *ch = &t->state.channels[0]; *ch = (qtc_channel){.index = 6, .configured = true, .secret = {1}};
    strcpy(ch->name, "same name");
    char ka[QTC_MAX_ID]; qtc_channel_key(ch->secret, ka);
    t->state.message_count = 1; qtc_message *m = &t->state.messages[0];
    m->conversation_kind = QTC_CONV_CHANNEL; strcpy(m->conversation_key, ka);
    strcpy(m->text, "A history"); strcpy(m->message_key, "A"); strcpy(m->logical_key, "A");
    m->part_index = m->part_total = 1;
    move_menu(t, 0); ASSERT_STREQ(displayed_key(t), ka);
    size_t indices[4]; ASSERT_EQ_INT(collect_logical_messages(t, indices, 4), 1);
    ch->index = 7; move_menu(t, 0); ASSERT_STREQ(displayed_key(t), ka);
    ASSERT_EQ_INT(collect_logical_messages(t, indices, 4), 1);
    normal_key(t, 13); ASSERT_EQ_INT(t->mode, MODE_COMPOSE); ASSERT_STREQ(t->open_key, ka);
    qtc_ipc_frame frame;
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0); ASSERT_EQ_INT(frame.type, QTC_IPC_MARK_READ);
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0); ASSERT_EQ_INT(frame.type, QTC_IPC_ACTIVE_CONVERSATION);
    append_input(t, 'x'); normal_key(t, 13);
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0); ASSERT_EQ_INT(frame.type, QTC_IPC_SEND_CHANNEL);
    qtc_ipc_send_channel_payload sent; memcpy(&sent, frame.payload, sizeof(sent)); ASSERT_STREQ(sent.conversation_key, ka);
    t->mode = MODE_NORMAL; ch->index = 6; ch->secret[15] = 2;
    move_menu(t, 0); ASSERT_TRUE(strcmp(displayed_key(t), ka));
    ASSERT_EQ_INT(collect_logical_messages(t, indices, 4), 0);
    ch->configured = false; move_menu(t, 0); ASSERT_EQ_INT(t->selected_kind, 0);
    ch->configured = true; ch->secret[15] = 0; move_menu(t, 0);
    ASSERT_EQ_INT(collect_logical_messages(t, indices, 4), 1);
    t->view = VIEW_CHANNELS; t->selected_channel = 6;
    screen s; ASSERT_EQ_INT(screen_init(&s, 80, 24), 0); render_channels(t, &s);
    char row[81]; for (int i = 0; i < 80; i++) row[i] = screen_at(&s, 3, i)->bytes[0]; row[80] = 0;
    ASSERT_TRUE(strstr(row, "[c] Create")); ASSERT_TRUE(strstr(row, "[r] Rotate")); ASSERT_TRUE(strstr(row, "[d] Leave"));
    screen_free(&s);
    ASSERT_EQ_INT(screen_init(&s, 60, 18), 0); t->width = 60; t->height = 18;
    render_channels(t, &s);
    for (int i = 0; i < 60; i++) row[i] = screen_at(&s, 3, i)->bytes[0];
    row[60] = 0; ASSERT_TRUE(strstr(row, "[d] Leave"));
    for (int i = 0; i < 60; i++) row[i] = screen_at(&s, 16, i)->bytes[0];
    row[60] = 0; ASSERT_TRUE(strstr(row, "[v] Review")); ASSERT_TRUE(strstr(row, "Esc/F6 Back"));
    screen_free(&s);
    normal_key(t, 'j'); ASSERT_EQ_INT(t->mode, MODE_JOIN_CHANNEL);
    const char *tag = "#hamradio"; process_input(t, (const uint8_t *)tag, strlen(tag)); normal_key(t, 13);
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0); ASSERT_EQ_INT(frame.type, QTC_IPC_CHANNEL_JOIN);
    qtc_ipc_channel_action_payload join; memcpy(&join, frame.payload, sizeof(join)); ASSERT_STREQ(join.uri, "#hamradio");
    close(pair[0]); close(pair[1]); free(t); puts("TUI channel tests passed"); return 0;
}
