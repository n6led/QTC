#include "../src/tui.c"
#include "test.h"

static void type_text(tui_ctx *t, const char *text) {
    process_input(t, (const uint8_t *)text, strlen(text));
}

static void seed(tui_ctx *t, const char *text) {
    t->mode = MODE_NORMAL;
    qtc_strlcpy(t->draft, text, sizeof(t->draft));
    start_input(t, MODE_COMPOSE);
    ASSERT_EQ_INT(t->input_cursor, strlen(text));
}

static void silent(int peer) {
    char c;
    ASSERT_EQ_INT(recv(peer, &c, 1, MSG_DONTWAIT), -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void draw_composer(tui_ctx *t) {
    screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
    render_messages(t, &s);
    ASSERT_TRUE(s.cursor); ASSERT_EQ_INT(s.cursor_r, t->height);
    ASSERT_TRUE(s.cursor_c >= 11 && s.cursor_c < t->width);
    ASSERT_EQ_INT(s.cursor_c, 11 + composer_cells(t->input, t->input_scroll, t->input_cursor));
    ASSERT_TRUE(t->input_scroll <= t->input_cursor);
    ASSERT_TRUE(((unsigned char)t->input[t->input_scroll] & 0xc0U) != 0x80U);
    const char *prefix = "Message: ";
    for (int i = 0; prefix[i]; i++) ASSERT_EQ_INT(screen_at(&s, t->height - 1, 1 + i)->bytes[0], prefix[i]);
    for (int col = 10; col < t->width - 2; col++) {
        screen_cell *cell = screen_at(&s, t->height - 1, col);
        if (cell->width == 2) {
            ASSERT_TRUE(col + 1 < t->width - 2);
            ASSERT_TRUE(screen_at(&s, t->height - 1, col + 1)->continuation);
        }
    }
    screen_free(&s);
}

static void editor_tests(void) {
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    t->fd = -1; t->width = 68; t->height = 24;
    seed(t, "abcd"); type_text(t, "\x1b[D\x1b[DXY");
    ASSERT_STREQ(t->input, "abXYcd"); ASSERT_EQ_INT(t->input_cursor, 4);
    type_text(t, "\x1b[H\x1b[D"); ASSERT_EQ_INT(t->input_cursor, 0);
    normal_key(t, 127); ASSERT_STREQ(t->input, "abXYcd");
    type_text(t, "\x1b[3~"); ASSERT_STREQ(t->input, "bXYcd"); ASSERT_EQ_INT(t->input_cursor, 0);
    type_text(t, "\x1b[C\x1b[C\x1b[3~"); ASSERT_STREQ(t->input, "bXcd");
    normal_key(t, 8); ASSERT_STREQ(t->input, "bcd"); ASSERT_EQ_INT(t->input_cursor, 1);
    type_text(t, "\x1b[F\x1b[C\x1b[3~"); ASSERT_EQ_INT(t->input_cursor, 3);
    normal_key(t, 127); ASSERT_STREQ(t->input, "bc");
    type_text(t, "\x1bOH"); ASSERT_EQ_INT(t->input_cursor, 0);
    type_text(t, "\x1bOF"); ASSERT_EQ_INT(t->input_cursor, 2);
    /* An escape sequence may span separate terminal reads. */
    type_text(t, "\x1b["); ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
    type_text(t, "D"); ASSERT_EQ_INT(t->input_cursor, 1);
    type_text(t, "\x1b[3"); type_text(t, "~"); ASSERT_STREQ(t->input, "b");
    seed(t, "Aé📡中Z");
    type_text(t, "\x1b[D\x1b[D"); ASSERT_EQ_INT(t->input_cursor, strlen("Aé📡"));
    normal_key(t, 127); ASSERT_STREQ(t->input, "Aé中Z");
    type_text(t, "\x1b[3~"); ASSERT_STREQ(t->input, "AéZ");
    type_text(t, "\x1b[D"); ASSERT_EQ_INT(t->input_cursor, 1);
    type_text(t, "ñ"); ASSERT_STREQ(t->input, "AñéZ");
    seed(t, "A📡B"); type_text(t, "\x1b[D");
    ASSERT_EQ_INT(composer_viewport(t, 56), 3); draw_composer(t);
    seed(t, ""); type_text(t, "\xf0\x9f"); ASSERT_EQ_INT(t->input_len, 0);
    type_text(t, "\x93\xa1"); ASSERT_STREQ(t->input, "📡");
    for (int i = 0; i < 45; i++) type_text(t, "ab📡");
    draw_composer(t); ASSERT_TRUE(t->input_scroll > 0);
    size_t right = t->input_scroll;
    for (int i = 0; i < 85; i++) type_text(t, "\x1b[D");
    draw_composer(t); ASSERT_TRUE(t->input_scroll < right);
    type_text(t, "\x1b[H"); draw_composer(t); ASSERT_EQ_INT(t->input_scroll, 0);
    type_text(t, "\x1b[F"); draw_composer(t); ASSERT_TRUE(t->input_scroll > 0);
    t->width = 100; draw_composer(t); size_t wider = t->input_scroll;
    t->width = 68; draw_composer(t); ASSERT_TRUE(t->input_scroll > wider);
    t->width = 250; draw_composer(t); ASSERT_EQ_INT(t->input_scroll, 0);
    seed(t, "");
    for (size_t i = 0; i < sizeof(t->input) - 2; i++) normal_key(t, 'a');
    type_text(t, "📡"); ASSERT_EQ_INT(t->input_len, sizeof(t->input) - 2);
    type_text(t, "b"); ASSERT_EQ_INT(t->input_len, sizeof(t->input) - 1);
    type_text(t, "z"); ASSERT_EQ_INT(t->input_len, sizeof(t->input) - 1);
    /* Query completion/cancellation must preserve the suffix at the cursor. */
    t->state.contact_count = 1; t->state.contacts[0].node_type = QTC_NODE_PERSON;
    strcpy(t->state.contacts[0].name, "Éva📡");
    seed(t, "hello tail"); type_text(t, "\x1b[H");
    for (int i = 0; i < 6; i++) type_text(t, "\x1b[C");
    type_text(t, "@É"); ASSERT_TRUE(t->mention_active);
    normal_key(t, '\t'); ASSERT_TRUE(t->mention_active);
    normal_key(t, '\r'); ASSERT_STREQ(t->input, "hello @[Éva📡] tail");
    ASSERT_EQ_INT(t->input_cursor, strlen("hello @[Éva📡] "));
    type_text(t, "@"); normal_key(t, 27); ASSERT_STREQ(t->input, "hello @[Éva📡] tail");
    type_text(t, "@query"); normal_key(t, 27); ASSERT_STREQ(t->input, "hello @[Éva📡] @querytail");
    free(t);
}

static void navigation_tests(void) {
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    t->fd = pair[0]; t->width = 100; t->height = 26;
    t->state.channel_count = 1;
    t->state.channels[0].index = 0; t->state.channels[0].configured = true;
    t->state.channels[0].unread = 4; strcpy(t->state.channels[0].name, "Public 📡");
    t->state.contact_count = 3;
    for (int i = 0; i < 3; i++) {
        qtc_contact *ct = &t->state.contacts[i];
        snprintf(ct->id, sizeof(ct->id), "contact%d", i);
        snprintf(ct->name, sizeof(ct->name), "Person%d 📡", i);
        ct->node_type = QTC_NODE_PERSON; ct->unread = 3;
        ct->favorite = i == 0; ct->route_known = i < 2;
    }
    menu_item items[8]; qtc_roster roster;
    size_t count = build_menu(t, items, 8, &roster); ASSERT_EQ_INT(count, 4);
    ASSERT_EQ_INT(items[0].kind, 1); ASSERT_EQ_INT(items[1].kind, 2);
    ASSERT_STREQ(items[2].key, "contact1"); ASSERT_STREQ(items[3].key, "contact2");
    for (size_t i = 0; i < count; i++) {
        qtc_message *m = &t->state.messages[t->state.message_count++];
        m->conversation_kind = i ? QTC_CONV_CONTACT : QTC_CONV_CHANNEL;
        strcpy(m->conversation_key, items[i].key);
        snprintf(m->message_key, sizeof(m->message_key), "msg%zu", i);
        strcpy(m->logical_key, m->message_key); m->part_index = m->part_total = 1;
        strcpy(m->text, "History 📡");
    }
    t->selected_kind = items[0].kind; strcpy(t->selected_key, items[0].key);
    for (size_t i = 0; i < count; i++) {
        if (i) special_key(t, "\x1b[B");
        ASSERT_EQ_INT(t->mode, MODE_NORMAL);
        ASSERT_STREQ(displayed_key(t), items[i].key);
        size_t indices[8]; ASSERT_EQ_INT(collect_logical_messages(t, indices, 8), 1);
        ASSERT_STREQ(t->state.messages[indices[0]].conversation_key, items[i].key);
        screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
        render_messages(t, &s); screen_free(&s);
        silent(pair[1]); /* No read/active/send IPC, hence no Companion command. */
        normal_key(t, '\r'); ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
        ASSERT_STREQ(t->open_key, items[i].key);
        draw_composer(t);
        qtc_ipc_frame frame;
        ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0);
        ASSERT_EQ_INT(frame.type, QTC_IPC_MARK_READ);
        ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &frame, 1000), 0);
        ASSERT_EQ_INT(frame.type, QTC_IPC_ACTIVE_CONVERSATION);
        silent(pair[1]);
        char selection[QTC_MAX_ID]; strcpy(selection, t->selected_key);
        type_text(t, "\x1b[D\x1b[C\x1b[A\x1b[B"); ASSERT_STREQ(t->selected_key, selection);
        normal_key(t, 27); ASSERT_EQ_INT(t->mode, MODE_NORMAL);
    }
    special_key(t, "\x1b[A"); ASSERT_STREQ(displayed_key(t), "contact1"); silent(pair[1]);
    ASSERT_EQ_INT(t->state.channels[0].unread, 4);
    for (int i = 0; i < 3; i++) ASSERT_EQ_INT(t->state.contacts[i].unread, 3);
    /* Preview can leave the draft's conversation; activation keeps the old guard. */
    strcpy(t->draft, "saved 📡");
    normal_key(t, '\r'); ASSERT_EQ_INT(t->mode, MODE_NORMAL);
    ASSERT_STREQ(t->open_key, "contact2"); ASSERT_STREQ(t->draft, "saved 📡"); silent(pair[1]);
    normal_key(t, 'm'); ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
    ASSERT_STREQ(t->input, "saved 📡"); ASSERT_EQ_INT(t->input_cursor, strlen(t->input));
    escape_mode(t); t->draft[0] = 0;
    normal_key(t, '\t'); ASSERT_EQ_INT(t->mode, MODE_MESSAGE_SELECT);
    ASSERT_STREQ(t->open_key, "contact1");
    move_message(t, 0); reply_message(t); ASSERT_EQ_INT(t->mode, MODE_COMPOSE);
    ASSERT_EQ_INT(t->input_cursor, strlen(t->input));
    ASSERT_TRUE(strstr(t->input, "@[Person1 📡] > History 📡 | ") != NULL);
    type_text(t, "reply"); ASSERT_TRUE(strstr(t->input, " | reply") != NULL);
    escape_mode(t); ASSERT_STREQ(t->draft, "");
    t->selected_kind = 4; t->selected_key[0] = 0;
    normal_key(t, '\r'); ASSERT_EQ_INT(t->mode, MODE_NORMAL);
    close(pair[0]); close(pair[1]); free(t);
}

int main(void) {
    ASSERT_TRUE(setlocale(LC_CTYPE, "") != NULL);
    editor_tests(); navigation_tests();
    puts("TUI composer and navigation tests passed");
    return 0;
}
