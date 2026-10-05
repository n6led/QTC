#include "../src/tui.c"
#include "test.h"
static void type(tui_ctx *t, const char *s) { process_input(t, (const uint8_t *)s, strlen(s)); }
static void silent(int peer) {
    struct pollfd p = {.fd = peer, .events = POLLIN}; ASSERT_EQ_INT(poll(&p, 1, 0), 0);
}
int main(void) {
    int pair[2]; ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t);
    t->fd = pair[0]; t->width = 80; t->height = 24;
    strcpy(t->draft, "saved draft");
    normal_key(t, 'i'); ASSERT_EQ_INT(t->mode, MODE_IMPORT_CONTACT);
    type(t, "meshcore://aAbB00ff"); normal_key(t, 27);
    ASSERT_EQ_INT(t->mode, MODE_NORMAL); ASSERT_STREQ(t->draft, "saved draft"); silent(pair[1]);
    normal_key(t, 'i'); type(t, "meshcore://zz"); normal_key(t, '\r');
    ASSERT_EQ_INT(t->mode, MODE_IMPORT_CONTACT); ASSERT_TRUE(t->card_error[0]); silent(pair[1]);
    normal_key(t, 27); normal_key(t, 'i'); type(t, "meshcore://aAbB00ff"); normal_key(t, '\r');
    qtc_ipc_frame f; ASSERT_EQ_INT(qtc_ipc_recv_blocking(pair[1], &f, 1000), 0);
    ASSERT_EQ_INT(f.type, QTC_IPC_IMPORT_CONTACT); ASSERT_EQ_INT(f.length, 4);
    ASSERT_EQ_INT(f.payload[0], 0xaa); ASSERT_EQ_INT(f.payload[3], 255);
    ASSERT_EQ_INT(t->mode, MODE_NORMAL); silent(pair[1]);
    normal_key(t, 'i'); type(t, "meshcore://");
    for (size_t i = 0; i < 2 * (QTC_MAX_FRAME - 1) + 2; i++) normal_key(t, 'a');
    ASSERT_TRUE(t->card_overflow); normal_key(t, '\r');
    ASSERT_EQ_INT(t->mode, MODE_IMPORT_CONTACT); silent(pair[1]);
    screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
    render_modal(t, &s); ASSERT_TRUE(s.cursor_c > 0 && s.cursor_c < t->width); screen_free(&s);
    normal_key(t, 127); normal_key(t, '\r'); silent(pair[1]); /* Overflow never submits a truncated card. */
    normal_key(t, 27);
    qtc_contact *ct = &t->state.contacts[0]; t->state.contact_count = 1;
    strcpy(ct->id, "alice"); strcpy(ct->name, "Alice"); ct->node_type = QTC_NODE_PERSON;
    menu_item items[8]; qtc_roster r;
    ASSERT_EQ_INT(build_menu(t, items, 8, &r), 1); ASSERT_EQ_INT(items[0].kind, 3);
    bool contacts = false, route = false;
    for (size_t i = 0; i < r.pinned_count; i++) if (!strcmp(r.pinned[i].label, "-- CONTACTS --")) {
        contacts = true; ASSERT_EQ_INT(r.pinned[i].kind, 4);
    }
    for (size_t i = 0; i < r.scrollable_count; i++) if (!strcmp(r.scrollable[i].label, "Unknown route (flood)")) {
        route = true; ASSERT_EQ_INT(r.scrollable[i].kind, 4);
    }
    ASSERT_TRUE(contacts && route);
    t->draft[0] = 0; move_menu(t, 1); ASSERT_STREQ(displayed_key(t), "alice"); silent(pair[1]);
    normal_key(t, '\r'); ASSERT_EQ_INT(t->mode, MODE_COMPOSE); ASSERT_STREQ(t->open_key, "alice");
    close(pair[0]); close(pair[1]); free(t);
    puts("TUI contact import tests passed"); return 0;
}
