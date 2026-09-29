/* Exercise the actual private TUI handlers/framebuffer without a real terminal. */
#include "../src/tui.c"
#include "test.h"

static void type_text(tui_ctx *t, const char *text) {
    while (*text) normal_key(t, (unsigned char)*text++);
}

static void add_message(tui_ctx *t, const char *key, const char *logical,
                        const char *text, int part, int total) {
    qtc_message m = {0};
    m.conversation_kind = t->open_kind;
    qtc_strlcpy(m.conversation_key, t->open_key, sizeof(m.conversation_key));
    qtc_strlcpy(m.message_key, key, sizeof(m.message_key));
    qtc_strlcpy(m.logical_key, logical, sizeof(m.logical_key));
    qtc_strlcpy(m.text, text, sizeof(m.text));
    m.part_index = part; m.part_total = total;
    tui_upsert_message(t, &m);
}

static void draw(tui_ctx *t) {
    screen s;
    ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
    render_messages(t, &s);
    if (t->mode == MODE_MESSAGE_SELECT && t->selected_message[0]) {
        int split = t->width / 3;
        if (split < 30) split = 30;
        if (split > 44) split = 44;
        bool marked = false;
        for (int row = 5; row < t->height - 2; row++) {
            screen_cell *cell = screen_at(&s, row, split + 2);
            if (cell->bytes[0] == '>' && cell->style == UI_SELECTED) marked = true;
        }
        ASSERT_TRUE(marked);
    }
    screen_free(&s);
}

int main(void) {
    ASSERT_TRUE(setlocale(LC_CTYPE, "") != NULL);
    ASSERT_EQ_INT(wcwidth(L'\U0001f4e1'), 2);
    char name[QTC_MAX_NAME], prefix[QTC_MAX_NAME + 4];
    const char *names[] = {"KO6IFX-N2", "N6LED - OBSVR🥃", "🌱MeshGarden🍎 BOT", "Cafe\xcc\x81"};
    for (size_t i = 0; i < QTC_ARRAY_LEN(names); i++) {
        ASSERT_EQ_INT(qtc_mention_prefix(names[i], prefix, sizeof(prefix)), 0);
        ASSERT_EQ_INT(qtc_mention_length(prefix), strlen(names[i]) + 3);
        char channel[160]; snprintf(channel, sizeof(channel), "%s: reading: 42", names[i]);
        ASSERT_TRUE(qtc_channel_sender(channel, name, sizeof(name)));
        ASSERT_STREQ(name, names[i]);
    }
    ASSERT_TRUE(!qtc_channel_sender("https://example.com", name, sizeof(name)));
    ASSERT_TRUE(!qtc_channel_sender("missing sender", name, sizeof(name)));
    ASSERT_TRUE(!qtc_channel_sender(": text", name, sizeof(name)));
    ASSERT_TRUE(!qtc_channel_sender("bad:name: text", name, sizeof(name)));
    ASSERT_TRUE(!qtc_mention_name_valid("bad]name"));
    ASSERT_TRUE(!qtc_mention_name_valid("bad\nname"));
    ASSERT_TRUE(!qtc_mention_name_valid("bad\xf0\x9f"));
    ASSERT_EQ_INT(qtc_mention_length("@ordinary"), 0);
    ASSERT_EQ_INT(qtc_mention_length("@[unfinished"), 0);
    ASSERT_EQ_INT(qtc_mention_length("@[]"), 0);
    ASSERT_EQ_INT(qtc_mention_length("@[a[b]"), 0);
    strcpy(prefix, "unchanged");
    ASSERT_EQ_INT(qtc_mention_prefix(names[0], prefix, 4), -1);
    ASSERT_STREQ(prefix, "unchanged");

    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    int sockets[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    t->fd = sockets[0]; t->width = 100; t->height = 28;
    t->open_kind = QTC_CONV_CONTACT; strcpy(t->open_key, "contact");
    t->state.contact_count = 1;
    strcpy(t->state.contacts[0].id, "contact");
    strcpy(t->state.contacts[0].name, names[0]);
    t->state.contacts[0].node_type = QTC_NODE_PERSON;
    add_message(t, "p1", "logical", "first ", 1, 2);
    add_message(t, "p2", "logical", "second", 2, 2);
    size_t indices[8];
    ASSERT_EQ_INT(collect_logical_messages(t, indices, 8), 1);
    normal_key(t, '\t'); draw(t);
    ASSERT_STREQ(t->selected_message, "logical");
    add_message(t, "new", "new", "new message", 1, 1);
    draw(t); ASSERT_STREQ(t->selected_message, "logical");
    special_key(t, "\x1b[B"); ASSERT_STREQ(t->selected_message, "new");
    special_key(t, "\x1b[A"); ASSERT_STREQ(t->selected_message, "logical");
    strcpy(t->draft, "existing draft");
    normal_key(t, 'r'); ASSERT_STREQ(t->input, "@[KO6IFX-N2] existing draft");
    escape_mode(t); ASSERT_STREQ(t->draft, "existing draft");
    normal_key(t, '\t'); normal_key(t, 'r');
    /* The existing direct-send payload contains only the ordinary mention text. */
    normal_key(t, '\r');
    qtc_ipc_frame frame;
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(sockets[1], &frame, 1000), 0);
    ASSERT_EQ_INT(frame.type, QTC_IPC_SEND_DIRECT);
    qtc_ipc_send_direct_payload direct; memcpy(&direct, frame.payload, sizeof(direct));
    ASSERT_STREQ(direct.text, "@[KO6IFX-N2] existing draft");
    ASSERT_TRUE(!t->draft[0]);

    t->open_kind = QTC_CONV_CHANNEL; strcpy(t->open_key, "0");
    t->state.message_count = 0; t->selected_message[0] = 0;
    add_message(t, "channel", "channel", "🌱MeshGarden🍎 BOT: latest reading", 1, 1);
    normal_key(t, '\t'); draw(t); normal_key(t, 'r');
    ASSERT_STREQ(t->input, "@[🌱MeshGarden🍎 BOT] ");
    normal_key(t, '\r');
    ASSERT_EQ_INT(qtc_ipc_recv_blocking(sockets[1], &frame, 1000), 0);
    ASSERT_EQ_INT(frame.type, QTC_IPC_SEND_CHANNEL);
    qtc_ipc_send_channel_payload channel; memcpy(&channel, frame.payload, sizeof(channel));
    ASSERT_STREQ(channel.text, "@[🌱MeshGarden🍎 BOT]");
    ASSERT_STREQ(t->state.messages[0].text, "🌱MeshGarden🍎 BOT: latest reading");
    /* Missing/ambiguous sender and a draft too large for a prefix are safe. */
    t->mode = MODE_MESSAGE_SELECT;
    strcpy(t->state.messages[0].text, "no sender prefix");
    reply_message(t); ASSERT_EQ_INT(t->mode, MODE_MESSAGE_SELECT);
    strcpy(t->state.messages[0].text, "🌱MeshGarden🍎 BOT: latest reading");
    memset(t->draft, 'x', sizeof(t->draft) - 1); t->draft[sizeof(t->draft) - 1] = 0;
    reply_message(t); ASSERT_EQ_INT(t->mode, MODE_MESSAGE_SELECT);
    ASSERT_EQ_INT(strlen(t->draft), sizeof(t->draft) - 1);
    t->draft[0] = 0;

    start_input(t, MODE_COMPOSE); type_text(t, "hello @");
    ASSERT_TRUE(t->mention_active);
    char candidates[MENTION_CANDIDATES][QTC_MAX_NAME];
    ASSERT_TRUE(mention_candidates(t, candidates) >= 2);
    ASSERT_STREQ(candidates[0], names[2]);
    special_key(t, "\x1b[B"); normal_key(t, '\r');
    ASSERT_STREQ(t->input, "hello @[KO6IFX-N2] ");
    ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
    type_text(t, "@"); normal_key(t, 27);
    ASSERT_TRUE(!t->mention_active); ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
    ASSERT_STREQ(t->input, "hello @[KO6IFX-N2] @");
    type_text(t, "ordinary");
    ASSERT_STREQ(t->input, "hello @[KO6IFX-N2] @ordinary");
    escape_mode(t); start_input(t, MODE_COMPOSE);
    ASSERT_STREQ(t->input, "hello @[KO6IFX-N2] @ordinary");
    t->input[0] = 0; t->input_len = 0; t->draft[0] = 0;
    type_text(t, "@🌱"); normal_key(t, '\r');
    ASSERT_STREQ(t->input, "@[🌱MeshGarden🍎 BOT] ");
    escape_mode(t);
    t->selected_kind = 2; strcpy(t->selected_key, "contact");
    ASSERT_TRUE(!open_selected(t)); ASSERT_STREQ(t->open_key, "0");
    t->draft[0] = 0;

    /* Selection survives scrolling, resize, and arrival; a multipart stays one item. */
    for (int i = 0; i < 25; i++) {
        char key[20]; snprintf(key, sizeof(key), "line-%d", i);
        add_message(t, key, key, "KO6IFX-N2: message", 1, 1);
    }
    t->selected_message[0] = 0; t->history_scroll = 12;
    t->mode = MODE_MESSAGE_SELECT; draw(t);
    char selected[160]; strcpy(selected, t->selected_message);
    ASSERT_TRUE(strcmp(selected, "line-24") != 0);
    t->width = 80; t->height = 20; draw(t);
    ASSERT_STREQ(t->selected_message, selected);
    add_message(t, "arrival", "arrival", "new sender: hello", 1, 1); draw(t);
    ASSERT_STREQ(t->selected_message, selected);
    move_message(t, 100); t->state.messages[t->state.message_count - 1].direction = QTC_MSG_OUTGOING;
    reply_message(t); ASSERT_EQ_INT(t->mode, MODE_MESSAGE_SELECT);
    t->state.message_count = 0; move_message(t, 0); draw(t); reply_message(t);
    ASSERT_TRUE(!t->selected_message[0]);

    screen s; ASSERT_EQ_INT(screen_init(&s, 100, 10), 0);
    const char *text = "say @[N6LED - OBSVR🥃] then @ordinary";
    screen_put_mentioned(&s, 0, 0, 90, text, text, 0, UI_INCOMING);
    ASSERT_EQ_INT(screen_at(&s, 0, 0)->style, UI_INCOMING);
    ASSERT_EQ_INT(screen_at(&s, 0, 4)->style, UI_ACCENT);
    ASSERT_EQ_INT(screen_at(&s, 0, 30)->style, UI_INCOMING);
    screen_put_mentioned(&s, 1, 0, 90, "OBSVR🥃] then @ordinary", text, 14, UI_INCOMING);
    ASSERT_EQ_INT(screen_at(&s, 1, 0)->style, UI_ACCENT);
    screen_put_mentioned(&s, 2, 0, 90, "mail@host @[broken", "mail@host @[broken", 0, UI_INCOMING);
    ASSERT_EQ_INT(screen_at(&s, 2, 9)->style, UI_INCOMING);
    screen_free(&s);
    close(sockets[0]); close(sockets[1]); free(t);
    puts("reply and mention TUI tests passed");
    return 0;
}
