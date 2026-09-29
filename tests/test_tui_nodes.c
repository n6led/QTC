/* Exercise the real F7 handlers and Unicode-aware framebuffer. */
#include "../src/tui.c"
#include "test.h"

static void draw(tui_ctx *t, char *text, size_t cap) {
    screen s;
    ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
    if (t->mode == MODE_NODE_DETAIL) render_node_detail(t, &s);
    else render_nodes(t, &s);
    size_t used = 0;
    for (int r = 6; r < t->height - 3; r++) {
        for (int col = 5; col < t->width - 5; col++) {
            screen_cell *cell = screen_at(&s, r, col);
            if (!cell->continuation && used + cell->len + 1 < cap) {
                memcpy(text + used, cell->bytes, cell->len); used += cell->len;
            }
        }
    }
    text[used] = 0;
    if (t->mode == MODE_NORMAL && t->selected_node[0]) {
        ASSERT_TRUE(t->node_position >= t->node_scroll);
        ASSERT_TRUE(t->node_position < t->node_scroll + node_rows(t));
        screen_cell *cell = screen_at(&s, 6 + (int)(t->node_position - t->node_scroll), 4);
        ASSERT_TRUE(cell->bytes[0] == '>' && cell->style == UI_SELECTED);
    }
    screen_free(&s);
}

static void test_columns(void) {
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    t->height = 18; t->state.contact_count = 3;
    for (size_t i = 0; i < 3; i++) {
        qtc_contact *c = &t->state.contacts[i];
        snprintf(c->id, sizeof(c->id), "node%zu", i);
        strcpy(c->prefix, "012345abcdef");
        c->node_type = QTC_NODE_REPEATER;
    }
    strcpy(t->state.contacts[0].name, "Short📡");
    memset(t->state.contacts[1].name, 'A', sizeof(t->state.contacts[1].name) - 1);
    for (int i = 0; i < 23; i++) strcat(t->state.contacts[2].name, "📡");
    strcpy(t->selected_node, "node2");
    const int widths[] = {68, 80, 100};
    for (size_t i = 0; i < QTC_ARRAY_LEN(widths); i++) {
        t->width = widths[i];
        screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
        render_nodes(t, &s);
        int key = t->width - 16, route = key - 11, type = route - 10;
        ASSERT_EQ_INT(screen_at(&s, 5, 7)->bytes[0], 'N');
        ASSERT_EQ_INT(screen_at(&s, 5, type)->bytes[0], 'T');
        for (int row = 6; row < 9; row++) {
            ASSERT_EQ_INT(screen_at(&s, row, type)->bytes[0], 'r');
            ASSERT_EQ_INT(screen_at(&s, row, route)->bytes[0], 'f');
            for (int j = 0; j < 12; j++)
                ASSERT_EQ_INT(screen_at(&s, row, key + j)->bytes[0], "012345abcdef"[j]);
            ASSERT_EQ_INT(screen_at(&s, row, type - 1)->bytes[0], ' ');
            ASSERT_EQ_INT(screen_at(&s, row, type - 2)->bytes[0], ' ');
            if (row == 7 || (row == 8 && t->width < 100))
                for (int j = type - 5; j < type - 2; j++)
                    ASSERT_EQ_INT(screen_at(&s, row, j)->bytes[0], '.');
        }
        ASSERT_EQ_INT(screen_at(&s, 8, 4)->bytes[0], '>');
        for (int col = 3; col < t->width - 4; col++)
            ASSERT_EQ_INT(screen_at(&s, 8, col)->style, UI_SELECTED);
        ASSERT_TRUE(screen_at(&s, 8, 8)->continuation);
        screen_free(&s);
    }
    free(t);
}

int main(void) {
    ASSERT_TRUE(setlocale(LC_CTYPE, "") != NULL);
    test_columns();
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    t->fd = -1; t->view = VIEW_NODES; t->width = 100; t->height = 18;
    char text[40000];
    draw(t, text, sizeof(text)); normal_key(t, '\r');
    ASSERT_EQ_INT(t->mode, MODE_NORMAL); ASSERT_STREQ(t->selected_node, "");
    for (size_t i = 0; i < 60; i++) {
        qtc_contact *c = &t->state.contacts[i];
        snprintf(c->id, sizeof(c->id), "%064zu", i + 1);
        memcpy(c->prefix, c->id, 12);
        snprintf(c->name, sizeof(c->name), "Node %zu", i);
        c->node_type = i % 2 ? QTC_NODE_PERSON : QTC_NODE_REPEATER;
    }
    t->state.contact_count = 6; draw(t, text, sizeof(text));
    move_nodes(t, 99); draw(t, text, sizeof(text));
    ASSERT_EQ_INT(t->node_position, 2); ASSERT_EQ_INT(t->node_scroll, 0);
    t->state.contact_count = 60; move_nodes(t, -99);
    for (int i = 0; i < 29; i++) {
        size_t old = t->node_scroll;
        if (i % 2) special_key(t, "\x1b[B"); else normal_key(t, 'j');
        draw(t, text, sizeof(text));
        ASSERT_EQ_INT(t->node_position, i + 1);
        if ((size_t)i + 1 < old + node_rows(t)) ASSERT_EQ_INT(t->node_scroll, old);
    }
    move_nodes(t, 1); ASSERT_EQ_INT(t->node_position, 29);
    ASSERT_EQ_INT(t->node_scroll, 21);
    t->height = 20; draw(t, text, sizeof(text)); ASSERT_EQ_INT(t->node_scroll, 19);
    t->height = 18; draw(t, text, sizeof(text)); ASSERT_EQ_INT(t->node_scroll, 21);
    t->height = 60; draw(t, text, sizeof(text)); ASSERT_EQ_INT(t->node_scroll, 0);
    t->height = 18;
    for (int i = 28; i >= 0; i--) {
        if (i % 2) special_key(t, "\x1b[A"); else normal_key(t, 'k');
        draw(t, text, sizeof(text)); ASSERT_EQ_INT(t->node_position, i);
    }
    move_nodes(t, -1); ASSERT_EQ_INT(t->node_position, 0);
    move_nodes(t, 15);
    char id[QTC_MAX_ID]; qtc_strlcpy(id, t->selected_node, sizeof(id));
    qtc_contact tmp = t->state.contacts[30];
    t->state.contacts[30] = t->state.contacts[2]; t->state.contacts[2] = tmp;
    draw(t, text, sizeof(text)); ASSERT_STREQ(t->selected_node, id); ASSERT_EQ_INT(t->node_position, 1);
    qtc_contact *c = find_contact(t, id);
    qtc_strlcpy(c->alias, "🌱MeshGarden🍎 BOT", sizeof(c->alias));
    memset(c->name, 'x', sizeof(c->name) - 1);
    /* Opening, scrolling and leaving details must be entirely local. */
    int sockets[2];
    ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    t->fd = sockets[0];
    normal_key(t, '\r'); ASSERT_EQ_INT(t->mode, MODE_NODE_DETAIL);
    draw(t, text, sizeof(text)); ASSERT_TRUE(strstr(text, c->alias) != NULL);
    t->height = 60; draw(t, text, sizeof(text));
    ASSERT_TRUE(strstr(text, id) != NULL);
    ASSERT_TRUE(strstr(text, "Last heard: unknown") != NULL);
    ASSERT_TRUE(strstr(text, "Location: unavailable/unverified") != NULL);
    ASSERT_TRUE(strstr(text, "Flags: 0x00") != NULL);
    ASSERT_TRUE(strstr(text, "1970") == NULL);
    c->route_known = true; c->route_hops = 2; c->favorite = true;
    c->unread = 3; c->flags = 0xa5; c->last_heard = 1700000000;
    qtc_strlcpy(c->favorite_group, "Infrastructure", sizeof(c->favorite_group));
    draw(t, text, sizeof(text));
    ASSERT_TRUE(strstr(text, "Route: known, 2 hops") != NULL);
    ASSERT_TRUE(strstr(text, "Favorite: yes") != NULL);
    ASSERT_TRUE(strstr(text, "Favorite group: Infrastructure") != NULL);
    ASSERT_TRUE(strstr(text, "Unread: 3") != NULL);
    ASSERT_TRUE(strstr(text, "Last heard: 2023-") != NULL);
    t->width = 68; draw(t, text, sizeof(text));
    char compact[40000]; size_t used = 0;
    for (size_t i = 0; text[i]; i++) if (text[i] != ' ') compact[used++] = text[i];
    compact[used] = 0; ASSERT_TRUE(strstr(compact, id) != NULL);
    ASSERT_TRUE(strstr(compact, c->name) != NULL);
    t->height = 18; special_key(t, "\x1b[6~"); draw(t, text, sizeof(text));
    ASSERT_TRUE(t->node_detail_scroll > 0); ASSERT_TRUE(strstr(text, "Flags: 0xA5") != NULL);
    special_key(t, "\x1b[5~"); draw(t, text, sizeof(text));
    ASSERT_EQ_INT(t->node_detail_scroll, 0);
    ASSERT_STREQ(t->selected_node, id);
    normal_key(t, 27); ASSERT_EQ_INT(t->mode, MODE_NORMAL);
    draw(t, text, sizeof(text)); ASSERT_STREQ(t->selected_node, id);
    normal_key(t, '\r'); special_key(t, "\x1b[18~"); ASSERT_EQ_INT(t->mode, MODE_NORMAL);
    normal_key(t, '\r'); c->node_type = QTC_NODE_PERSON;
    draw(t, text, sizeof(text)); ASSERT_TRUE(strstr(text, "no longer available") != NULL);
    normal_key(t, 27); draw(t, text, sizeof(text));
    ASSERT_TRUE(strcmp(t->selected_node, id) != 0); ASSERT_EQ_INT(t->node_position, 1);
    t->state.contact_count = 0; draw(t, text, sizeof(text));
    ASSERT_STREQ(t->selected_node, ""); ASSERT_EQ_INT(t->node_scroll, 0);
    char unexpected;
    ASSERT_EQ_INT(recv(sockets[1], &unexpected, 1, MSG_DONTWAIT), -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    close(sockets[0]); close(sockets[1]); t->fd = -1;
    special_key(t, "\x1b[18~"); ASSERT_EQ_INT(t->view, VIEW_MESSAGES);
    set_view(t, VIEW_NODES); normal_key(t, 27); ASSERT_EQ_INT(t->view, VIEW_MESSAGES);
    free(t); puts("TUI node tests passed"); return 0;
}
