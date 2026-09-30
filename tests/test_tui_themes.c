#include "../src/tui.c"
#include "test.h"
#include "qtc/db.h"

static bool contains(screen *s, const char *text) {
    char row[4096];
    for (int r = 0; r < s->h; r++) {
        size_t n = 0;
        for (int c = 0; c < s->w; c++) {
            screen_cell *cell = screen_at(s, r, c);
            if (!cell->continuation && n + cell->len < sizeof(row)) {
                memcpy(row + n, cell->bytes, cell->len); n += cell->len;
            }
        }
        row[n] = 0;
        if (strstr(row, text)) return true;
    }
    return false;
}

static void test_help_borders(void) {
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    const int widths[] = {68, 80, 100};
    const int heights[] = {18, 28};
    const size_t scrolls[] = {0, 8, 25, 1000};
    for (size_t wi = 0; wi < QTC_ARRAY_LEN(widths); wi++) {
        for (size_t hi = 0; hi < QTC_ARRAY_LEN(heights); hi++) {
            t->width = widths[wi]; t->height = heights[hi];
            int w = t->width > 90 ? 84 : t->width - 4;
            int left = (t->width - w) / 2, right = left + w - 1;
            int top = 2, h = t->height - 4;
            for (size_t si = 0; si < QTC_ARRAY_LEN(scrolls); si++) {
                screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
                /* Reproduce a chat glyph straddling the help box near its bottom.
                 * A raw box fill alone leaves the outside lead cell alive. */
                int row = top + h - 4;
                screen_put_text(&s, row, left - 1, 2, "📡", UI_INCOMING);
                screen_put_text(&s, row, right, 2, "界", UI_INCOMING);
                ASSERT_TRUE(screen_at(&s, row, left)->continuation);
                t->help_scroll = scrolls[si];
                render_help(t, &s);
                ASSERT_EQ_INT(screen_at(&s, row, left - 1)->width, 1);
                ASSERT_EQ_INT(screen_at(&s, row, right + 1)->continuation, 0);
                for (int r = top + 1; r < top + h - 1; r++) {
                    ASSERT_EQ_INT(screen_at(&s, r, left)->bytes[0], '|');
                    ASSERT_EQ_INT(screen_at(&s, r, right)->bytes[0], '|');
                    ASSERT_EQ_INT(screen_at(&s, r, left)->style, UI_BORDER);
                    ASSERT_EQ_INT(screen_at(&s, r, right)->style, UI_BORDER);
                    /* Model sequential emission/cursor re-anchor positions, not fonts. */
                    int cursor = 0;
                    for (int c = 0; c <= right; c++) {
                        screen_cell *cell = screen_at(&s, r, c);
                        if (cell->continuation) continue;
                        if (c == left || c == right) ASSERT_EQ_INT(cursor, c);
                        cursor += cell->width;
                        if (cell->len > 1 || cell->width > 1) cursor = c + cell->width;
                    }
                }
                if (!scrolls[si]) {
                    ASSERT_EQ_INT(screen_at(&s, top + 2, left + 2)->style, UI_SECTION);
                    for (int r = top + 3; r < top + 8; r++) {
                        ASSERT_EQ_INT(screen_at(&s, r, left + 2)->style, UI_ACCENT);
                        ASSERT_EQ_INT(screen_at(&s, r, left + 18)->style, UI_NORMAL);
                        ASSERT_TRUE(screen_at(&s, r, left + 18)->bytes[0] != ' ');
                    }
                }
                screen_free(&s);
            }
        }
    }
    free(t);
}

static void test_highlight_contrast(void) {
    const ui_style styles[] = {UI_SELECTED, UI_NAVIGATION};
    for (int i = 0; i < QTC_THEME_COUNT; i++) {
        const ui_theme *theme = &THEMES[i];
        for (size_t j = 0; j < QTC_ARRAY_LEN(styles); j++) {
            const char *sgr = theme->sgr[styles[j]];
            /* Reset attributes first: bold-black must not become bright gray.
             * Explicit black on bright backgrounds, or reset terminal reverse. */
            ASSERT_TRUE(strncmp(sgr, "\x1b[0;", 4) == 0);
            ASSERT_TRUE(strstr(sgr, ";7m") != NULL ||
                        strstr(sgr, ";30;10") != NULL);
        }
        ASSERT_TRUE(strcmp(theme->sgr[UI_NAVIGATION], theme->sgr[UI_STATUS]) != 0);
        ASSERT_TRUE(strcmp(theme->sgr[UI_ACCENT], theme->sgr[UI_NORMAL]) != 0);
        ASSERT_TRUE(strstr(theme->sgr[UI_ACCENT], ";4;") != NULL ||
                    strstr(theme->sgr[UI_ACCENT], ";4m") != NULL);
        ASSERT_TRUE(strcmp(theme->sgr[UI_INPUT], theme->sgr[UI_NORMAL]) != 0);
        ASSERT_TRUE(strstr(theme->sgr[UI_RECONNECTING], ";33;") == NULL);
    }
    const ui_theme *high = &THEMES[qtc_tui_theme_index("high-contrast")];
    ASSERT_STREQ(high->sgr[UI_SELECTED], "\x1b[0;30;107m");
}

int main(void) {
    (void)setlocale(LC_CTYPE, "");
    test_help_borders();
    test_highlight_contrast();
    tui_ctx *t = calloc(1, sizeof(*t)); ASSERT_TRUE(t != NULL);
    t->fd = -1;
    qtc_db db;
    ASSERT_EQ_INT(qtc_db_open(&db, ":memory:"), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    ASSERT_EQ_INT(qtc_db_load_state(&db, &t->state), 0);
    qtc_db_close(&db);
    ASSERT_STREQ(active_theme(t)->name, "signal");
    const char *names[] = {"signal", "amber", "phosphor", "high-contrast", "classic", "midnight", "mono"};
    for (size_t i = 0; i < QTC_ARRAY_LEN(names); i++) {
        int index = qtc_tui_theme_index(names[i]);
        ASSERT_TRUE(index >= 0);
        ASSERT_STREQ(theme_name(index), names[i]);
        for (int j = 0; j < UI_STYLE_COUNT; j++) {
            const char *sgr = THEMES[index].sgr[j];
            ASSERT_TRUE(sgr && strncmp(sgr, "\x1b[", 2) == 0);
            ASSERT_EQ_INT(sgr[strlen(sgr) - 1], 'm');
            ASSERT_TRUE(strstr(sgr, "38;2") == NULL);
        }
    }
    ASSERT_EQ_INT(qtc_tui_theme_index("invalid"), -1);
    ASSERT_EQ_INT(qtc_tui_theme_index(NULL), -1);
    t->theme_overridden = true; t->theme_override = 1;
    ASSERT_STREQ(active_theme(t)->name, "amber");
    ASSERT_EQ_INT(t->state.settings.theme, QTC_THEME_DEFAULT);
    t->theme_overridden = false;
    ASSERT_STREQ(radio_label(t), "RECONNECTING");
    t->loading = true; ASSERT_STREQ(radio_label(t), "DISCONNECTED");
    t->loading = false; t->state.radio_connected = true;
    ASSERT_STREQ(radio_label(t), "INITIALIZING");
    t->session_ready = true; ASSERT_STREQ(radio_label(t), "RADIO READY");
    qtc_ipc_status_details details = {0};
    strcpy(details.session, "ready");
    qtc_ipc_frame frame = {.type = QTC_IPC_STATUS_DETAILS, .length = sizeof(details)};
    memcpy(frame.payload, &details, sizeof(details));
    t->session_ready = false; ipc_frame(&frame, t);
    ASSERT_TRUE(t->session_ready);
    qtc_ipc_status_payload status = {.radio_connected = false};
    frame.type = QTC_IPC_STATUS; frame.length = sizeof(status);
    memcpy(frame.payload, &status, sizeof(status)); ipc_frame(&frame, t);
    ASSERT_STREQ(radio_label(t), "RECONNECTING"); ASSERT_TRUE(!t->session_ready);
    status.radio_connected = true; strcpy(status.message, "MeshCore session ready");
    memcpy(frame.payload, &status, sizeof(status)); ipc_frame(&frame, t);
    ASSERT_STREQ(radio_label(t), "RADIO READY");
    t->demo = true; ASSERT_STREQ(radio_label(t), "DEMO"); t->demo = false;

    t->state.contact_count = 1;
    qtc_contact *ct = &t->state.contacts[0];
    strcpy(ct->id, "node"); strcpy(ct->prefix, "012345abcdef");
    strcpy(ct->name, "Long network node name 📡📡📡📡📡📡"); ct->node_type = QTC_NODE_REPEATER;
    strcpy(t->selected_node, ct->id);
    t->open_kind = QTC_CONV_CHANNEL; strcpy(t->open_key, "0");
    t->state.channel_count = 1; t->state.channels[0].configured = true;
    strcpy(t->state.channels[0].name, "Public");
    t->state.message_count = 2;
    for (int i = 0; i < 2; i++) {
        qtc_message *m = &t->state.messages[i];
        m->conversation_kind = QTC_CONV_CHANNEL; strcpy(m->conversation_key, "0");
        snprintf(m->message_key, sizeof(m->message_key), "message%d", i);
        strcpy(m->text, i ? "hello @[Radio]" : "outgoing 📡");
        m->direction = i ? QTC_MSG_INCOMING : QTC_MSG_OUTGOING;
        m->status = QTC_MSG_SENT;
    }
    strcpy(t->selected_message, "message1");
    const int widths[] = {68, 80, 120};
    for (size_t w = 0; w < QTC_ARRAY_LEN(widths); w++) {
        t->width = widths[w]; t->height = 18;
        for (int view = VIEW_MESSAGES; view <= VIEW_SETTINGS; view++) {
            screen baseline; ASSERT_EQ_INT(screen_init(&baseline, t->width, t->height), 0);
            for (int theme = 0; theme < QTC_THEME_COUNT; theme++) {
                t->state.settings.theme = theme; t->view = (tui_view)view;
                screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
                t->mode = view == VIEW_MESSAGES ? MODE_MESSAGE_SELECT : MODE_NORMAL;
                render_shell(t, &s);
                if (view == VIEW_NODES) render_nodes(t, &s);
                else if (view == VIEW_MESSAGES) render_messages(t, &s);
                else if (view == VIEW_CHANNELS) render_channels(t, &s);
                else render_node_detail(t, &s);
                ASSERT_TRUE(contains(&s, "RADIO READY"));
                if (view == VIEW_MESSAGES) {
                    ASSERT_TRUE(contains(&s, "hello @[Radio]"));
                    ASSERT_TRUE(contains(&s, "outgoing 📡"));
                    bool selected = false;
                    for (int row = 5; row < t->height - 2; row++)
                        for (int col = 30; col < t->width; col++)
                            if (screen_at(&s, row, col)->style == UI_SELECTED) selected = true;
                    ASSERT_TRUE(selected);
                }
                if (view == VIEW_NODES) {
                    ASSERT_EQ_INT(screen_at(&s, 6, 4)->style, UI_SELECTED);
                    ASSERT_EQ_INT(screen_at(&s, 6, t->width - 16)->bytes[0], '0');
                    ASSERT_TRUE(contains(&s, "? Help"));
                }
                if (!theme) memcpy(baseline.cells, s.cells, (size_t)s.w * s.h * sizeof(screen_cell));
                else ASSERT_TRUE(memcmp(baseline.cells, s.cells, (size_t)s.w * s.h * sizeof(screen_cell)) == 0);
                screen_free(&s);
            }
            screen_free(&baseline);
        }
        screen s; ASSERT_EQ_INT(screen_init(&s, t->width, t->height), 0);
        t->mode = MODE_NORMAL; normal_key(t, '?'); ASSERT_TRUE(t->help_open);
        render_help(t, &s); ASSERT_TRUE(contains(&s, "KEYBOARD HELP"));
        special_key(t, "\x1b[6~"); ASSERT_EQ_INT(t->help_scroll, 5);
        t->help_scroll = 1000; render_help(t, &s); ASSERT_TRUE(t->help_scroll < 1000);
        normal_key(t, 27); ASSERT_TRUE(!t->help_open);
        t->mode = MODE_NODE_DETAIL;
        normal_key(t, '?'); normal_key(t, '?'); ASSERT_TRUE(!t->help_open);
        ASSERT_EQ_INT(t->mode, MODE_NODE_DETAIL); ASSERT_STREQ(t->selected_node, "node");
        t->mode = MODE_COMPOSE; t->input_len = 0; t->input[0] = 0;
        normal_key(t, '?'); ASSERT_STREQ(t->input, "?"); ASSERT_TRUE(!t->help_open);
        t->mode = MODE_NORMAL;
        screen_free(&s);
    }
    /* Semantic mention styling and bytes stay independent of palette choice. */
    for (int i = 0; i < QTC_THEME_COUNT; i++) {
        screen s; ASSERT_EQ_INT(screen_init(&s, 80, 18), 0);
        t->state.settings.theme = i;
        screen_put_mentioned(&s, 0, 0, 70, "hi @[Radio] 📡", "hi @[Radio] 📡", 0, UI_INCOMING);
        ASSERT_TRUE(contains(&s, "hi @[Radio] 📡"));
        ASSERT_EQ_INT(screen_at(&s, 0, 4)->style, UI_ACCENT);
        screen_free(&s);
    }
    free(t);
    puts("TUI theme and help tests passed");
    return 0;
}
