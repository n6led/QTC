#define _GNU_SOURCE
#include "qtc/tui.h"
#include "qtc/invite.h"
#include "qtc/ipc.h"
#include "qtc/message.h"
#include "qtc/mention.h"
#include "qtc/notify.h"
#include "qtc/roster.h"
#include "qtc/util.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

typedef enum { VIEW_MESSAGES, VIEW_CHANNELS, VIEW_NODES, VIEW_SETTINGS } tui_view;
typedef enum {
    FEEDBACK_NONE = 0, FEEDBACK_INFO, FEEDBACK_SUCCESS, FEEDBACK_ERROR
} feedback_level;
typedef enum {
    MODE_NORMAL, MODE_SEARCH, MODE_COMPOSE, MODE_CREATE_CHANNEL, MODE_JOIN_CHANNEL,
    MODE_INVITE_PICKER, MODE_INVITE_REVIEW, MODE_ROTATE_CONFIRM, MODE_LEAVE_CONFIRM,
    MODE_INCOMING_INVITE, MODE_ALIAS, MODE_FAVORITE_GROUP, MODE_DEVICE_NAME,
    MODE_TX_POWER, MODE_THEME_PICKER, MODE_PRESET_PICKER, MODE_MESSAGE_SELECT,
    MODE_NODE_DETAIL
} tui_mode;

typedef struct {
    int kind;
    int source_index;
    char key[QTC_MAX_ID];
} menu_item;

typedef struct {
    int fd;
    qtc_ipc_reader reader;
    qtc_state state;
    bool loading;
    bool running;
    bool dirty;
    struct termios original;
    bool terminal_saved;
    int stdout_flags;
    bool stdout_flags_saved;
    char *output;
    size_t output_len;
    size_t output_off;
    int width;
    int height;
    tui_view view;
    tui_mode mode;
    char status[160];
    char action_feedback[160];
    feedback_level action_feedback_level;
    int64_t action_feedback_until;
    bool advert_feedback_pending;
    char search[QTC_MAX_NAME];
    char input[QTC_MAX_TEXT];
    size_t input_len;
    char draft[QTC_MAX_TEXT];
    char reply_backup[QTC_MAX_TEXT];
    bool replying;
    char selected_message[160];
    bool mention_active;
    size_t mention_start;
    size_t mention_cursor;
    int selected_kind;
    char selected_key[QTC_MAX_ID];
    int selected_channel;
    char selected_node[QTC_MAX_ID];
    size_t node_position;
    size_t node_scroll;
    size_t node_detail_scroll;
    size_t contact_scroll;
    size_t history_scroll;
    qtc_conversation_kind open_kind;
    char open_key[QTC_MAX_ID];
    char invite_cursor[QTC_MAX_ID];
    char invite_search[QTC_MAX_NAME];
    char invite_ids[64][QTC_MAX_ID];
    size_t invite_count;
    bool confirm_action;
    int64_t modal_enter_block_until;
    int64_t last_ctrl_q;
    int64_t incoming_invite_id;
    bool help_open;
    size_t help_scroll;
    bool theme_overridden;
    int theme_override;
    bool demo;
    bool session_ready;
    char profile[64];
    int theme_cursor;
    int preset_cursor;
    char banner_title[QTC_MAX_NAME];
    char banner_body[256];
    int64_t banner_until;
    char escape_buf[32];
    size_t escape_len;
} tui_ctx;

typedef struct {
    const char *name;
    double freq_mhz;
    double bw_khz;
    int sf;
    int cr;
} radio_preset;

static const radio_preset RADIO_PRESETS[] = {
    {"Europe / UK", 867.500, 250.0, 10, 5},
    {"Europe / UK narrow", 869.618, 62.5, 8, 5},
    {"USA / Canada", 910.525, 62.5, 7, 5},
    {"Australia / New Zealand", 915.800, 250.0, 10, 5}
};

static volatile sig_atomic_t g_resize;
static void sigwinch_handler(int sig) { (void)sig; g_resize = 1; }
static const char *theme_name(int index);
static int theme_index(const tui_ctx *t);
static void move_message(tui_ctx *t, int delta);
static void reply_message(tui_ctx *t);

static void set_action_feedback(tui_ctx *t, const char *message,
                                feedback_level level, int64_t duration_ms) {
    qtc_strlcpy(t->action_feedback, message, sizeof(t->action_feedback));
    t->action_feedback_level = level;
    t->action_feedback_until = qtc_now_millis() + duration_ms;
    t->dirty = true;
}

static bool is_advert_feedback(const char *message) {
    return message != NULL && strstr(message, "advertisement") != NULL;
}

static feedback_level advert_feedback_level(const char *message) {
    if (message == NULL) return FEEDBACK_INFO;
    if (strstr(message, "failed") != NULL ||
        strstr(message, "timed out") != NULL ||
        strstr(message, "not connected") != NULL ||
        strstr(message, "Could not") != NULL)
        return FEEDBACK_ERROR;
    if (strstr(message, "sent") != NULL)
        return FEEDBACK_SUCCESS;
    return FEEDBACK_INFO;
}

static void tty_write_all(const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
}

/* Terminal output must never block IPC processing. A full frame uses absolute
 * cursor addressing, so replacing a partially written older frame with the
 * newest frame is safe: the newest frame rewrites every visible row. */
static void discard_output(tui_ctx *t) {
    free(t->output);
    t->output = NULL;
    t->output_len = 0;
    t->output_off = 0;
}

static void queue_output(tui_ctx *t, char *data, size_t len) {
    discard_output(t);
    t->output = data;
    t->output_len = len;
}

static void flush_output(tui_ctx *t) {
    while (t->output != NULL && t->output_off < t->output_len) {
        ssize_t n = write(STDOUT_FILENO, t->output + t->output_off,
                          t->output_len - t->output_off);
        if (n > 0) {
            t->output_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        discard_output(t);
        return;
    }
    if (t->output != NULL && t->output_off == t->output_len)
        discard_output(t);
}

static int copy_to_clipboard(const char *text) {
    static const char *commands[] = {
        "wl-copy 2>/dev/null",
        "xclip -selection clipboard 2>/dev/null",
        "xsel --clipboard --input 2>/dev/null",
        "pbcopy 2>/dev/null"
    };
    if (text == NULL || *text == 0) return -1;
    for (size_t i = 0; i < QTC_ARRAY_LEN(commands); i++) {
        FILE *pipe = popen(commands[i], "w");
        if (pipe == NULL) continue;
        size_t len = strlen(text);
        bool ok = fwrite(text, 1, len, pipe) == len;
        int rc = pclose(pipe);
        if (ok && rc == 0) return 0;
    }
    return -1;
}

static const char *contact_name(const qtc_contact *c) { return c->alias[0] ? c->alias : c->name; }

static qtc_contact *find_contact(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->state.contact_count; i++)
        if (strcmp(t->state.contacts[i].id, id) == 0 || strcmp(t->state.contacts[i].prefix, id) == 0) return &t->state.contacts[i];
    return NULL;
}
static qtc_channel *find_channel(tui_ctx *t, int index) {
    for (size_t i = 0; i < t->state.channel_count; i++) if (t->state.channels[i].index == index) return &t->state.channels[i];
    return NULL;
}
static qtc_invitation *find_invitation(tui_ctx *t, int64_t id) {
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].id == id) return &t->state.invitations[i];
    return NULL;
}

static int set_raw_terminal(tui_ctx *t) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &t->original) != 0) return -1;
    t->terminal_saved = true; struct termios raw = t->original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN | ISIG); raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_oflag &= (tcflag_t)~OPOST; raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    const char *enter = "\x1b[?1049h\x1b[?25l";
    tty_write_all(enter, strlen(enter));
    t->stdout_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (t->stdout_flags >= 0) {
        t->stdout_flags_saved = true;
        (void)fcntl(STDOUT_FILENO, F_SETFL, t->stdout_flags | O_NONBLOCK);
    }
    return 0;
}

static void restore_terminal(tui_ctx *t) {
    discard_output(t);
    if (t->stdout_flags_saved)
        (void)fcntl(STDOUT_FILENO, F_SETFL, t->stdout_flags);
    if (t->terminal_saved) (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->original);
    const char *leave = "\x1b[0m\x1b[?25h\x1b[?1049l";
    tty_write_all(leave, strlen(leave));
}

static void update_size(tui_ctx *t) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        t->width = ws.ws_col > 40 ? ws.ws_col : 80; t->height = ws.ws_row > 12 ? ws.ws_row : 24;
    } else { t->width = 100; t->height = 30; }
    if (t->width > 300) t->width = 300;
    if (t->height > 120) t->height = 120;
}

static void tui_upsert_contact(tui_ctx *t, const qtc_contact *contact) {
    for (size_t i = 0; i < t->state.contact_count; i++) {
        if (strcmp(t->state.contacts[i].id, contact->id) == 0) {
            t->state.contacts[i] = *contact;
            t->state.revisions.contacts++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.contact_count < QTC_MAX_CONTACTS) {
        t->state.contacts[t->state.contact_count++] = *contact;
        t->state.revisions.contacts++;
        t->dirty = true;
    }
}

static void tui_upsert_channel(tui_ctx *t, const qtc_channel *channel) {
    for (size_t i = 0; i < t->state.channel_count; i++) {
        if (t->state.channels[i].index == channel->index) {
            t->state.channels[i] = *channel;
            t->state.revisions.channels++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.channel_count < QTC_MAX_CHANNELS) {
        t->state.channels[t->state.channel_count++] = *channel;
        t->state.revisions.channels++;
        t->dirty = true;
    }
}

static void tui_upsert_message(tui_ctx *t, const qtc_message *message) {
    for (size_t i = 0; i < t->state.message_count; i++) {
        if (strcmp(t->state.messages[i].message_key, message->message_key) == 0) {
            t->state.messages[i] = *message;
            t->state.revisions.messages++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.message_count == QTC_MAX_MESSAGES) {
        memmove(&t->state.messages[0], &t->state.messages[1],
                (QTC_MAX_MESSAGES - 1U) * sizeof(t->state.messages[0]));
        t->state.message_count--;
    }
    t->state.messages[t->state.message_count++] = *message;
    t->state.revisions.messages++;
    t->dirty = true;
}

static void tui_upsert_invitation(tui_ctx *t, const qtc_invitation *invitation) {
    for (size_t i = 0; i < t->state.invitation_count; i++) {
        if (t->state.invitations[i].id == invitation->id) {
            t->state.invitations[i] = *invitation;
            t->dirty = true;
            return;
        }
    }
    if (t->state.invitation_count < QTC_MAX_INVITATIONS) {
        t->state.invitations[t->state.invitation_count++] = *invitation;
        t->dirty = true;
    }
}

static void ipc_frame(const qtc_ipc_frame *f, void *userdata) {
    tui_ctx *t = userdata;
    switch (f->type) {
        case QTC_IPC_STATE_BEGIN:
            t->loading = true; t->state.contact_count = t->state.channel_count = t->state.message_count = t->state.invitation_count = 0;
            if (f->length == sizeof(qtc_revisions)) memcpy(&t->state.revisions, f->payload, sizeof(qtc_revisions));
            break;
        case QTC_IPC_CONTACT:
            if (f->length == sizeof(qtc_contact)) {
                if (t->loading && t->state.contact_count < QTC_MAX_CONTACTS)
                    memcpy(&t->state.contacts[t->state.contact_count++], f->payload, sizeof(qtc_contact));
                else if (!t->loading)
                    tui_upsert_contact(t, (const qtc_contact *)f->payload);
            }
            break;
        case QTC_IPC_CHANNEL:
            if (f->length == sizeof(qtc_channel)) {
                if (t->loading && t->state.channel_count < QTC_MAX_CHANNELS)
                    memcpy(&t->state.channels[t->state.channel_count++], f->payload, sizeof(qtc_channel));
                else if (!t->loading)
                    tui_upsert_channel(t, (const qtc_channel *)f->payload);
            }
            break;
        case QTC_IPC_MESSAGE:
            if (f->length == sizeof(qtc_message)) {
                if (t->loading && t->state.message_count < QTC_MAX_MESSAGES)
                    memcpy(&t->state.messages[t->state.message_count++], f->payload, sizeof(qtc_message));
                else if (!t->loading)
                    tui_upsert_message(t, (const qtc_message *)f->payload);
            }
            break;
        case QTC_IPC_INVITATION:
            if (f->length == sizeof(qtc_invitation)) {
                if (t->loading && t->state.invitation_count < QTC_MAX_INVITATIONS)
                    memcpy(&t->state.invitations[t->state.invitation_count++], f->payload, sizeof(qtc_invitation));
                else if (!t->loading)
                    tui_upsert_invitation(t, (const qtc_invitation *)f->payload);
            }
            break;
        case QTC_IPC_SETTINGS:
            if (f->length == sizeof(qtc_settings)) {
                memcpy(&t->state.settings, f->payload, sizeof(qtc_settings));
                if (!t->loading) t->dirty = true;
            }
            break;
        case QTC_IPC_STATUS:
            if (f->length == sizeof(qtc_ipc_status_payload)) {
                const qtc_ipc_status_payload *s = (const void *)f->payload;
                if (!s->radio_connected) t->session_ready = false;
                else if (!t->state.radio_connected && !s->demo_mode && t->fd >= 0) {
                    /* Existing optional status details also identify an already-ready
                     * core on attach, without a radio query or a new IPC field. */
                    uint8_t request = QTC_IPC_STATUS_DETAILS_VERSION;
                    (void)qtc_ipc_send(t->fd, QTC_IPC_PING, &request, sizeof(request));
                }
                if (s->radio_connected && strcmp(s->message, "MeshCore session ready") == 0)
                    t->session_ready = true;
                t->demo = s->demo_mode;
                t->state.radio_connected = s->radio_connected;
                t->state.radio_max_channels = s->max_channels;
                t->state.radio_max_contacts = s->max_contacts;
                t->state.radio_tx_power = s->tx_power;
                t->state.radio_max_tx_power = s->max_tx_power;
                t->state.radio_freq = s->freq;
                t->state.radio_bw = s->bw;
                t->state.radio_sf = s->sf;
                t->state.radio_cr = s->cr;
                qtc_strlcpy(t->state.radio_name, s->radio_name, sizeof(t->state.radio_name));
                qtc_strlcpy(t->state.radio_model, s->radio_model, sizeof(t->state.radio_model));
                qtc_strlcpy(t->state.radio_version, s->radio_version, sizeof(t->state.radio_version));
                qtc_strlcpy(t->status, s->message, sizeof(t->status));
                if (is_advert_feedback(s->message)) {
                    feedback_level level = advert_feedback_level(s->message);
                    set_action_feedback(t, s->message, level,
                                        level == FEEDBACK_INFO ? 8000 : 6000);
                    if (level != FEEDBACK_INFO) t->advert_feedback_pending = false;
                }
                if (!t->loading) t->dirty = true;
            }
            break;
        case QTC_IPC_STATUS_DETAILS:
            if (f->length == sizeof(qtc_ipc_status_details)) {
                const qtc_ipc_status_details *d = (const void *)f->payload;
                t->session_ready = strcmp(d->session, "ready") == 0;
                t->dirty = true;
            }
            break;
        case QTC_IPC_STATE_END: t->loading = false; t->dirty = true; break;
        case QTC_IPC_BANNER:
            if (f->length == sizeof(qtc_ipc_banner_payload)) {
                const qtc_ipc_banner_payload *b = (const void *)f->payload;
                qtc_strlcpy(t->banner_title, b->title, sizeof(t->banner_title));
                qtc_strlcpy(t->banner_body, b->body, sizeof(t->banner_body));
                t->banner_until = qtc_now_millis() + 6000;
                t->dirty = true;
            }
            break;
        case QTC_IPC_CLIPBOARD_TEXT:
            if (f->length == sizeof(qtc_ipc_clipboard_payload)) {
                const qtc_ipc_clipboard_payload *p = (const void *)f->payload;
                if (copy_to_clipboard(p->text) == 0)
                    qtc_strlcpy(t->status, "MeshCore contact card copied to clipboard", sizeof(t->status));
                else
                    qtc_strlcpy(t->status, "Install wl-clipboard, xclip, or xsel to copy the contact card (pbcopy on macOS)", sizeof(t->status));
                t->dirty = true;
            }
            break;
        case QTC_IPC_ERROR: {
            const char *message = f->length ? (const char *)f->payload : QTC_DISPLAY_NAME " core error";
            qtc_strlcpy(t->status, message, sizeof(t->status));
            if (t->advert_feedback_pending) {
                set_action_feedback(t, message, FEEDBACK_ERROR, 7000);
                t->advert_feedback_pending = false;
            }
            t->dirty = true;
            break;
        }
        default: break;
    }
}

static int verify_core_version(int fd) {
    qtc_ipc_hello h = {.protocol_version = QTC_IPC_PROTOCOL_VERSION};
    qtc_strlcpy(h.client_name, "qtc-tui", sizeof(h.client_name));
    qtc_strlcpy(h.app_version, QTC_VERSION, sizeof(h.app_version));
    if (qtc_ipc_send(fd, QTC_IPC_HELLO, &h, sizeof(h)) != 0) return -1;
    qtc_ipc_frame frame;
    if (qtc_ipc_recv_blocking(fd, &frame, 1500) != 0 ||
        frame.type != QTC_IPC_CORE_INFO ||
        frame.length != sizeof(qtc_ipc_core_info)) {
        errno = EPROTO;
        return -1;
    }
    const qtc_ipc_core_info *info = (const void *)frame.payload;
    if (info->protocol_version != QTC_IPC_PROTOCOL_VERSION ||
        strcmp(info->app_version, QTC_VERSION) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static size_t build_menu(tui_ctx *t, menu_item *items, size_t max, qtc_roster *roster) {
    qtc_roster_build(&t->state, t->search, roster); size_t n = 0;
    for (size_t i = 0; i < roster->pinned_count && n < max; i++) {
        qtc_roster_row *r = &roster->pinned[i]; if (r->kind != 1 && r->kind != 2) continue;
        items[n].kind = r->kind; items[n].source_index = r->source_index;
        if (r->kind == 1) snprintf(items[n].key, sizeof(items[n].key), "%d", t->state.channels[r->source_index].index);
        else qtc_strlcpy(items[n].key, t->state.contacts[r->source_index].id, sizeof(items[n].key));
        n++;
    }
    for (size_t i = 0; i < roster->scrollable_count && n < max; i++) {
        qtc_roster_row *r = &roster->scrollable[i]; if (r->kind != 3) continue;
        items[n].kind = 3; items[n].source_index = r->source_index;
        qtc_strlcpy(items[n].key, t->state.contacts[r->source_index].id, sizeof(items[n].key)); n++;
    }
    return n;
}

static int selected_menu_pos(tui_ctx *t, menu_item *items, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == t->selected_kind && strcmp(items[i].key, t->selected_key) == 0) return (int)i;
    /* A contact changes roster kind when it is favorited or unfavorited. Keep the
     * identity selected instead of jumping to the first channel after the snapshot. */
    if (t->selected_kind == 2 || t->selected_kind == 3) {
        for (size_t i = 0; i < count; i++) {
            if ((items[i].kind == 2 || items[i].kind == 3) &&
                strcmp(items[i].key, t->selected_key) == 0) {
                t->selected_kind = items[i].kind;
                return (int)i;
            }
        }
    }
    if (count > 0) { t->selected_kind = items[0].kind; qtc_strlcpy(t->selected_key, items[0].key, sizeof(t->selected_key)); return 0; }
    t->selected_kind = 0; t->selected_key[0] = 0; return -1;
}

static void move_menu(tui_ctx *t, int delta) {
    menu_item items[QTC_MAX_CONTACTS + QTC_MAX_CHANNELS]; qtc_roster roster;
    size_t count = build_menu(t, items, QTC_ARRAY_LEN(items), &roster); int pos = selected_menu_pos(t, items, count);
    if (pos < 0) return;
    pos += delta;
    if (pos < 0) pos = 0;
    if ((size_t)pos >= count) pos = (int)count - 1;
    t->selected_kind = items[pos].kind; qtc_strlcpy(t->selected_key, items[pos].key, sizeof(t->selected_key)); t->dirty = true;
}

static void report_active_conversation(tui_ctx *t, bool active) {
    qtc_ipc_active_payload p = {0};
    p.active = active;
    p.kind = t->open_kind;
    qtc_strlcpy(p.key, active ? t->open_key : "", sizeof(p.key));
    (void)qtc_ipc_send(t->fd, QTC_IPC_ACTIVE_CONVERSATION, &p, sizeof(p));
}

static void leave_conversation(tui_ctx *t) {
    if (t->open_key[0]) report_active_conversation(t, false);
}

static void cancel_mention(tui_ctx *t) {
    /* Only a bare active trigger is transient; typed queries are user text. */
    if (t->mention_active && t->input_len == t->mention_start + 1 &&
        t->input[t->mention_start] == '@') {
        t->input_len = t->mention_start;
        t->input[t->input_len] = 0;
    }
    t->mention_active = false;
    t->dirty = true;
}

static void save_composer(tui_ctx *t) {
    if (t->mode == MODE_COMPOSE) {
        cancel_mention(t);
        qtc_strlcpy(t->draft, t->input, sizeof(t->draft));
        t->replying = false;
    }
    t->mention_active = false;
}

static void set_view(tui_ctx *t, tui_view view) {
    save_composer(t);
    if (t->view == VIEW_MESSAGES && view != VIEW_MESSAGES) leave_conversation(t);
    t->view = view;
    t->mode = MODE_NORMAL;
    if (view == VIEW_MESSAGES && t->open_key[0]) report_active_conversation(t, true);
    t->dirty = true;
}

static bool open_selected(tui_ctx *t) {
    qtc_conversation_kind kind = t->selected_kind == 1 ? QTC_CONV_CHANNEL : QTC_CONV_CONTACT;
    bool changed = kind != t->open_kind || strcmp(t->selected_key, t->open_key) != 0;
    if (changed && t->draft[0]) {
        qtc_strlcpy(t->status, "Draft kept in current conversation; m to resume, clear and Enter to discard", sizeof(t->status));
        t->dirty = true; return false;
    }
    if (t->selected_kind == 1) { t->open_kind = QTC_CONV_CHANNEL; qtc_strlcpy(t->open_key, t->selected_key, sizeof(t->open_key)); }
    else if (t->selected_kind == 2 || t->selected_kind == 3) { t->open_kind = QTC_CONV_CONTACT; qtc_strlcpy(t->open_key, t->selected_key, sizeof(t->open_key)); }
    else return false;
    qtc_ipc_mark_read_payload p = {.kind = t->open_kind}; qtc_strlcpy(p.key, t->open_key, sizeof(p.key));
    (void)qtc_ipc_send(t->fd, QTC_IPC_MARK_READ, &p, sizeof(p));
    t->history_scroll = 0;
    if (changed) t->selected_message[0] = 0;
    report_active_conversation(t, true);
    t->dirty = true;
    return true;
}

static void start_input(tui_ctx *t, tui_mode mode) {
    save_composer(t);
    t->mode = mode;
    qtc_strlcpy(t->input, mode == MODE_COMPOSE ? t->draft : "", sizeof(t->input));
    t->input_len = strlen(t->input); t->mention_active = false; t->dirty = true;
}
static void append_input(tui_ctx *t, char c) {
    if (t->input_len + 1 < sizeof(t->input)) { t->input[t->input_len++] = c; t->input[t->input_len] = 0; t->dirty = true; }
}
static size_t utf8_previous_boundary(const char *text, size_t length) {
    if (length == 0) return 0;
    size_t pos = length - 1;
    while (pos > 0 && (((unsigned char)text[pos] & 0xc0U) == 0x80U)) pos--;
    return pos;
}
static void backspace_input(tui_ctx *t) {
    if (t->input_len) {
        t->input_len = utf8_previous_boundary(t->input, t->input_len);
        t->input[t->input_len] = 0;
        t->dirty = true;
    }
}

static void send_composed(tui_ctx *t) {
    qtc_trim(t->input);
    if (t->input[0] == 0) {
        t->draft[0] = 0; t->input_len = 0; t->replying = false;
        t->mention_active = false; t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->open_kind == QTC_CONV_CONTACT) {
        qtc_ipc_send_direct_payload p = {0}; qtc_strlcpy(p.contact_id, t->open_key, sizeof(p.contact_id)); qtc_strlcpy(p.text, t->input, sizeof(p.text));
        (void)qtc_ipc_send(t->fd, QTC_IPC_SEND_DIRECT, &p, sizeof(p));
    } else if (t->open_kind == QTC_CONV_CHANNEL) {
        qtc_ipc_send_channel_payload p = {.channel_index = atoi(t->open_key)}; qtc_strlcpy(p.text, t->input, sizeof(p.text));
        (void)qtc_ipc_send(t->fd, QTC_IPC_SEND_CHANNEL, &p, sizeof(p));
    }
    t->mode = MODE_NORMAL; t->input[0] = 0; t->input_len = 0; t->dirty = true;
    t->draft[0] = 0; t->replying = false; t->mention_active = false;
}

static bool display_sender(tui_ctx *t, const qtc_message *m, char *out, size_t cap) {
    if (m->direction == QTC_MSG_OUTGOING) return false;
    if (m->conversation_kind == QTC_CONV_CHANNEL) {
        if (m->part_total > 1 && m->part_index != 1) {
            const qtc_message *first = NULL;
            for (size_t i = 0; i < t->state.message_count; i++)
                if (t->state.messages[i].part_index == 1 && m->logical_key[0] &&
                    strcmp(t->state.messages[i].logical_key, m->logical_key) == 0)
                    first = &t->state.messages[i];
            if (!first) return false;
            m = first;
        }
        return qtc_channel_sender(m->text, out, cap);
    }
    qtc_contact *ct = find_contact(t, m->conversation_key);
    if (!ct || !qtc_mention_name_valid(contact_name(ct))) return false;
    qtc_strlcpy(out, contact_name(ct), cap);
    return true;
}

#define MENTION_CANDIDATES 64
static void add_mention(tui_ctx *t, char names[][QTC_MAX_NAME], size_t *count, const char *name) {
    if (*count >= MENTION_CANDIDATES || !qtc_mention_name_valid(name) ||
        !qtc_search_match(t->input + t->mention_start + 1, name)) return;
    for (size_t i = 0; i < *count; i++) if (strcmp(names[i], name) == 0) return;
    qtc_strlcpy(names[(*count)++], name, QTC_MAX_NAME);
}

static size_t mention_candidates(tui_ctx *t, char names[][QTC_MAX_NAME]) {
    size_t count = 0;
    for (size_t i = t->state.message_count; i > 0; i--) {
        const qtc_message *m = &t->state.messages[i - 1];
        char sender[QTC_MAX_NAME];
        if (m->conversation_kind == t->open_kind && strcmp(m->conversation_key, t->open_key) == 0 &&
            display_sender(t, m, sender, sizeof(sender))) add_mention(t, names, &count, sender);
    }
    if (t->open_kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(t, t->open_key);
        if (ct) add_mention(t, names, &count, contact_name(ct));
    }
    for (size_t i = 0; i < t->state.contact_count; i++)
        if (t->state.contacts[i].node_type == QTC_NODE_PERSON)
            add_mention(t, names, &count, contact_name(&t->state.contacts[i]));
    return count;
}

static void accept_mention(tui_ctx *t) {
    char names[MENTION_CANDIDATES][QTC_MAX_NAME], prefix[QTC_MAX_NAME + 4];
    size_t count = mention_candidates(t, names);
    if (!count) return;
    if (t->mention_cursor >= count) t->mention_cursor = count - 1;
    if (qtc_mention_prefix(names[t->mention_cursor], prefix, sizeof(prefix)) != 0 ||
        t->mention_start + strlen(prefix) >= sizeof(t->input)) return;
    strcpy(t->input + t->mention_start, prefix);
    t->input_len = strlen(t->input); t->mention_active = false; t->dirty = true;
}

static size_t person_list(tui_ctx *t, int *indices, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < t->state.contact_count && n < max; i++) {
        qtc_contact *c = &t->state.contacts[i];
        if (c->node_type == QTC_NODE_PERSON && qtc_search_match(t->invite_search, contact_name(c))) indices[n++] = (int)i;
    }
    return n;
}
static bool invite_selected(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->invite_count; i++) {
        if (strcmp(t->invite_ids[i], id) == 0) return true;
    }
    return false;
}
static void toggle_invite(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->invite_count; i++) if (strcmp(t->invite_ids[i], id) == 0) {
        for (size_t j = i + 1; j < t->invite_count; j++) qtc_strlcpy(t->invite_ids[j - 1], t->invite_ids[j], QTC_MAX_ID);
        t->invite_count--; t->dirty = true; return;
    }
    if (t->invite_count < QTC_ARRAY_LEN(t->invite_ids)) qtc_strlcpy(t->invite_ids[t->invite_count++], id, QTC_MAX_ID);
    t->dirty = true;
}
static int invite_cursor_pos(tui_ctx *t, int *indices, size_t count) {
    for (size_t i = 0; i < count; i++) if (strcmp(t->state.contacts[indices[i]].id, t->invite_cursor) == 0) return (int)i;
    if (count) qtc_strlcpy(t->invite_cursor, t->state.contacts[indices[0]].id, sizeof(t->invite_cursor));
    return count ? 0 : -1;
}
static void move_invite_cursor(tui_ctx *t, int delta) {
    int idx[QTC_MAX_CONTACTS]; size_t n = person_list(t, idx, QTC_ARRAY_LEN(idx)); int pos = invite_cursor_pos(t, idx, n);
    if (pos < 0) return;
    pos += delta;
    if (pos < 0) pos = 0;
    if ((size_t)pos >= n) pos = (int)n - 1;
    qtc_strlcpy(t->invite_cursor, t->state.contacts[idx[pos]].id, sizeof(t->invite_cursor)); t->dirty = true;
}

static void send_invites(tui_ctx *t) {
    for (size_t i = 0; i < t->invite_count; i++) {
        qtc_ipc_channel_invite_payload p = {.channel_index = t->selected_channel};
        qtc_strlcpy(p.contact_id, t->invite_ids[i], sizeof(p.contact_id));
        (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_INVITE, &p, sizeof(p));
    }
    snprintf(t->status, sizeof(t->status), "Invitation queued for %zu contact%s", t->invite_count, t->invite_count == 1 ? "" : "s");
    t->mode = MODE_NORMAL; t->invite_count = 0; t->invite_search[0] = 0; t->confirm_action = false; t->dirty = true;
}

static void save_settings(tui_ctx *t) { (void)qtc_ipc_send(t->fd, QTC_IPC_SETTINGS, &t->state.settings, sizeof(t->state.settings)); }

static void cycle_theme(tui_ctx *t, int delta) {
    int theme = theme_index(t);
    t->theme_overridden = false;
    if (theme < 0 || theme >= QTC_THEME_COUNT) theme = QTC_THEME_DEFAULT;
    theme = (theme + delta) % QTC_THEME_COUNT;
    if (theme < 0) theme += QTC_THEME_COUNT;
    t->state.settings.theme = theme;
    save_settings(t);
    t->dirty = true;
}

static void apply_preset(tui_ctx *t) {
    if (t->preset_cursor < 0 || (size_t)t->preset_cursor >= QTC_ARRAY_LEN(RADIO_PRESETS)) return;
    const radio_preset *preset = &RADIO_PRESETS[t->preset_cursor];
    qtc_ipc_radio_preset_payload p = {0};
    qtc_strlcpy(p.name, preset->name, sizeof(p.name));
    p.freq_mhz = preset->freq_mhz;
    p.bw_khz = preset->bw_khz;
    p.sf = preset->sf;
    p.cr = preset->cr;
    p.repeat_mode = false;
    (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_PRESET, &p, sizeof(p));
    snprintf(t->status, sizeof(t->status), "%s preset queued", preset->name);
}

/* Positions and viewport offsets refer only to the filtered infrastructure list. */
static size_t node_rows(const tui_ctx *t) {
    return t->height > 9 ? (size_t)(t->height - 9) : 1;
}

static size_t node_selection(tui_ctx *t, int *idx) {
    size_t n = 0;
    for (size_t i = 0; i < t->state.contact_count; i++)
        if (t->state.contacts[i].node_type != QTC_NODE_PERSON) idx[n++] = (int)i;
    if (!n) {
        t->selected_node[0] = 0;
        t->node_position = t->node_scroll = 0;
        return 0;
    }
    size_t pos = 0;
    while (pos < n && strcmp(t->state.contacts[idx[pos]].id, t->selected_node)) pos++;
    if (pos == n) pos = t->node_position < n ? t->node_position : n - 1;
    t->node_position = pos;
    qtc_strlcpy(t->selected_node, t->state.contacts[idx[pos]].id, sizeof(t->selected_node));
    qtc_roster_clamp(pos, n, node_rows(t), &t->node_scroll);
    return n;
}

static void move_nodes(tui_ctx *t, int delta) {
    int idx[QTC_MAX_CONTACTS];
    size_t n = node_selection(t, idx);
    if (!n) return;
    int next = (int)t->node_position + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= n) next = (int)n - 1;
    t->node_position = (size_t)next;
    qtc_strlcpy(t->selected_node, t->state.contacts[idx[next]].id, sizeof(t->selected_node));
    qtc_roster_clamp(t->node_position, n, node_rows(t), &t->node_scroll);
    t->dirty = true;
}

static void scroll_node_detail(tui_ctx *t, int delta) {
    if (delta < 0) {
        size_t step = (size_t)-delta;
        t->node_detail_scroll = t->node_detail_scroll > step ? t->node_detail_scroll - step : 0;
    } else t->node_detail_scroll += (size_t)delta;
    t->dirty = true;
}

static void handle_enter(tui_ctx *t) {
    if (t->mode == MODE_SEARCH) { t->mode = MODE_NORMAL; t->dirty = true; return; }
    if (t->mode == MODE_COMPOSE) { send_composed(t); return; }
    if (t->mode == MODE_CREATE_CHANNEL) {
        qtc_trim(t->input); if (t->input[0]) { qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.name, t->input, sizeof(p.name));
            (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_CREATE, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_JOIN_CHANNEL) {
        qtc_trim(t->input); if (t->input[0]) { qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.uri, t->input, sizeof(p.uri));
            (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_JOIN, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_ALIAS) {
        qtc_contact *ct = find_contact(t, t->selected_key);
        if (ct != NULL) {
            qtc_trim(t->input);
            qtc_ipc_contact_text_payload p = {0};
            qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id));
            qtc_strlcpy(p.value, t->input, sizeof(p.value));
            (void)qtc_ipc_send(t->fd, QTC_IPC_SET_ALIAS, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_FAVORITE_GROUP) {
        qtc_contact *ct = find_contact(t, t->selected_key);
        if (ct != NULL) {
            qtc_trim(t->input);
            qtc_ipc_favorite_payload p = {.favorite = true};
            qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id));
            qtc_strlcpy(p.group, t->input[0] ? t->input : "Favorites", sizeof(p.group));
            (void)qtc_ipc_send(t->fd, QTC_IPC_SET_FAVORITE, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_DEVICE_NAME) {
        qtc_trim(t->input);
        if (t->input[0]) {
            qtc_ipc_device_action_payload p = {0};
            qtc_strlcpy(p.text, t->input, sizeof(p.text));
            (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_NAME, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_TX_POWER) {
        qtc_trim(t->input);
        char *end = NULL;
        long power = strtol(t->input, &end, 10);
        if (t->input[0] && end != NULL && *end == 0 && power >= -20 && power <= 30) {
            qtc_ipc_device_action_payload p = {.value = (int)power};
            (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_TX_POWER, &p, sizeof(p));
        } else {
            qtc_strlcpy(t->status, "TX power must be a whole number in dBm", sizeof(t->status));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_THEME_PICKER) {
        t->theme_overridden = false;
        t->state.settings.theme = t->theme_cursor;
        save_settings(t);
        t->mode = MODE_NORMAL;
        snprintf(t->status, sizeof(t->status), "Theme changed to %s", theme_name(theme_index(t)));
        t->dirty = true;
        return;
    }
    if (t->mode == MODE_PRESET_PICKER) {
        apply_preset(t);
        t->mode = MODE_NORMAL;
        t->dirty = true;
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        if (t->invite_count > 0) { t->mode = MODE_INVITE_REVIEW; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        if (t->confirm_action) send_invites(t); else { t->mode = MODE_INVITE_PICKER; t->dirty = true; }
        return;
    }
    if (t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        if (t->confirm_action) { qtc_ipc_channel_action_payload p = {.channel_index = t->selected_channel};
            (void)qtc_ipc_send(t->fd, t->mode == MODE_ROTATE_CONFIRM ? QTC_IPC_CHANNEL_ROTATE : QTC_IPC_CHANNEL_LEAVE, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return;
    }
    if (t->mode == MODE_INCOMING_INVITE) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        int64_t id = t->incoming_invite_id;
        (void)qtc_ipc_send(t->fd, t->confirm_action ? QTC_IPC_INVITE_ACCEPT : QTC_IPC_INVITE_IGNORE, &id, sizeof(id));
        t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return;
    }
    if (t->view == VIEW_NODES) {
        int idx[QTC_MAX_CONTACTS];
        if (t->mode == MODE_NORMAL && node_selection(t, idx)) {
            t->mode = MODE_NODE_DETAIL; t->node_detail_scroll = 0; t->dirty = true;
        }
        return;
    }
    if (t->view == VIEW_MESSAGES) {
        if (!open_selected(t)) return;
        if (t->open_key[0]) start_input(t, MODE_COMPOSE);
    }
    else if (t->view == VIEW_CHANNELS && t->selected_channel >= 0) {
        t->selected_kind = 1; snprintf(t->selected_key, sizeof(t->selected_key), "%d", t->selected_channel);
        set_view(t, VIEW_MESSAGES);
        if (open_selected(t)) start_input(t, MODE_COMPOSE);
    }
}

static void escape_mode(tui_ctx *t) {
    if (t->mode == MODE_COMPOSE) {
        cancel_mention(t);
        qtc_strlcpy(t->draft, t->replying ? t->reply_backup : t->input, sizeof(t->draft));
        t->replying = false; t->mention_active = false;
    }
    if (t->mode == MODE_SEARCH) t->search[0] = 0;
    if (t->mode != MODE_NORMAL) { t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return; }
    if (t->view != VIEW_MESSAGES) set_view(t, VIEW_MESSAGES);
}

static void move_channels(tui_ctx *t, int delta) {
    int slots[QTC_MAX_CHANNELS]; size_t n = 0;
    for (size_t i = 0; i < t->state.channel_count; i++) if (t->state.channels[i].configured) slots[n++] = t->state.channels[i].index;
    if (!n) { t->selected_channel = -1; return; }
    size_t pos = 0; while (pos < n && slots[pos] != t->selected_channel) pos++;
    if (pos == n) pos = 0;
    int next = (int)pos + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= n) next = (int)n - 1;
    t->selected_channel = slots[next]; t->dirty = true;
}

static void first_pending_invite(tui_ctx *t) {
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].status == QTC_INVITE_PENDING) {
        t->incoming_invite_id = t->state.invitations[i].id; t->mode = MODE_INCOMING_INVITE; t->confirm_action = false;
        t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; return;
    }
    qtc_strlcpy(t->status, "No pending private-channel invitations", sizeof(t->status)); t->dirty = true;
}

/* Help never consumes printable input from an editor or invite search. */
static bool can_open_help(const tui_ctx *t) {
    return t->mode == MODE_NORMAL || t->mode == MODE_MESSAGE_SELECT ||
           t->mode == MODE_NODE_DETAIL;
}

static void normal_key(tui_ctx *t, unsigned char c) {
    if (c == 3) { leave_conversation(t); t->running = false; return; }
    if (c == 17) {
        int64_t now = qtc_now_millis();
        if (now - t->last_ctrl_q < 2000) { (void)qtc_ipc_send(t->fd, QTC_IPC_SHUTDOWN, NULL, 0); t->running = false; }
        else { t->last_ctrl_q = now; qtc_strlcpy(t->status, "Press Ctrl+Q again to stop the background core", sizeof(t->status)); t->dirty = true; }
        return;
    }
    if (t->help_open) {
        if (c == '?' || c == 27) t->help_open = false;
        else if (c == 'j') t->help_scroll++;
        else if (c == 'k' && t->help_scroll) t->help_scroll--;
        t->dirty = true;
        return;
    }
    if (c == '?' && can_open_help(t)) {
        t->help_open = true; t->help_scroll = 0; t->dirty = true;
        return;
    }
    if (t->mode == MODE_NODE_DETAIL) {
        if (c == 'j') scroll_node_detail(t, 1);
        else if (c == 'k') scroll_node_detail(t, -1);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (t->mode == MODE_COMPOSE && t->mention_active) {
        if (c == 27) { cancel_mention(t); return; }
        if (c == '\t') return;
        if (c == '\r' || c == '\n') { accept_mention(t); return; }
    }
    if (t->view == VIEW_MESSAGES && t->open_key[0] && c == '\t' &&
        (t->mode == MODE_NORMAL || t->mode == MODE_COMPOSE || t->mode == MODE_MESSAGE_SELECT)) {
        save_composer(t);
        t->mode = t->mode == MODE_MESSAGE_SELECT ? MODE_NORMAL : MODE_MESSAGE_SELECT;
        t->dirty = true; return;
    }
    if (t->mode == MODE_MESSAGE_SELECT) {
        if (c == 'j') move_message(t, 1);
        else if (c == 'k') move_message(t, -1);
        else if (c == 'r') reply_message(t);
        else if (c == 'm') start_input(t, MODE_COMPOSE);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (t->mode == MODE_SEARCH || t->mode == MODE_COMPOSE ||
        t->mode == MODE_CREATE_CHANNEL || t->mode == MODE_JOIN_CHANNEL ||
        t->mode == MODE_ALIAS || t->mode == MODE_FAVORITE_GROUP ||
        t->mode == MODE_DEVICE_NAME || t->mode == MODE_TX_POWER) {
        bool trigger = t->mode == MODE_COMPOSE && !t->mention_active && c == '@' &&
                       (!t->input_len || t->input[t->input_len - 1] == ' ');
        size_t at = t->input_len;
        if (c == 127 || c == 8) backspace_input(t); else if (c == '\r' || c == '\n') handle_enter(t); else if (c == 27) escape_mode(t); else if (c >= 32) append_input(t, (char)c);
        if (trigger && t->input_len > at) { t->mention_active = true; t->mention_start = at; }
        if (t->mention_active) {
            t->mention_cursor = 0;
            if (t->input_len <= t->mention_start || c == '[') t->mention_active = false;
        }
        if (t->mode == MODE_SEARCH) qtc_strlcpy(t->search, t->input, sizeof(t->search));
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        else if (c == ' ') { if (t->invite_cursor[0]) toggle_invite(t, t->invite_cursor); }
        else if (c == 127 || c == 8) { size_t n = strlen(t->invite_search); if (n) t->invite_search[utf8_previous_boundary(t->invite_search, n)] = 0; t->dirty = true; }
        else if (c >= 32) { size_t n = strlen(t->invite_search); if (n + 1 < sizeof(t->invite_search)) { t->invite_search[n] = (char)c; t->invite_search[n + 1] = 0; t->dirty = true; } }
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW || t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM || t->mode == MODE_INCOMING_INVITE) {
        if (c == '\t' || c == 'h' || c == 'l') { t->confirm_action = !t->confirm_action; t->dirty = true; }
        else if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (t->mode == MODE_THEME_PICKER || t->mode == MODE_PRESET_PICKER) {
        int max = t->mode == MODE_THEME_PICKER ? QTC_THEME_COUNT : (int)QTC_ARRAY_LEN(RADIO_PRESETS);
        int *cursor = t->mode == MODE_THEME_PICKER ? &t->theme_cursor : &t->preset_cursor;
        if (c == 'j') { if (*cursor + 1 < max) (*cursor)++; t->dirty = true; }
        else if (c == 'k') { if (*cursor > 0) (*cursor)--; t->dirty = true; }
        else if (c >= '1' && c < '1' + max) { *cursor = c - '1'; handle_enter(t); }
        else if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (c == 27) { escape_mode(t); return; }
    if (c == '\r' || c == '\n') { handle_enter(t); return; }
    if (t->view == VIEW_MESSAGES) {
        if (c == 'j') move_menu(t, 1); else if (c == 'k') move_menu(t, -1);
        else if (c == '/') { t->mode = MODE_SEARCH; qtc_strlcpy(t->input, t->search, sizeof(t->input)); t->input_len = strlen(t->input); t->dirty = true; }
        else if (c == 'm' && t->open_key[0]) start_input(t, MODE_COMPOSE);
        else if (c == 'f' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key); if (ct) { qtc_ipc_favorite_payload p = {.favorite = !ct->favorite};
                qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id)); qtc_strlcpy(p.group, ct->favorite_group[0] ? ct->favorite_group : "Favorites", sizeof(p.group));
                (void)qtc_ipc_send(t->fd, QTC_IPC_SET_FAVORITE, &p, sizeof(p));
                snprintf(t->status, sizeof(t->status), "%s %s Favorites", contact_name(ct), p.favorite ? "added to" : "removed from");
                t->dirty = true; }
        }
        else if (c == 'e' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_ALIAS);
                qtc_strlcpy(t->input, ct->alias, sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
        else if (c == 'g' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_FAVORITE_GROUP);
                qtc_strlcpy(t->input, ct->favorite_group[0] ? ct->favorite_group : "Favorites", sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
        else if (c == 's') set_view(t, VIEW_SETTINGS);
    } else if (t->view == VIEW_CHANNELS) {
        if (c == 'n') move_channels(t, 1); else if (c == 'p') move_channels(t, -1);
        else if (c == 'c') start_input(t, MODE_CREATE_CHANNEL);
        else if (c == 'j') start_input(t, MODE_JOIN_CHANNEL);
        else if (c == 'i' && t->selected_channel >= 0) { t->mode = MODE_INVITE_PICKER; t->invite_count = 0; t->invite_search[0] = 0; t->invite_cursor[0] = 0; t->dirty = true; }
        else if (c == 'r' && t->selected_channel >= 0) { t->mode = MODE_ROTATE_CONFIRM; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        else if (c == 'd' && t->selected_channel >= 0) { t->mode = MODE_LEAVE_CONFIRM; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        else if (c == 'v') first_pending_invite(t);
    } else if (t->view == VIEW_NODES) {
        if (c == 'j') move_nodes(t, 1); else if (c == 'k') move_nodes(t, -1);
    } else if (t->view == VIEW_SETTINGS) {
        if (c == '1') { t->state.settings.desktop_notifications = !t->state.settings.desktop_notifications; save_settings(t); t->dirty = true; }
        else if (c == '2') { t->state.settings.sound_enabled = !t->state.settings.sound_enabled; save_settings(t); t->dirty = true; }
        else if (c == '3') { t->state.settings.notify_direct = !t->state.settings.notify_direct; save_settings(t); t->dirty = true; }
        else if (c == '4') { t->state.settings.notify_channel = !t->state.settings.notify_channel; save_settings(t); t->dirty = true; }
        else if (c == '5') cycle_theme(t, 1);
        else if (c == 't') { t->theme_cursor = theme_index(t); t->mode = MODE_THEME_PICKER; t->dirty = true; }
        else if (c == '6') { t->state.settings.banner_enabled = !t->state.settings.banner_enabled; save_settings(t); t->dirty = true; }
        else if (c == '7') { t->state.settings.suppress_open_conversation = !t->state.settings.suppress_open_conversation; save_settings(t); t->dirty = true; }
        else if (c == '8') { t->state.settings.retry_unconfirmed = !t->state.settings.retry_unconfirmed; save_settings(t); t->dirty = true; }
        else if (c == '9') { t->state.settings.reset_stale_route = !t->state.settings.reset_stale_route; save_settings(t); t->dirty = true; }
        else if (c == '0') { t->state.settings.show_signal = !t->state.settings.show_signal; save_settings(t); t->dirty = true; }
        else if (c == ',' && t->state.settings.stored_poll_seconds > 1) { t->state.settings.stored_poll_seconds--; save_settings(t); t->dirty = true; }
        else if (c == '.' && t->state.settings.stored_poll_seconds < 3600) { t->state.settings.stored_poll_seconds++; save_settings(t); t->dirty = true; }
        else if (c == '[' && t->state.settings.max_direct_attempts > 1) { t->state.settings.max_direct_attempts--; save_settings(t); t->dirty = true; }
        else if (c == ']' && t->state.settings.max_direct_attempts < 4) { t->state.settings.max_direct_attempts++; save_settings(t); t->dirty = true; }
        else if (c == 'd') {
            start_input(t, MODE_DEVICE_NAME);
            qtc_strlcpy(t->input, t->state.radio_name, sizeof(t->input));
            t->input_len = strlen(t->input);
        }
        else if (c == 'p') {
            start_input(t, MODE_TX_POWER);
            snprintf(t->input, sizeof(t->input), "%d", t->state.radio_tx_power);
            t->input_len = strlen(t->input);
        }
        else if (c == 'z' || c == 'x') {
            qtc_ipc_device_action_payload p = {.flag = c == 'x'};
            const char *sending = p.flag ?
                                  "Sending flood advertisement — waiting for radio confirmation..." :
                                  "Sending 0-hop advertisement — waiting for radio confirmation...";
            if (qtc_ipc_send(t->fd, QTC_IPC_DEVICE_ADVERTISE,
                             &p, sizeof(p)) == 0) {
                qtc_strlcpy(t->status, sending, sizeof(t->status));
                t->advert_feedback_pending = true;
                set_action_feedback(t, sending, FEEDBACK_INFO, 8000);
            } else {
                const char *failed = "Could not send advertisement request to the " QTC_DISPLAY_NAME " core";
                qtc_strlcpy(t->status, failed, sizeof(t->status));
                t->advert_feedback_pending = false;
                set_action_feedback(t, failed, FEEDBACK_ERROR, 7000);
            }
        }
        else if (c == 'c') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_COPY_CARD, NULL, 0);
        else if (c == 'o') { t->preset_cursor = 0; t->mode = MODE_PRESET_PICKER; t->dirty = true; }
        else if (c == 'y') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SYNC_MESSAGES, NULL, 0);
        else if (c == 'r') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_RECONNECT, NULL, 0);
        else if (c == 'n') {
            int rc = qtc_notify_desktop(QTC_DISPLAY_NAME " notification test", "Desktop notifications are working");
            qtc_strlcpy(t->status, rc == 0 ? "Desktop notification test sent" : "No desktop notification helper is available", sizeof(t->status));
            t->dirty = true;
        } else if (c == 'a') {
            int rc = qtc_notify_sound();
            qtc_strlcpy(t->status, rc == 0 ? "Notification sound test started" : "No supported sound player or sound file is available", sizeof(t->status));
            t->dirty = true;
        }
    }
}

static void special_key(tui_ctx *t, const char *seq) {
    if (t->help_open && strcmp(seq, "\x1b[19~") != 0) {
        if (strcmp(seq, "\x1b[B") == 0) t->help_scroll++;
        else if (strcmp(seq, "\x1b[A") == 0 && t->help_scroll) t->help_scroll--;
        else if (strcmp(seq, "\x1b[6~") == 0) t->help_scroll += 5;
        else if (strcmp(seq, "\x1b[5~") == 0)
            t->help_scroll = t->help_scroll > 5 ? t->help_scroll - 5 : 0;
        t->dirty = true;
        return;
    }
    if (t->mode == MODE_NODE_DETAIL) {
        if (strcmp(seq, "\x1b[A") == 0) { scroll_node_detail(t, -1); return; }
        if (strcmp(seq, "\x1b[B") == 0) { scroll_node_detail(t, 1); return; }
        if (strcmp(seq, "\x1b[5~") == 0) { scroll_node_detail(t, -(int)node_rows(t)); return; }
        if (strcmp(seq, "\x1b[6~") == 0) { scroll_node_detail(t, (int)node_rows(t)); return; }
        if (strcmp(seq, "\x1b[18~") == 0) { escape_mode(t); return; }
    }
    if (t->mode == MODE_COMPOSE && t->mention_active) {
        if (strcmp(seq, "\x1b[A") == 0 || strcmp(seq, "\x1b[B") == 0) {
            char names[MENTION_CANDIDATES][QTC_MAX_NAME];
            size_t count = mention_candidates(t, names);
            if (strcmp(seq, "\x1b[A") == 0 && t->mention_cursor) t->mention_cursor--;
            else if (strcmp(seq, "\x1b[B") == 0 && t->mention_cursor + 1 < count) t->mention_cursor++;
            t->dirty = true; return;
        }
        cancel_mention(t);
    }
    if (t->mode == MODE_MESSAGE_SELECT) {
        if (strcmp(seq, "\x1b[A") == 0) { move_message(t, -1); return; }
        if (strcmp(seq, "\x1b[B") == 0) { move_message(t, 1); return; }
        if (strcmp(seq, "\x1b[5~") == 0) { move_message(t, -5); return; }
        if (strcmp(seq, "\x1b[6~") == 0) { move_message(t, 5); return; }
    }
    if (strcmp(seq, "\x1b[A") == 0) {
        if (t->mode == MODE_INVITE_PICKER) move_invite_cursor(t, -1);
        else if (t->mode == MODE_THEME_PICKER) { if (t->theme_cursor > 0) t->theme_cursor--; t->dirty = true; }
        else if (t->mode == MODE_PRESET_PICKER) { if (t->preset_cursor > 0) t->preset_cursor--; t->dirty = true; }
        else if (t->view == VIEW_MESSAGES) move_menu(t, -1);
        else if (t->view == VIEW_CHANNELS) move_channels(t, -1);
        else if (t->view == VIEW_NODES) move_nodes(t, -1);
    } else if (strcmp(seq, "\x1b[B") == 0) {
        if (t->mode == MODE_INVITE_PICKER) move_invite_cursor(t, 1);
        else if (t->mode == MODE_THEME_PICKER) { if (t->theme_cursor + 1 < QTC_THEME_COUNT) t->theme_cursor++; t->dirty = true; }
        else if (t->mode == MODE_PRESET_PICKER) { if ((size_t)(t->preset_cursor + 1) < QTC_ARRAY_LEN(RADIO_PRESETS)) t->preset_cursor++; t->dirty = true; }
        else if (t->view == VIEW_MESSAGES) move_menu(t, 1);
        else if (t->view == VIEW_CHANNELS) move_channels(t, 1);
        else if (t->view == VIEW_NODES) move_nodes(t, 1);
    } else if (strcmp(seq, "\x1b[C") == 0 || strcmp(seq, "\x1b[D") == 0) {
        if (t->mode == MODE_INVITE_REVIEW || t->mode == MODE_ROTATE_CONFIRM ||
            t->mode == MODE_LEAVE_CONFIRM || t->mode == MODE_INCOMING_INVITE) {
            t->confirm_action = !t->confirm_action;
            t->dirty = true;
        }
    } else if (strcmp(seq, "\x1b[5~") == 0) {
        if (t->view == VIEW_MESSAGES && t->open_key[0]) {
            t->history_scroll += (size_t)(t->height > 12 ? (t->height - 8) / 2 : 4);
            t->dirty = true;
        }
    } else if (strcmp(seq, "\x1b[6~") == 0) {
        size_t step = (size_t)(t->height > 12 ? (t->height - 8) / 2 : 4);
        t->history_scroll = t->history_scroll > step ? t->history_scroll - step : 0;
        t->dirty = true;
    } else if (strcmp(seq, "\x1b[12~") == 0 || strcmp(seq, "\x1bOQ") == 0) {
        if (t->view == VIEW_MESSAGES && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_ALIAS);
                qtc_strlcpy(t->input, ct->alias, sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
    } else if (strcmp(seq, "\x1b[14~") == 0 || strcmp(seq, "\x1bOS") == 0) {
        set_view(t, t->view == VIEW_SETTINGS ? VIEW_MESSAGES : VIEW_SETTINGS);
    } else if (strcmp(seq, "\x1b[15~") == 0) {
        (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_RECONNECT, NULL, 0);
    } else if (strcmp(seq, "\x1b[17~") == 0) {
        set_view(t, t->view == VIEW_CHANNELS ? VIEW_MESSAGES : VIEW_CHANNELS);
    } else if (strcmp(seq, "\x1b[18~") == 0) {
        set_view(t, t->view == VIEW_NODES ? VIEW_MESSAGES : VIEW_NODES);
    } else if (strcmp(seq, "\x1b[19~") == 0) {
        leave_conversation(t);
        t->running = false;
    }
}

static void process_input(tui_ctx *t, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (data[i] == 27) {
            size_t remain = len - i, n = remain < sizeof(t->escape_buf) - 1 ? remain : sizeof(t->escape_buf) - 1;
            memcpy(t->escape_buf, data + i, n); t->escape_buf[n] = 0;
            const char *known[] = {"\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[5~", "\x1b[6~", "\x1b[12~", "\x1bOQ", "\x1b[14~", "\x1bOS", "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~"};
            bool matched = false;
            for (size_t k = 0; k < QTC_ARRAY_LEN(known); k++) if (strncmp(t->escape_buf, known[k], strlen(known[k])) == 0) {
                special_key(t, known[k]); i += strlen(known[k]); matched = true; break;
            }
            if (!matched) { normal_key(t, 27); i++; }
        } else { normal_key(t, data[i++]); }
    }
}

/* Styled, UTF-8-aware terminal framebuffer. Frames use absolute cursor addressing,
 * so rendering remains correct even when the terminal is in raw mode with OPOST off. */
typedef enum {
    UI_NORMAL = 0,
    UI_HEADER,
    UI_BORDER,
    UI_SECTION,
    UI_MUTED,
    UI_SELECTED,
    UI_UNREAD,
    UI_OUTGOING,
    UI_INCOMING,
    UI_STATUS,
    UI_INPUT,
    UI_ACCENT,
    UI_DANGER,
    UI_SUCCESS,
    UI_READY,
    UI_RECONNECTING,
    UI_NAVIGATION,
    UI_STYLE_COUNT
} ui_style;

typedef struct {
    const char *name;
    const char *sgr[UI_STYLE_COUNT];
} ui_theme;

/* Semantic SGR palettes; IDs 0..3 retain existing saved preferences. */
static const ui_theme THEMES[QTC_THEME_COUNT] = {
    {.name = "classic", .sgr = {
        [UI_NORMAL] = "\x1b[0;32;40m",
        [UI_HEADER] = "\x1b[1;30;42m",
        [UI_BORDER] = "\x1b[0;32;40m",
        [UI_SECTION] = "\x1b[1;92;40m",
        [UI_MUTED] = "\x1b[2;32;40m",
        [UI_SELECTED] = "\x1b[1;30;102m",
        [UI_UNREAD] = "\x1b[1;97;40m",
        [UI_OUTGOING] = "\x1b[1;92;40m",
        [UI_INCOMING] = "\x1b[0;32;40m",
        [UI_STATUS] = "\x1b[0;30;42m",
        [UI_INPUT] = "\x1b[1;97;40m",
        [UI_ACCENT] = "\x1b[1;92;40m",
        [UI_DANGER] = "\x1b[1;91;40m",
        [UI_SUCCESS] = "\x1b[1;92;40m",
        [UI_READY] = "\x1b[1;92;40m",
        [UI_RECONNECTING] = "\x1b[1;33;40m",
        [UI_NAVIGATION] = "\x1b[1;30;102m",
    }},
    {.name = "amber", .sgr = {
        [UI_NORMAL] = "\x1b[0;33;40m",
        [UI_HEADER] = "\x1b[1;30;43m",
        [UI_BORDER] = "\x1b[0;33;40m",
        [UI_SECTION] = "\x1b[1;93;40m",
        [UI_MUTED] = "\x1b[2;33;40m",
        [UI_SELECTED] = "\x1b[1;30;103m",
        [UI_UNREAD] = "\x1b[1;97;40m",
        [UI_OUTGOING] = "\x1b[1;93;40m",
        [UI_INCOMING] = "\x1b[0;33;40m",
        [UI_STATUS] = "\x1b[0;30;43m",
        [UI_INPUT] = "\x1b[1;97;40m",
        [UI_ACCENT] = "\x1b[1;93;40m",
        [UI_DANGER] = "\x1b[1;91;40m",
        [UI_SUCCESS] = "\x1b[1;93;40m",
        [UI_READY] = "\x1b[1;93;40m",
        [UI_RECONNECTING] = "\x1b[1;33;40m",
        [UI_NAVIGATION] = "\x1b[1;30;103m",
    }},
    {.name = "midnight", .sgr = {
        [UI_NORMAL] = "\x1b[0;97;44m",
        [UI_HEADER] = "\x1b[1;97;45m",
        [UI_BORDER] = "\x1b[0;96;44m",
        [UI_SECTION] = "\x1b[1;96;44m",
        [UI_MUTED] = "\x1b[2;37;44m",
        [UI_SELECTED] = "\x1b[1;30;106m",
        [UI_UNREAD] = "\x1b[1;93;44m",
        [UI_OUTGOING] = "\x1b[1;96;44m",
        [UI_INCOMING] = "\x1b[0;97;44m",
        [UI_STATUS] = "\x1b[1;97;45m",
        [UI_INPUT] = "\x1b[1;93;44m",
        [UI_ACCENT] = "\x1b[1;96;44m",
        [UI_DANGER] = "\x1b[1;91;44m",
        [UI_SUCCESS] = "\x1b[1;92;44m",
        [UI_READY] = "\x1b[1;92;44m",
        [UI_RECONNECTING] = "\x1b[1;33;40m",
        [UI_NAVIGATION] = "\x1b[1;30;106m",
    }},
    {.name = "mono", .sgr = {
        [UI_NORMAL] = "\x1b[0m",
        [UI_HEADER] = "\x1b[7m",
        [UI_BORDER] = "\x1b[2m",
        [UI_SECTION] = "\x1b[1m",
        [UI_MUTED] = "\x1b[2m",
        [UI_SELECTED] = "\x1b[7m",
        [UI_UNREAD] = "\x1b[1m",
        [UI_OUTGOING] = "\x1b[1m",
        [UI_INCOMING] = "\x1b[0m",
        [UI_STATUS] = "\x1b[7m",
        [UI_INPUT] = "\x1b[1m",
        [UI_ACCENT] = "\x1b[1m",
        [UI_DANGER] = "\x1b[1;4m",
        [UI_SUCCESS] = "\x1b[1m",
        [UI_READY] = "\x1b[1m",
        [UI_RECONNECTING] = "\x1b[1;4m",
        [UI_NAVIGATION] = "\x1b[7m",
    }},
    {.name = "signal", .sgr = {
        [UI_NORMAL] = "\x1b[0;37;40m",
        [UI_HEADER] = "\x1b[1;97;44m",
        [UI_BORDER] = "\x1b[0;36;40m",
        [UI_SECTION] = "\x1b[1;96;40m",
        [UI_MUTED] = "\x1b[0;37;40m",
        [UI_SELECTED] = "\x1b[1;30;46m",
        [UI_UNREAD] = "\x1b[1;93;40m",
        [UI_OUTGOING] = "\x1b[0;96;40m",
        [UI_INCOMING] = "\x1b[0;37;40m",
        [UI_STATUS] = "\x1b[0;97;44m",
        [UI_INPUT] = "\x1b[1;97;40m",
        [UI_ACCENT] = "\x1b[1;4;93;40m",
        [UI_DANGER] = "\x1b[1;91;40m",
        [UI_SUCCESS] = "\x1b[1;92;40m",
        [UI_READY] = "\x1b[1;92;40m",
        [UI_RECONNECTING] = "\x1b[1;33;40m",
        [UI_NAVIGATION] = "\x1b[1;30;46m",
    }},
    {.name = "phosphor", .sgr = {
        [UI_NORMAL] = "\x1b[0;32;40m",
        [UI_HEADER] = "\x1b[1;30;42m",
        [UI_BORDER] = "\x1b[0;32;40m",
        [UI_SECTION] = "\x1b[1;92;40m",
        [UI_MUTED] = "\x1b[2;32;40m",
        [UI_SELECTED] = "\x1b[1;30;102m",
        [UI_UNREAD] = "\x1b[1;4;92;40m",
        [UI_OUTGOING] = "\x1b[1;92;40m",
        [UI_INCOMING] = "\x1b[0;32;40m",
        [UI_STATUS] = "\x1b[0;30;42m",
        [UI_INPUT] = "\x1b[1;97;40m",
        [UI_ACCENT] = "\x1b[1;4;92;40m",
        [UI_DANGER] = "\x1b[1;91;40m",
        [UI_SUCCESS] = "\x1b[1;92;40m",
        [UI_READY] = "\x1b[1;92;40m",
        [UI_RECONNECTING] = "\x1b[1;33;40m",
        [UI_NAVIGATION] = "\x1b[1;30;102m",
    }},
    {.name = "high-contrast", .sgr = {
        [UI_NORMAL] = "\x1b[0;97;40m",
        [UI_HEADER] = "\x1b[1;30;107m",
        [UI_BORDER] = "\x1b[0;97;40m",
        [UI_SECTION] = "\x1b[1;97;40m",
        [UI_MUTED] = "\x1b[0;97;40m",
        [UI_SELECTED] = "\x1b[1;7;97;40m",
        [UI_UNREAD] = "\x1b[1;4;97;40m",
        [UI_OUTGOING] = "\x1b[1;97;40m",
        [UI_INCOMING] = "\x1b[0;97;40m",
        [UI_STATUS] = "\x1b[1;30;107m",
        [UI_INPUT] = "\x1b[1;97;40m",
        [UI_ACCENT] = "\x1b[1;4;97;40m",
        [UI_DANGER] = "\x1b[1;7;97;40m",
        [UI_SUCCESS] = "\x1b[1;97;40m",
        [UI_READY] = "\x1b[1;97;40m",
        [UI_RECONNECTING] = "\x1b[1;7;97;40m",
        [UI_NAVIGATION] = "\x1b[1;7;97;40m",
    }},
};

int qtc_tui_theme_index(const char *name) {
    if (name == NULL) return -1;
    for (int i = 0; i < QTC_THEME_COUNT; i++)
        if (strcmp(name, THEMES[i].name) == 0) return i;
    return -1;
}

typedef struct {
    char bytes[12];
    uint8_t len;
    uint8_t width;
    uint8_t continuation;
    uint8_t style;
} screen_cell;

typedef struct {
    int w;
    int h;
    screen_cell *cells;
    int cursor_r;
    int cursor_c;
    bool cursor;
} screen;

static int theme_index(const tui_ctx *t) {
    int index = t->theme_overridden ? t->theme_override : t->state.settings.theme;
    return index >= 0 && index < QTC_THEME_COUNT ? index : QTC_THEME_DEFAULT;
}

static const ui_theme *active_theme(const tui_ctx *t) {
    return &THEMES[theme_index(t)];
}

static const char *theme_name(int index) {
    if (index < 0 || index >= (int)QTC_ARRAY_LEN(THEMES)) index = QTC_THEME_DEFAULT;
    return THEMES[index].name;
}

static void cell_space(screen_cell *cell, ui_style style) {
    memset(cell, 0, sizeof(*cell));
    cell->bytes[0] = ' ';
    cell->len = 1;
    cell->width = 1;
    cell->style = (uint8_t)style;
}

static int screen_init(screen *s, int w, int h) {
    memset(s, 0, sizeof(*s));
    s->w = w;
    s->h = h;
    s->cells = calloc((size_t)w * (size_t)h, sizeof(*s->cells));
    if (s->cells == NULL) return -1;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) cell_space(&s->cells[i], UI_NORMAL);
    s->cursor_r = h;
    s->cursor_c = 1;
    return 0;
}

static void screen_free(screen *s) {
    free(s->cells);
    s->cells = NULL;
}

static screen_cell *screen_at(screen *s, int r, int c) {
    if (r < 0 || r >= s->h || c < 0 || c >= s->w) return NULL;
    return &s->cells[(size_t)r * (size_t)s->w + (size_t)c];
}

static void screen_clear_footprint(screen *s, int r, int c, ui_style style) {
    screen_cell *cell = screen_at(s, r, c);
    if (cell == NULL) return;
    if (cell->continuation && c > 0) {
        screen_cell *lead = screen_at(s, r, c - 1);
        if (lead != NULL && lead->width == 2) cell_space(lead, style);
    } else if (cell->width == 2 && c + 1 < s->w) {
        screen_cell *tail = screen_at(s, r, c + 1);
        if (tail != NULL) cell_space(tail, style);
    }
    cell_space(cell, style);
}

static void screen_fill(screen *s, int r, int left, int width, ui_style style) {
    if (r < 0 || r >= s->h || width <= 0) return;
    if (left < 0) { width += left; left = 0; }
    if (left + width > s->w) width = s->w - left;
    for (int c = left; c < left + width; c++) cell_space(screen_at(s, r, c), style);
}

static size_t decode_char(const char *text, size_t remaining, wchar_t *wc) {
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t n = mbrtowc(wc, text, remaining, &state);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'?';
        return 1;
    }
    return n;
}

static int text_width(const char *text) {
    int width = 0;
    const char *p = text;
    size_t remaining = strlen(text);
    while (remaining > 0) {
        wchar_t wc;
        size_t n = decode_char(p, remaining, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        width += cw;
        p += n;
        remaining -= n;
    }
    return width;
}

static int screen_put_text(screen *s, int r, int c, int max_cells, const char *text, ui_style style) {
    if (r < 0 || r >= s->h || c >= s->w || max_cells <= 0 || text == NULL) return c;
    int start = c;
    int limit = c + max_cells;
    if (limit > s->w - 1) limit = s->w - 1;
    const char *p = text;
    size_t remaining = strlen(text);
    while (remaining > 0 && c < limit) {
        wchar_t wc;
        size_t n = decode_char(p, remaining, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) { wc = L'?'; cw = 1; n = 1; }
        if (cw == 0) {
            int lead_col = c - 1;
            while (lead_col >= start) {
                screen_cell *lead = screen_at(s, r, lead_col);
                if (lead != NULL && !lead->continuation) {
                    if ((size_t)lead->len + n < sizeof(lead->bytes)) {
                        memcpy(lead->bytes + lead->len, p, n);
                        lead->len = (uint8_t)(lead->len + n);
                    }
                    break;
                }
                lead_col--;
            }
            p += n;
            remaining -= n;
            continue;
        }
        if (cw > 2) cw = 1;
        if (c + cw > limit) break;
        screen_clear_footprint(s, r, c, style);
        screen_cell *cell = screen_at(s, r, c);
        if (cell == NULL) break;
        memset(cell, 0, sizeof(*cell));
        if (n >= sizeof(cell->bytes)) { cell->bytes[0] = '?'; cell->len = 1; cell->width = 1; }
        else { memcpy(cell->bytes, p, n); cell->len = (uint8_t)n; cell->width = (uint8_t)cw; }
        cell->style = (uint8_t)style;
        if (cw == 2) {
            screen_cell *tail = screen_at(s, r, c + 1);
            if (tail != NULL) {
                memset(tail, 0, sizeof(*tail));
                tail->continuation = 1;
                tail->style = (uint8_t)style;
            }
        }
        c += cw;
        p += n;
        remaining -= n;
    }
    return c;
}

static int screen_put_fmt(screen *s, int r, int c, int max_cells, ui_style style, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return screen_put_text(s, r, c, max_cells, buf, style);
}

static void screen_hline(screen *s, int r, int left, int width, char ch, ui_style style) {
    if (width <= 0) return;
    char one[2] = {ch, 0};
    for (int c = left; c < left + width && c < s->w - 1; c++) (void)screen_put_text(s, r, c, 1, one, style);
}

static void screen_vline(screen *s, int c, int top, int height, char ch, ui_style style) {
    char one[2] = {ch, 0};
    for (int r = top; r < top + height && r < s->h; r++) (void)screen_put_text(s, r, c, 1, one, style);
}

static void screen_box(screen *s, int top, int left, int height, int width, const char *title) {
    if (width > s->w - 2) width = s->w - 2;
    if (height > s->h - 2) height = s->h - 2;
    if (width < 8 || height < 4) return;
    for (int r = top; r < top + height; r++) screen_fill(s, r, left, width, UI_NORMAL);
    screen_hline(s, top, left, width, '-', UI_BORDER);
    screen_hline(s, top + height - 1, left, width, '-', UI_BORDER);
    screen_vline(s, left, top, height, '|', UI_BORDER);
    screen_vline(s, left + width - 1, top, height, '|', UI_BORDER);
    (void)screen_put_text(s, top, left, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top, left + width - 1, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top + height - 1, left, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top + height - 1, left + width - 1, 1, "+", UI_BORDER);
    if (title != NULL) screen_put_fmt(s, top, left + 2, width - 4, UI_SECTION, " %s ", title);
}

static ui_style roster_style(const qtc_roster_row *row, bool selected) {
    if (selected) return UI_SELECTED;
    if (row->kind == 4) return row->label[0] == ' ' ? UI_MUTED : UI_SECTION;
    if (row->label[0] == '!') return UI_UNREAD;
    return UI_NORMAL;
}

static void render_roster_row(screen *s, int row, int split, const qtc_roster_row *rr, bool selected) {
    ui_style style = roster_style(rr, selected);
    screen_fill(s, row, 0, split, style);
    if (rr->kind == 4) {
        screen_put_text(s, row, 2, split - 4, rr->label, style);
    } else {
        screen_put_text(s, row, 1, 1, selected ? ">" : " ", style);
        screen_put_text(s, row, 3, split - 5, rr->label, style);
    }
}

static const char *message_status_label(qtc_message_status status) {
    switch (status) {
        case QTC_MSG_QUEUED: return "queued";
        case QTC_MSG_SENDING: return "sending";
        case QTC_MSG_SENT: return "sent";
        case QTC_MSG_DELIVERED: return "delivered";
        case QTC_MSG_UNCONFIRMED: return "unconfirmed";
        case QTC_MSG_FAILED: return "failed";
        default: return "";
    }
}

typedef struct {
    const qtc_message *first;
    char text[QTC_MAX_TEXT];
    qtc_message_status status;
    int part_total;
    int part_count;
} logical_message_view;

static const char *message_logical_key(const qtc_message *m) {
    return m->logical_key[0] ? m->logical_key : m->message_key;
}

static size_t collect_logical_messages(tui_ctx *t, size_t *indices, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < t->state.message_count && count < max; i++) {
        const qtc_message *m = &t->state.messages[i];
        if (m->conversation_kind != t->open_kind ||
            strcmp(m->conversation_key, t->open_key) != 0) continue;
        const char *key = message_logical_key(m);
        bool seen = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(message_logical_key(&t->state.messages[indices[j]]), key) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) indices[count++] = i;
    }
    return count;
}

static void move_message(tui_ctx *t, int delta) {
    size_t indices[QTC_MAX_MESSAGES];
    size_t count = collect_logical_messages(t, indices, QTC_ARRAY_LEN(indices));
    if (!count) { t->selected_message[0] = 0; t->dirty = true; return; }
    size_t pos = 0;
    while (pos < count && strcmp(message_logical_key(&t->state.messages[indices[pos]]), t->selected_message)) pos++;
    if (pos == count) pos = count - 1;
    int next = (int)pos + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= count) next = (int)count - 1;
    qtc_strlcpy(t->selected_message, message_logical_key(&t->state.messages[indices[next]]), sizeof(t->selected_message));
    t->dirty = true;
}

static void load_logical_message(tui_ctx *t, size_t index, logical_message_view *view);

/* A quote is single-line ordinary text. Invalid UTF-8 becomes '?' and control
 * characters cannot reach the composer. Keep whitespace between words only. */
static void reply_quote(const char *body, size_t budget, char *out) {
    char normalized[QTC_MAX_TEXT];
    size_t used = 0, remaining = strlen(body);
    bool space = false;
    while (remaining) {
        wchar_t wc;
        mbstate_t state = {0};
        size_t n = mbrtowc(&wc, body, remaining, &state);
        bool invalid = n == (size_t)-1 || n == (size_t)-2;
        if (!invalid && (wc > 0x10ffff || (wc >= 0xd800 && wc <= 0xdfff))) invalid = true;
        if (invalid) { n = 1; wc = L'?'; }
        if (iswspace(wc)) space = used > 0;
        else if (!iswcntrl(wc)) {
            if (used + (space ? 1U : 0U) + n >= sizeof(normalized)) break;
            if (space) normalized[used++] = ' ';
            if (invalid) normalized[used++] = '?';
            else { memcpy(normalized + used, body, n); used += n; }
            space = false;
        }
        body += n; remaining -= n;
    }
    normalized[used] = 0;
    if (used > budget) {
        size_t cut = qtc_utf8_chunk_length(normalized, budget - 3);
        while (cut && normalized[cut - 1] == ' ') cut--;
        memcpy(out, normalized, cut);
        memcpy(out + cut, "...", 4);
    } else memcpy(out, normalized, used + 1);
}

static void reply_message(tui_ctx *t) {
    size_t indices[QTC_MAX_MESSAGES];
    size_t count = collect_logical_messages(t, indices, QTC_ARRAY_LEN(indices));
    char sender[QTC_MAX_NAME], prefix[QTC_MAX_NAME + 4];
    for (size_t i = 0; i < count; i++) {
        const qtc_message *m = &t->state.messages[indices[i]];
        if (strcmp(message_logical_key(m), t->selected_message)) continue;
        if (!display_sender(t, m, sender, sizeof(sender)) ||
            qtc_mention_prefix(sender, prefix, sizeof(prefix)) != 0) break;
        logical_message_view view;
        load_logical_message(t, indices[i], &view);
        const char *body = view.text;
        char parsed_sender[QTC_MAX_NAME];
        if (m->conversation_kind == QTC_CONV_CHANNEL &&
            qtc_channel_sender(body, parsed_sender, sizeof(parsed_sender)) &&
            strcmp(parsed_sender, sender) == 0) body += strlen(parsed_sender) + 2;
        size_t prefix_len = strlen(prefix), draft_len = strlen(t->draft);
        size_t single_limit = m->conversation_kind == QTC_CONV_CONTACT ?
                             QTC_DIRECT_RADIO_TEXT_MAX : QTC_CHANNEL_RADIO_TEXT_MAX;
        /* Reserve half the bytes after the mention and "> " / " | " for
         * new response text. Existing drafts may still use normal multipart. */
        size_t quote_budget = (single_limit - prefix_len - 5) / 2;
        char quote[QTC_MAX_TEXT];
        reply_quote(body, quote_budget, quote);
        size_t seed_len = prefix_len + 5 + strlen(quote);
        if (seed_len + draft_len >= sizeof(t->input)) {
            qtc_strlcpy(t->status, "Not enough room for quoted reply; draft unchanged", sizeof(t->status));
            t->dirty = true; return;
        }
        qtc_strlcpy(t->reply_backup, t->draft, sizeof(t->reply_backup));
        start_input(t, MODE_COMPOSE);
        /* Build outside tui_ctx so fortified copies cannot alias its fields. */
        char reply[QTC_MAX_TEXT];
        memcpy(reply, prefix, prefix_len);
        memcpy(reply + prefix_len, "> ", 2);
        memcpy(reply + prefix_len + 2, quote, strlen(quote));
        memcpy(reply + seed_len - 3, " | ", 3);
        memcpy(reply + seed_len, t->draft, draft_len + 1);
        memcpy(t->input, reply, seed_len + draft_len + 1);
        t->input_len = seed_len + draft_len; t->replying = true;
        return;
    }
    qtc_strlcpy(t->status, "Reply unavailable: select an incoming message with a known sender", sizeof(t->status));
    t->dirty = true;
}

static void load_logical_message(tui_ctx *t, size_t index, logical_message_view *view) {
    memset(view, 0, sizeof(*view));
    view->first = &t->state.messages[index];
    view->status = view->first->status;
    view->part_total = view->first->part_total > 0 ? view->first->part_total : 1;
    view->part_count = 1;
    if (qtc_message_assemble(t->state.messages, t->state.message_count,
                             message_logical_key(view->first), view->text,
                             sizeof(view->text), &view->part_total,
                             &view->part_count, &view->status) < 0) {
        qtc_strlcpy(view->text, view->first->text, sizeof(view->text));
    }
}

static size_t wrapped_segment(const char *text, size_t offset, int max_cells,
                              char *out, size_t out_len, size_t *next_offset) {
    size_t length = strlen(text);
    while (offset < length && text[offset] == ' ') offset++;
    if (offset >= length) {
        if (out_len) out[0] = 0;
        *next_offset = length;
        return 0;
    }
    size_t pos = offset;
    size_t last_space = SIZE_MAX;
    int cells = 0;
    while (pos < length) {
        if (text[pos] == '\n') break;
        wchar_t wc;
        size_t n = decode_char(text + pos, length - pos, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        if (cells + cw > max_cells) break;
        if (wc == L' ' || wc == L'\t') last_space = pos;
        cells += cw;
        pos += n;
    }
    size_t end = pos;
    size_t next = pos;
    if (pos < length && text[pos] == '\n') {
        next = pos + 1;
    } else if (pos < length && last_space != SIZE_MAX && last_space > offset) {
        end = last_space;
        next = last_space + 1;
    } else if (end == offset) {
        wchar_t wc;
        size_t n = decode_char(text + offset, length - offset, &wc);
        (void)wc;
        end = offset + n;
        next = end;
    }
    while (end > offset && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
    size_t bytes = end - offset;
    if (out_len > 0) {
        if (bytes >= out_len) bytes = out_len - 1;
        memcpy(out, text + offset, bytes);
        out[bytes] = 0;
    }
    *next_offset = next;
    return bytes;
}

static int wrapped_line_count(const char *text, int width) {
    if (text == NULL || *text == 0) return 1;
    size_t offset = 0;
    int lines = 0;
    char discard[QTC_MAX_TEXT];
    while (offset < strlen(text)) {
        size_t next = offset;
        (void)wrapped_segment(text, offset, width, discard, sizeof(discard), &next);
        lines++;
        if (next <= offset) break;
        offset = next;
    }
    return lines > 0 ? lines : 1;
}

static void screen_put_mentioned(screen *s, int row, int col, int width,
                                 const char *segment, const char *text, size_t offset, ui_style style) {
    screen_put_text(s, row, col, width, segment, style);
    while (text[offset] == ' ') offset++;
    size_t length = strlen(segment);
    for (size_t i = 0; text[i]; ) {
        size_t n = qtc_mention_length(text + i);
        if (!n) { i++; continue; }
        size_t begin = i > offset ? i : offset;
        size_t end = i + n < offset + length ? i + n : offset + length;
        if (begin < end) {
            char prefix[QTC_MAX_TEXT];
            size_t a = begin - offset, b = end - offset;
            memcpy(prefix, segment, a); prefix[a] = 0;
            int left = text_width(prefix);
            memcpy(prefix, segment, b); prefix[b] = 0;
            int right = text_width(prefix);
            for (int c = left; c < right && c < width; c++) {
                screen_cell *cell = screen_at(s, row, col + c);
                if (cell) cell->style = UI_ACCENT;
            }
        }
        i += n;
    }
}

static void render_messages(tui_ctx *t, screen *s) {
    int split = t->width / 3;
    if (split < 30) split = 30;
    if (split > 44) split = 44;
    int footer_top = t->height - 2;
    screen_vline(s, split, 2, footer_top - 2, '|', UI_BORDER);

    qtc_roster roster;
    menu_item items[QTC_MAX_CONTACTS + QTC_MAX_CHANNELS];
    size_t item_count = build_menu(t, items, QTC_ARRAY_LEN(items), &roster);
    (void)selected_menu_pos(t, items, item_count);

    int row = 2;
    for (size_t i = 0; i < roster.pinned_count && row < footer_top; i++, row++) {
        qtc_roster_row *rr = &roster.pinned[i];
        bool selected = false;
        if (rr->kind == 1) {
            char key[16];
            snprintf(key, sizeof(key), "%d", t->state.channels[rr->source_index].index);
            selected = t->selected_kind == 1 && strcmp(t->selected_key, key) == 0;
        } else if (rr->kind == 2) {
            selected = t->selected_kind == 2 && strcmp(t->selected_key, t->state.contacts[rr->source_index].id) == 0;
        }
        render_roster_row(s, row, split, rr, selected);
    }

    int viewport = footer_top - row;
    if (viewport < 0) viewport = 0;
    size_t selected_scroll_row = 0;
    for (size_t i = 0; i < roster.scrollable_count; i++) {
        if (roster.scrollable[i].kind == 3 && t->selected_kind == 3 &&
            strcmp(t->selected_key, t->state.contacts[roster.scrollable[i].source_index].id) == 0) {
            selected_scroll_row = i;
            break;
        }
    }
    qtc_roster_clamp(selected_scroll_row, roster.scrollable_count, (size_t)viewport, &t->contact_scroll);
    for (int v = 0; v < viewport; v++) {
        size_t i = t->contact_scroll + (size_t)v;
        if (i >= roster.scrollable_count) break;
        qtc_roster_row *rr = &roster.scrollable[i];
        bool selected = rr->kind == 3 && t->selected_kind == 3 &&
                        strcmp(t->selected_key, t->state.contacts[rr->source_index].id) == 0;
        render_roster_row(s, row + v, split, rr, selected);
    }

    int right = split + 2;
    int rw = t->width - right - 1;
    char title[256] = "Select a conversation";
    char subtitle[256] = "Use Up/Down and Enter to open a chat";
    if (t->open_kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(t, t->open_key);
        if (ct != NULL) {
            snprintf(title, sizeof(title), "%s", contact_name(ct));
            if (ct->route_known) snprintf(subtitle, sizeof(subtitle), "%s - %d hop%s - key %.12s", qtc_node_type_label(ct->node_type), ct->route_hops, ct->route_hops == 1 ? "" : "s", ct->prefix);
            else snprintf(subtitle, sizeof(subtitle), "%s - flood - key %.12s", qtc_node_type_label(ct->node_type), ct->prefix);
        }
    } else if (t->open_kind == QTC_CONV_CHANNEL) {
        qtc_channel *ch = find_channel(t, atoi(t->open_key));
        if (ch != NULL) {
            snprintf(title, sizeof(title), "# %s", ch->name);
            snprintf(subtitle, sizeof(subtitle), "%s channel - radio slot %d", ch->is_private ? "private" : "public", ch->index);
        }
    }
    screen_put_text(s, 2, right, rw, title, UI_SECTION);
    screen_put_text(s, 3, right, rw, subtitle, UI_MUTED);
    screen_hline(s, 4, right, rw, '-', UI_BORDER);

    int first_message_row = 5;
    int message_rows = footer_top - first_message_row;
    size_t logical_indices[QTC_MAX_MESSAGES];
    size_t logical_count = collect_logical_messages(t, logical_indices,
                                                     QTC_ARRAY_LEN(logical_indices));
    int text_area = rw - 2;
    if (text_area < 12) text_area = 12;
    size_t total_lines = 0;
    size_t starts[QTC_MAX_MESSAGES], heights[QTC_MAX_MESSAGES];
    size_t selected = logical_count;
    for (size_t i = 0; i < logical_count; i++) {
        logical_message_view view;
        load_logical_message(t, logical_indices[i], &view);
        starts[i] = total_lines;
        heights[i] = 1U + (size_t)wrapped_line_count(view.text, text_area);
        total_lines += heights[i];
        if (strcmp(message_logical_key(view.first), t->selected_message) == 0) selected = i;
    }
    size_t max_scroll = total_lines > (size_t)message_rows ?
                        total_lines - (size_t)message_rows : 0;
    if (t->history_scroll > max_scroll) t->history_scroll = max_scroll;
    size_t window_start = total_lines > (size_t)message_rows + t->history_scroll ?
                          total_lines - (size_t)message_rows - t->history_scroll : 0;
    size_t window_end = window_start + (size_t)message_rows;
    if (t->mode == MODE_MESSAGE_SELECT && logical_count) {
        if (selected == logical_count) {
            selected = 0;
            for (size_t i = 0; i < logical_count; i++) if (starts[i] < window_end) selected = i;
            qtc_strlcpy(t->selected_message, message_logical_key(&t->state.messages[logical_indices[selected]]), sizeof(t->selected_message));
        }
        if (starts[selected] < window_start || heights[selected] > (size_t)message_rows)
            window_start = starts[selected];
        else if (starts[selected] + heights[selected] > window_end)
            window_start = starts[selected] + heights[selected] - (size_t)message_rows;
        if (window_start > max_scroll) window_start = max_scroll;
        t->history_scroll = max_scroll - window_start;
        window_end = window_start + (size_t)message_rows;
    }
    size_t visual_line = 0;
    int draw_row = first_message_row;
    for (size_t i = 0; i < logical_count && draw_row < footer_top; i++) {
        logical_message_view view;
        load_logical_message(t, logical_indices[i], &view);
        const qtc_message *m = view.first;
        struct tm tmv;
        time_t ts = (time_t)m->created_at;
        localtime_r(&ts, &tmv);
        char meta[256];
        const char *who = m->direction == QTC_MSG_OUTGOING ? "YOU" : "THEM";
        const char *status = m->direction == QTC_MSG_OUTGOING ?
                             message_status_label(view.status) : "";
        if (view.part_total > 1 && view.part_count < view.part_total) {
            snprintf(meta, sizeof(meta), "%02d:%02d  %s  [%d/%d parts]%s%s",
                     tmv.tm_hour, tmv.tm_min, who, view.part_count, view.part_total,
                     status[0] ? "  " : "", status);
        } else if (t->state.settings.show_signal && m->direction == QTC_MSG_INCOMING) {
            snprintf(meta, sizeof(meta), "%02d:%02d  %s  [SNR %.1f dB / path %d]",
                     tmv.tm_hour, tmv.tm_min, who,
                     (double)m->snr_quarter_db / 4.0, m->path_len);
        } else {
            if (status[0])
                snprintf(meta, sizeof(meta), "%02d:%02d  %s  [%s]",
                         tmv.tm_hour, tmv.tm_min, who, status);
            else
                snprintf(meta, sizeof(meta), "%02d:%02d  %s",
                         tmv.tm_hour, tmv.tm_min, who);
        }
        if (visual_line >= window_start && visual_line < window_end && draw_row < footer_top) {
            ui_style meta_style = view.status == QTC_MSG_FAILED ? UI_DANGER : UI_MUTED;
            if (t->mode == MODE_MESSAGE_SELECT && i == selected) meta_style = UI_SELECTED;
            screen_put_text(s, draw_row++, right, rw, meta, meta_style);
        }
        visual_line++;

        size_t offset = 0;
        int line_count = wrapped_line_count(view.text, text_area);
        for (int line = 0; line < line_count; line++) {
            char segment[QTC_MAX_TEXT];
            size_t next = offset;
            (void)wrapped_segment(view.text, offset, text_area, segment,
                                  sizeof(segment), &next);
            if (visual_line >= window_start && visual_line < window_end && draw_row < footer_top) {
                ui_style style = m->direction == QTC_MSG_OUTGOING ? UI_OUTGOING : UI_INCOMING;
                if (t->mode == MODE_MESSAGE_SELECT && i == selected)
                    screen_put_text(s, draw_row, right, 1, ">", UI_SELECTED);
                screen_put_mentioned(s, draw_row++, right + 2, text_area, segment, view.text, offset, style);
            }
            visual_line++;
            if (next <= offset) break;
            offset = next;
        }
    }
    if (logical_count == 0 && t->open_key[0])
        screen_put_text(s, first_message_row + 1, right, rw,
                        "No messages in this conversation yet.", UI_MUTED);
    if (t->history_scroll > 0)
        screen_put_fmt(s, 4, right + rw - 28, 28, UI_ACCENT,
                       "%zu newer line%s below", t->history_scroll,
                       t->history_scroll == 1 ? "" : "s");

    screen_fill(s, footer_top, 0, t->width - 1, UI_STATUS);
    if (t->banner_until > qtc_now_millis() && t->banner_title[0])
        screen_put_fmt(s, footer_top, 1, t->width - 3, UI_UNREAD,
                       "NEW  %s: %s", t->banner_title, t->banner_body);
    else
        screen_put_text(s, footer_top, 1, t->width - 3,
                        t->mode == MODE_MESSAGE_SELECT ? "r Reply  m Draft  Tab/Esc Roster  Up/Down Select  ? Help" :
                        t->mode == MODE_COMPOSE ? (t->replying ?
                            "Enter Send  @ Mention  Tab Messages  Esc Cancel reply  F4 Settings  F8 Detach" :
                            "Enter Send  @ Mention  Tab Messages  Esc Save draft  F4 Settings  F8 Detach") :
                        "Enter Write  Tab Messages  F4 Settings  F8 Detach  ? Help", UI_STATUS);
    screen_fill(s, t->height - 1, 0, t->width - 1, t->mode == MODE_SEARCH || t->mode == MODE_COMPOSE ? UI_INPUT : UI_NORMAL);
    if (t->mode == MODE_SEARCH) {
        screen_put_text(s, t->height - 1, 1, 8, "Search: ", UI_INPUT);
        screen_put_text(s, t->height - 1, 9, t->width - 11, t->input, UI_INPUT);
        s->cursor = true;
        s->cursor_r = t->height;
        s->cursor_c = 10 + text_width(t->input);
    } else if (t->mode == MODE_COMPOSE) {
        screen_put_text(s, t->height - 1, 1, 9, "Message: ", UI_INPUT);
        screen_put_text(s, t->height - 1, 10, t->width - 12, t->input, UI_INPUT);
        s->cursor = true;
        s->cursor_r = t->height;
        s->cursor_c = 11 + text_width(t->input);
    } else {
        if (t->banner_until > qtc_now_millis() && t->banner_title[0])
            screen_put_fmt(s, t->height - 1, 1, t->width - 3, UI_UNREAD,
                           "%s: %s", t->banner_title, t->banner_body);
        else
            screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
    }
    if (t->mode == MODE_COMPOSE && t->mention_active) {
        char names[MENTION_CANDIDATES][QTC_MAX_NAME];
        size_t count = mention_candidates(t, names);
        if (count && t->mention_cursor >= count) t->mention_cursor = count - 1;
        size_t first = t->mention_cursor >= 4 ? t->mention_cursor - 3 : 0;
        int top = footer_top - 5;
        screen_fill(s, top, right, rw, UI_SECTION);
        screen_put_text(s, top, right, rw, "Mentions: Up/Down Enter accept Esc cancel", UI_SECTION);
        for (size_t i = 0; i < QTC_THEME_COUNT; i++) {
            ui_style style = first + i == t->mention_cursor ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 1 + (int)i, right, rw, style);
            if (first + i < count) screen_put_text(s, top + 1 + (int)i, right, rw, names[first + i], style);
        }
        if (!count) screen_put_text(s, top + 1, right, rw, "No matching names; Esc keeps typed text", UI_MUTED);
    }
}

static size_t pending_invites(tui_ctx *t) {
    size_t n = 0;
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].status == QTC_INVITE_PENDING) n++;
    return n;
}

static void render_page_header(screen *s, const char *title, const char *description) {
    screen_put_text(s, 2, 3, s->w - 6, title, UI_SECTION);
    screen_put_text(s, 3, 3, s->w - 6, description, UI_MUTED);
    screen_hline(s, 4, 2, s->w - 5, '-', UI_BORDER);
}

static void render_channels(tui_ctx *t, screen *s) {
    render_page_header(s, "CHANNELS", "Create, join, invite, rotate, or leave without exposing raw keys.");
    int row = 6;
    bool found = false;
    for (size_t i = 0; i < t->state.channel_count && row < t->height - 3; i++) {
        qtc_channel *ch = &t->state.channels[i];
        if (!ch->configured) continue;
        if (t->selected_channel < 0) t->selected_channel = ch->index;
        bool selected = ch->index == t->selected_channel;
        found = true;
        ui_style style = selected ? UI_SELECTED : (ch->unread ? UI_UNREAD : UI_NORMAL);
        screen_fill(s, row, 3, t->width - 7, style);
        screen_put_fmt(s, row, 4, t->width - 9, style, "%c  %-2d  %s", selected ? '>' : ' ', ch->index, ch->name);
        const char *type = ch->is_private ? "PRIVATE" : "PUBLIC";
        int type_width = text_width(type);
        screen_put_text(s, row, t->width - type_width - 5, type_width, type, style);
        if (ch->unread) screen_put_text(s, row, t->width - type_width - 12, 6, "NEW", style);
        row++;
    }
    if (!found) {
        t->selected_channel = -1;
        screen_put_text(s, row++, 5, t->width - 10, "No configured channels. Press c to create one or J to join an invitation.", UI_MUTED);
    }
    size_t pending = pending_invites(t);
    if (pending > 0) screen_put_fmt(s, row + 1, 5, t->width - 10, UI_UNREAD, "%zu pending private-channel invitation%s - press v to review", pending, pending == 1 ? "" : "s");
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3, "Enter Write  c Create  j Join  i Invite  F6 Back  ? Help", UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static void render_nodes(tui_ctx *t, screen *s) {
    render_page_header(s, "NETWORK NODES", "Repeaters and infrastructure are separate from people you message.");
    /* Reserve metadata columns from the right; names cannot move them. */
    int key_col = t->width - 16;
    int route_col = key_col - 11;
    int type_col = route_col - 10;
    int name_col = 7, name_width = type_col - name_col - 2;
    screen_put_text(s, 5, name_col, name_width, "Name", UI_SECTION);
    screen_put_text(s, 5, type_col, 8, "Type", UI_SECTION);
    screen_put_text(s, 5, route_col, 9, "Route", UI_SECTION);
    screen_put_text(s, 5, key_col, 12, "Key", UI_SECTION);
    int row = 6;
    int idx[QTC_MAX_CONTACTS];
    size_t n = node_selection(t, idx);
    for (size_t i = t->node_scroll; i < n && row < t->height - 3; i++) {
        qtc_contact *c = &t->state.contacts[idx[i]];
        bool selected = strcmp(t->selected_node, c->id) == 0;
        ui_style style = selected ? UI_SELECTED : UI_NORMAL;
        screen_fill(s, row, 3, t->width - 7, style);
        char route[32];
        if (!c->route_known) qtc_strlcpy(route, "flood", sizeof(route));
        else snprintf(route, sizeof(route), "%d hop%s", c->route_hops, c->route_hops == 1 ? "" : "s");
        screen_put_text(s, row, 4, 1, selected ? ">" : " ", style);
        const char *name = contact_name(c);
        if (name_width > 3 && text_width(name) > name_width) {
            screen_put_text(s, row, name_col, name_width - 3, name, style);
            screen_put_text(s, row, name_col + name_width - 3, 3, "...", style);
        } else screen_put_text(s, row, name_col, name_width, name, style);
        screen_put_text(s, row, type_col, 8, qtc_node_type_label(c->node_type), style);
        screen_put_text(s, row, route_col, 9, route, style);
        screen_put_text(s, row, key_col, 12, c->prefix, style);
        row++;
    }
    if (!n) screen_put_text(s, row, 5, t->width - 10, "No repeaters, rooms, sensors, or unknown nodes.", UI_MUTED);
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3, "Up/Down j/k Select  Enter Details  F7/Esc Back  ? Help", UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static void render_node_detail(tui_ctx *t, screen *s) {
    render_page_header(s, "NETWORK NODE DETAIL", "Cached information; opening this page sends no radio query.");
    /* Do not silently switch the inspected identity if it disappears. */
    qtc_contact *c = find_contact(t, t->selected_node);
    char fields[14][512];
    size_t count = 0;
    if (c == NULL || c->node_type == QTC_NODE_PERSON) {
        qtc_strlcpy(fields[count++], "Node is no longer available. Press Esc to return to the list.", sizeof(fields[0]));
    } else {
        snprintf(fields[count++], sizeof(fields[0]), "Name: %s", contact_name(c));
        snprintf(fields[count++], sizeof(fields[0]), "Original name: %s", c->name[0] ? c->name : "unknown");
        snprintf(fields[count++], sizeof(fields[0]), "Alias: %s", c->alias[0] ? c->alias : "none");
        snprintf(fields[count++], sizeof(fields[0]), "Type: %s (%d)", qtc_node_type_label(c->node_type), c->node_type);
        snprintf(fields[count++], sizeof(fields[0]), "Node ID: %s", c->id);
        snprintf(fields[count++], sizeof(fields[0]), "Prefix: %s", c->prefix);
        if (c->route_known) snprintf(fields[count++], sizeof(fields[0]), "Route: known, %d hop%s", c->route_hops, c->route_hops == 1 ? "" : "s");
        else qtc_strlcpy(fields[count++], "Route: flood (unknown route)", sizeof(fields[0]));
        char date[80] = "unknown";
        time_t stamp = (time_t)c->last_heard;
        struct tm local;
        if (c->last_heard > 0 && localtime_r(&stamp, &local) != NULL)
            (void)strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &local);
        snprintf(fields[count++], sizeof(fields[0]), "Last heard: %s", date);
        qtc_strlcpy(fields[count++], "Location: unavailable/unverified (no stored validity indicator)", sizeof(fields[0]));
        snprintf(fields[count++], sizeof(fields[0]), "Favorite: %s", c->favorite ? "yes" : "no");
        snprintf(fields[count++], sizeof(fields[0]), "Favorite group: %s", c->favorite_group[0] ? c->favorite_group : "none");
        snprintf(fields[count++], sizeof(fields[0]), "Unread: %d", c->unread);
        snprintf(fields[count++], sizeof(fields[0]), "Flags: 0x%02X", (unsigned)c->flags);
    }
    int width = t->width - 10;
    size_t lines = 0;
    for (size_t i = 0; i < count; i++) lines += (size_t)wrapped_line_count(fields[i], width);
    size_t rows = node_rows(t), max = lines > rows ? lines - rows : 0;
    if (t->node_detail_scroll > max) t->node_detail_scroll = max;
    size_t line = 0;
    for (size_t i = 0; i < count; i++) {
        size_t offset = 0;
        do {
            char segment[512]; size_t next;
            (void)wrapped_segment(fields[i], offset, width, segment, sizeof(segment), &next);
            if (line >= t->node_detail_scroll && line - t->node_detail_scroll < rows)
                screen_put_text(s, 6 + (int)(line - t->node_detail_scroll), 5, width, segment, UI_NORMAL);
            line++; offset = next;
        } while (offset < strlen(fields[i]));
    }
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3, "Up/Down j/k PgUp/PgDn Scroll  Esc/F7 Back  ? Help", UI_STATUS);
    screen_put_fmt(s, t->height - 1, 1, t->width - 3, UI_MUTED, "Lines %zu-%zu of %zu", t->node_detail_scroll + 1,
                   t->node_detail_scroll + (lines < rows ? lines : rows), lines);
}

static const char *onoff(bool value) { return value ? "ON" : "OFF"; }

static void setting_row(screen *s, int row, const char *key, const char *label, const char *value) {
    screen_put_text(s, row, 5, 4, key, UI_ACCENT);
    screen_put_text(s, row, 10, 42, label, UI_NORMAL);
    screen_put_text(s, row, 54, s->w - 59, value, strcmp(value, "OFF") == 0 ? UI_MUTED : UI_SUCCESS);
}

static void render_settings(tui_ctx *t, screen *s) {
    render_page_header(s, "SETTINGS", "Messaging, notification, display, and radio controls are saved per profile.");
    int row = 6;
    setting_row(s, row++, "1", "Desktop notifications", onoff(t->state.settings.desktop_notifications));
    setting_row(s, row++, "2", "Notification sound", onoff(t->state.settings.sound_enabled));
    setting_row(s, row++, "3", "Notify for direct messages", onoff(t->state.settings.notify_direct));
    setting_row(s, row++, "4", "Notify for channel messages", onoff(t->state.settings.notify_channel));
    setting_row(s, row++, "6", "In-terminal message banners", onoff(t->state.settings.banner_enabled));
    setting_row(s, row++, "7", "Suppress open-chat notifications", onoff(t->state.settings.suppress_open_conversation));
    setting_row(s, row++, "8", "Retry unconfirmed direct messages", onoff(t->state.settings.retry_unconfirmed));
    setting_row(s, row++, "9", "Reset stale route before retry", onoff(t->state.settings.reset_stale_route));
    setting_row(s, row++, "0", "Show SNR and path in history", onoff(t->state.settings.show_signal));
    if (row < t->height - 7) {
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Theme [t]: %s", theme_name(theme_index(t)));
        screen_put_text(s, row++, 5, t->width - 10,
                        "Themes: Green Phosphor | Amber CRT | Midnight BBS | Mono TTY", UI_MUTED);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Stored-message poll [,/.]: %d seconds    Direct attempts [[/]]: %d",
                       t->state.settings.stored_poll_seconds,
                       t->state.settings.max_direct_attempts);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Radio [d name / p power]: %s    TX %d dBm (max %d)",
                       t->state.radio_name[0] ? t->state.radio_name : "unknown",
                       t->state.radio_tx_power, t->state.radio_max_tx_power);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_MUTED,
                       "Device: %s", t->state.settings.serial_device[0] ?
                       t->state.settings.serial_device : "automatic");
        if (t->state.radio_freq > 0.0)
            screen_put_fmt(s, row++, 5, t->width - 10, UI_MUTED,
                           "Preset: %.4f MHz  BW %.1f kHz  SF %d  CR %d",
                           t->state.radio_freq, t->state.radio_bw,
                           t->state.radio_sf, t->state.radio_cr);
    }
    if (t->action_feedback_until > qtc_now_millis() &&
        t->action_feedback[0] != 0) {
        ui_style style = UI_ACCENT;
        if (t->action_feedback_level == FEEDBACK_SUCCESS) style = UI_SUCCESS;
        else if (t->action_feedback_level == FEEDBACK_ERROR) style = UI_DANGER;
        screen_fill(s, t->height - 3, 0, t->width - 1, style);
        screen_put_fmt(s, t->height - 3, 2, t->width - 4, style,
                       "ADVERT  %s", t->action_feedback);
    }
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3,
                    "t Theme  5 Cycle  o Preset  r/F5 Reconnect  F4 Back  ? Help",
                    UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static void render_modal(tui_ctx *t, screen *s) {
    int w = t->width > 90 ? 80 : t->width - 6;
    int h = 9;
    int top = (t->height - h) / 2;
    int left = (t->width - w) / 2;
    if (t->mode == MODE_ALIAS || t->mode == MODE_FAVORITE_GROUP ||
        t->mode == MODE_DEVICE_NAME || t->mode == MODE_TX_POWER) {
        const char *title = "EDIT VALUE";
        const char *prompt = "Value:";
        if (t->mode == MODE_ALIAS) { title = "CONTACT ALIAS"; prompt = "Alias (empty clears it):"; }
        else if (t->mode == MODE_FAVORITE_GROUP) { title = "FAVORITE GROUP"; prompt = "Favorite group:"; }
        else if (t->mode == MODE_DEVICE_NAME) { title = "RADIO NAME"; prompt = "New radio name (1-32 bytes):"; }
        else if (t->mode == MODE_TX_POWER) { title = "TX POWER"; prompt = "TX power in dBm:"; }
        screen_box(s, top, left, h, w, title);
        screen_put_text(s, top + 2, left + 3, w - 6, prompt, UI_NORMAL);
        screen_fill(s, top + 4, left + 3, w - 6, UI_INPUT);
        screen_put_text(s, top + 4, left + 4, 2, "> ", UI_INPUT);
        screen_put_text(s, top + 4, left + 6, w - 10, t->input, UI_INPUT);
        screen_put_text(s, top + 6, left + 3, w - 6,
                        "Enter saves. Esc cancels.", UI_MUTED);
        s->cursor = true;
        s->cursor_r = top + 5;
        s->cursor_c = left + 7 + text_width(t->input);
        return;
    }
    if (t->mode == MODE_CREATE_CHANNEL || t->mode == MODE_JOIN_CHANNEL) {
        const char *title = t->mode == MODE_CREATE_CHANNEL ? "CREATE PRIVATE CHANNEL" : "JOIN PRIVATE CHANNEL";
        screen_box(s, top, left, h, w, title);
        screen_put_text(s, top + 2, left + 3, w - 6, t->mode == MODE_CREATE_CHANNEL ? "Channel name (" QTC_DISPLAY_NAME " generates a secure key):" : "Invitation URI, raw 32-character key, or Name:key:", UI_NORMAL);
        screen_fill(s, top + 4, left + 3, w - 6, UI_INPUT);
        screen_put_text(s, top + 4, left + 4, 2, "> ", UI_INPUT);
        screen_put_text(s, top + 4, left + 6, w - 10, t->input, UI_INPUT);
        screen_put_text(s, top + 6, left + 3, w - 6, "Enter confirms. Esc cancels.", UI_MUTED);
        s->cursor = true;
        s->cursor_r = top + 5;
        s->cursor_c = left + 7 + text_width(t->input);
        return;
    }
    if (t->mode == MODE_THEME_PICKER) {
        h = QTC_THEME_COUNT + 5;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "SELECT THEME");
        for (int i = 0; i < QTC_THEME_COUNT; i++) {
            ui_style style = i == t->theme_cursor ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 2 + i, left + 3, w - 6, style);
            screen_put_fmt(s, top + 2 + i, left + 4, w - 8, style,
                           "%d. %s%s", i + 1, theme_name(i),
                           i == theme_index(t) ? "  [current]" : "");
        }
        screen_put_text(s, top + h - 2, left + 3, w - 6,
                        "Up/Down or j/k selects. Enter applies. Esc cancels.", UI_MUTED);
        return;
    }
    if (t->mode == MODE_PRESET_PICKER) {
        h = 17;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "FIRST-CONNECT RADIO PRESET");
        screen_put_text(s, top + 2, left + 3, w - 6,
                        "This changes the radio frequency and LoRa parameters.", UI_DANGER);
        for (size_t i = 0; i < QTC_ARRAY_LEN(RADIO_PRESETS); i++) {
            const radio_preset *preset = &RADIO_PRESETS[i];
            ui_style style = (int)i == t->preset_cursor ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 4 + (int)i * 2, left + 3, w - 6, style);
            screen_put_fmt(s, top + 4 + (int)i * 2, left + 4, w - 8, style,
                           "%zu. %-26s %.4f MHz  BW %.1f  SF%d  CR%d",
                           i + 1, preset->name, preset->freq_mhz, preset->bw_khz,
                           preset->sf, preset->cr);
        }
        screen_put_text(s, top + 13, left + 3, w - 6,
                        "Confirm the exact preset with your local MeshCore community.", UI_DANGER);
        screen_put_text(s, top + h - 2, left + 3, w - 6,
                        "Up/Down or j/k selects. Enter applies. Esc cancels.", UI_MUTED);
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        h = t->height - 6;
        if (h > 28) h = 28;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "SELECT MESHCORE CONTACTS");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Invite to \"%s\"", ch ? ch->name : "channel");
        screen_put_fmt(s, top + 3, left + 3, w - 6, UI_INPUT, "Search: %s", t->invite_search);
        int indices[QTC_MAX_CONTACTS];
        size_t count = person_list(t, indices, QTC_ARRAY_LEN(indices));
        int pos = invite_cursor_pos(t, indices, count);
        int rows = h - 7;
        int start = pos >= rows ? pos - rows + 1 : 0;
        for (int r = 0; r < rows && (size_t)(start + r) < count; r++) {
            qtc_contact *c = &t->state.contacts[indices[start + r]];
            bool selected = start + r == pos;
            ui_style style = selected ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 5 + r, left + 2, w - 4, style);
            screen_put_fmt(s, top + 5 + r, left + 3, w - 6, style, "%c [%c] %s", selected ? '>' : ' ', invite_selected(t, c->id) ? 'x' : ' ', contact_name(c));
        }
        screen_put_fmt(s, top + h - 2, left + 3, w - 6, UI_MUTED, "Type search  Space select  Enter review  Esc cancel   Selected: %zu", t->invite_count);
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW) {
        h = 11 + (int)t->invite_count;
        if (h > t->height - 4) h = t->height - 4;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "REVIEW PRIVATE CHANNEL INVITATION");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        if (t->invite_count == 1) {
            qtc_contact *ct = find_contact(t, t->invite_ids[0]);
            screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Invite %s to private channel \"%s\"?", ct ? contact_name(ct) : t->invite_ids[0], ch ? ch->name : "");
        } else {
            screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Send \"%s\" invitation to %zu contacts?", ch ? ch->name : "", t->invite_count);
        }
        screen_put_text(s, top + 3, left + 3, w - 6, "This sends each recipient the private channel key.", UI_DANGER);
        int recipient_row = top + 5;
        for (size_t i = 0; i < t->invite_count && recipient_row < top + h - 3; i++, recipient_row++) {
            qtc_contact *ct = find_contact(t, t->invite_ids[i]);
            screen_put_text(s, recipient_row, left + 5, w - 10, ct ? contact_name(ct) : t->invite_ids[i], UI_NORMAL);
        }
        screen_put_fmt(s, top + h - 2, left + 3, w - 6, UI_ACCENT, "%s Cancel       %s Send %zu invite%s", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ", t->invite_count, t->invite_count == 1 ? "" : "s");
        return;
    }
    if (t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM) {
        screen_box(s, top, left, h, w, t->mode == MODE_ROTATE_CONFIRM ? "ROTATE CHANNEL KEY" : "LEAVE CHANNEL");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "%s \"%s\"?", t->mode == MODE_ROTATE_CONFIRM ? "Generate a new private key for" : "Leave", ch ? ch->name : "channel");
        screen_put_text(s, top + 3, left + 3, w - 6, t->mode == MODE_ROTATE_CONFIRM ? "Old invitations will stop working." : "Local message history will be preserved.", UI_DANGER);
        screen_put_fmt(s, top + 6, left + 3, w - 6, UI_ACCENT, "%s Cancel               %s %s", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ", t->mode == MODE_ROTATE_CONFIRM ? "Rotate" : "Leave");
        return;
    }
    if (t->mode == MODE_INCOMING_INVITE) {
        qtc_invitation *inv = find_invitation(t, t->incoming_invite_id);
        screen_box(s, top, left, h, w, "PRIVATE CHANNEL INVITATION");
        screen_put_text(s, top + 2, left + 3, w - 6, inv ? inv->channel_name : "Unknown channel", UI_SECTION);
        qtc_contact *ct = inv ? find_contact(t, inv->sender_contact_id) : NULL;
        screen_put_fmt(s, top + 3, left + 3, w - 6, UI_NORMAL, "From: %s", ct ? contact_name(ct) : (inv ? inv->sender_contact_id : "unknown"));
        screen_put_text(s, top + 4, left + 3, w - 6, "Joining writes the private key into a free radio channel slot.", UI_DANGER);
        screen_put_fmt(s, top + 6, left + 3, w - 6, UI_ACCENT, "%s Ignore               %s Join", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ");
    }
}

static void out_append(char *out, size_t cap, size_t *used, const char *data, size_t len) {
    if (*used >= cap || len == 0) return;
    if (len > cap - *used) len = cap - *used;
    memcpy(out + *used, data, len);
    *used += len;
}

static void out_fmt(char *out, size_t cap, size_t *used, const char *fmt, ...) {
    if (*used >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t written = (size_t)n;
    if (written > cap - *used) written = cap - *used;
    *used += written;
}

static const char *radio_label(const tui_ctx *t) {
    if (t->demo) return "DEMO";
    if (t->loading) return "DISCONNECTED";
    if (!t->state.radio_connected) return "RECONNECTING";
    if (!t->session_ready) return "INITIALIZING";
    return "RADIO READY";
}

static void render_shell(tui_ctx *t, screen *s) {
    screen_fill(s, 0, 0, t->width - 1, UI_HEADER);
    const char *label = radio_label(t);
    int state_width = (int)strlen(label);
    int state_col = t->width - state_width - 2;
    screen_put_fmt(s, 0, 1, state_col - 2, UI_HEADER,
                   QTC_DISPLAY_NAME " TERMINAL %s | %s", QTC_VERSION,
                   t->state.radio_name[0] ? t->state.radio_name : t->profile);
    screen_put_text(s, 0, state_col, state_width, label,
                    t->demo || (t->state.radio_connected && t->session_ready) ?
                    UI_READY : UI_RECONNECTING);
    screen_fill(s, 1, 0, t->width - 1, UI_STATUS);
    const char *tabs[] = {"MESSAGES", "F6 CHANNELS", "F7 NODES", "F4 SETTINGS"};
    int col = 1;
    for (int i = 0; i < 4; i++) {
        screen_put_text(s, 1, col, (int)strlen(tabs[i]), tabs[i],
                        t->view == (tui_view)i ? UI_NAVIGATION : UI_STATUS);
        col += (int)strlen(tabs[i]) + 2;
    }
    if (can_open_help(t)) screen_put_text(s, 1, col, t->width - col - 2, "? Help", UI_STATUS);
}

static void render_help(tui_ctx *t, screen *s) {
    static const char *const lines[] = {
        "GLOBAL",
        "F4 Settings   F5 Reconnect   F6 Channels   F7 Nodes",
        "F8 / Ctrl+C Detach   Ctrl+Q twice Stop core",
        "? Help (outside text entry)   Esc / ? Close help",
        "CONVERSATIONS",
        "Up/Down or j/k Select   Enter Write   / Search",
        "f Favorite   F2 / e Alias   g Favorite group",
        "COMPOSER",
        "Enter Send   @ Mention   Tab Message selection",
        "Esc Save draft (or cancel reply); ? types a question mark",
        "Mention picker: Up/Down Select   Enter Accept   Esc Cancel",
        "MESSAGE HISTORY",
        "Tab Select messages   Up/Down or j/k Select",
        "PgUp/PgDn Scroll/jump   r Reply   m Draft   Tab/Esc Roster",
        "CHANNELS",
        "Up/Down or n/p Select   Enter Write   c Create   j Join",
        "i Invite   r Rotate key   d Leave   v Review invites",
        "NETWORK NODES",
        "Up/Down or j/k Select   Enter Details   F7 Messages",
        "NODE DETAIL",
        "Up/Down or j/k Scroll   PgUp/PgDn Page   Esc/F7 Back",
        "SETTINGS",
        "t Theme picker   5 Cycle theme",
        "Theme picker: Up/Down or j/k Select   Enter Apply   Esc Cancel",
        "o Preset   d Radio name   p Power   c Copy card   y Sync",
        "z Zero-hop advert   x Flood advert   r Reconnect"
    };
    int h = t->height - 4, w = t->width > 90 ? 84 : t->width - 4;
    int top = 2, left = (t->width - w) / 2;
    size_t rows = (size_t)(h - 4), count = QTC_ARRAY_LEN(lines);
    size_t max = count > rows ? count - rows : 0;
    if (t->help_scroll > max) t->help_scroll = max;
    screen_box(s, top, left, h, w, QTC_DISPLAY_NAME " KEYBOARD HELP");
    for (size_t i = 0; i < rows && i + t->help_scroll < count; i++) {
        const char *line = lines[i + t->help_scroll];
        screen_put_text(s, top + 2 + (int)i, left + 2, w - 4, line,
                        strchr(line, ' ') == NULL || strcmp(line, "MESSAGE HISTORY") == 0 ||
                        strcmp(line, "NETWORK NODES") == 0 || strcmp(line, "NODE DETAIL") == 0 ?
                        UI_SECTION : UI_NORMAL);
    }
    screen_put_text(s, top + h - 2, left + 2, w - 4,
                    "Up/Down j/k Scroll   PgUp/PgDn Page   Esc/? Close", UI_STATUS);
    s->cursor = false;
}

static void render(tui_ctx *t) {
    update_size(t);
    screen s;
    if (screen_init(&s, t->width, t->height) != 0) return;
    const ui_theme *theme = active_theme(t);

    render_shell(t, &s);

    if (t->width < 68 || t->height < 18) {
        screen_put_fmt(&s, 4, 3, t->width - 6, UI_DANGER, "Terminal is too small: %dx%d", t->width, t->height);
        screen_put_text(&s, 6, 3, t->width - 6, QTC_DISPLAY_NAME " needs at least 68 columns by 18 rows.", UI_NORMAL);
        screen_put_text(&s, 8, 3, t->width - 6, "Resize the terminal; the interface will redraw automatically.", UI_MUTED);
    } else {
        if (t->view == VIEW_MESSAGES) render_messages(t, &s);
        else if (t->view == VIEW_CHANNELS) render_channels(t, &s);
        else if (t->view == VIEW_NODES) {
            if (t->mode == MODE_NODE_DETAIL) render_node_detail(t, &s);
            else render_nodes(t, &s);
        }
        else render_settings(t, &s);
        if (t->mode != MODE_NORMAL && t->mode != MODE_SEARCH && t->mode != MODE_COMPOSE &&
            t->mode != MODE_MESSAGE_SELECT && t->mode != MODE_NODE_DETAIL) render_modal(t, &s);
        if (t->help_open) render_help(t, &s);
    }

    /* Allow for cell bytes, style changes, and Unicode cursor re-anchors. */
    size_t cap = (size_t)t->width * (size_t)t->height * 64U + (size_t)t->height * 40U + 512U;
    char *out = malloc(cap);
    if (out == NULL) { screen_free(&s); return; }
    size_t used = 0;
    out_append(out, cap, &used, "\x1b[?25l", strlen("\x1b[?25l"));
    for (int r = 0; r < t->height; r++) {
        out_fmt(out, cap, &used, "\x1b[%d;1H", r + 1);
        out_append(out, cap, &used, theme->sgr[UI_NORMAL], strlen(theme->sgr[UI_NORMAL]));
        out_append(out, cap, &used, "\x1b[2K", strlen("\x1b[2K"));
        int current_style = -1;
        for (int c = 0; c < t->width - 1; c++) {
            screen_cell *cell = screen_at(&s, r, c);
            if (cell == NULL || cell->continuation) continue;
            if ((int)cell->style != current_style) {
                const char *sgr = theme->sgr[cell->style < UI_STYLE_COUNT ? cell->style : UI_NORMAL];
                out_append(out, cap, &used, sgr, strlen(sgr));
                current_style = cell->style;
            }
            out_append(out, cap, &used, cell->bytes, cell->len);
            /* The terminal may disagree with libc about Unicode width. Emit
             * the complete cell (including combining marks), then restore the
             * next framebuffer position. Plain ASCII needs no extra movement. */
            if ((cell->len > 1 || cell->width > 1) && c + cell->width < t->width - 1)
                out_fmt(out, cap, &used, "\x1b[%d;%dH", r + 1, c + cell->width + 1);
        }
    }
    out_append(out, cap, &used, "\x1b[0m", strlen("\x1b[0m"));
    if (s.cursor) {
        if (s.cursor_c < 1) s.cursor_c = 1;
        if (s.cursor_c > t->width) s.cursor_c = t->width;
        out_fmt(out, cap, &used, "\x1b[%d;%dH\x1b[?25h", s.cursor_r, s.cursor_c);
    } else {
        out_append(out, cap, &used, "\x1b[?25l", strlen("\x1b[?25l"));
    }
    queue_output(t, out, used);
    screen_free(&s);
    t->dirty = false;
}

int qtc_tui_run(const qtc_paths *paths, int theme_override) {
    (void)setlocale(LC_CTYPE, "");
    tui_ctx t;
    memset(&t, 0, sizeof(t));
    t.theme_overridden = theme_override >= 0;
    t.theme_override = theme_override;
    t.state.settings.theme = QTC_THEME_DEFAULT;
    qtc_strlcpy(t.profile, paths->profile, sizeof(t.profile));
    t.running = true;
    t.dirty = true;
    t.selected_channel = -1;
    t.fd = qtc_ipc_client_connect(paths->socket_path, 2000);
    if (t.fd < 0) {
        fprintf(stderr, "Could not connect to " QTC_DISPLAY_NAME " core: %s\n", strerror(errno));
        return 1;
    }
    qtc_ipc_reader_init(&t.reader);
    if (verify_core_version(t.fd) != 0) {
        close(t.fd);
        fprintf(stderr, QTC_DISPLAY_NAME " background core is incompatible with " QTC_DISPLAY_NAME " %s; restart " QTC_DISPLAY_NAME "\n",
                QTC_VERSION);
        return 1;
    }
    if (set_raw_terminal(&t) != 0) {
        close(t.fd);
        fprintf(stderr, QTC_DISPLAY_NAME " requires an interactive terminal\n");
        return 1;
    }
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGPIPE, SIG_IGN);

    while (t.running) {
        int64_t now = qtc_now_millis();
        if (g_resize) {
            g_resize = 0;
            t.dirty = true;
        }
        if (t.banner_until > 0 && t.banner_until <= now) {
            t.banner_until = 0;
            t.banner_title[0] = 0;
            t.banner_body[0] = 0;
            t.dirty = true;
        }
        if (t.action_feedback_until > 0 &&
            t.action_feedback_until <= now) {
            t.action_feedback_until = 0;
            t.action_feedback[0] = 0;
            t.action_feedback_level = FEEDBACK_NONE;
            t.dirty = true;
        }

        /* Build only the newest frame. Terminal writes are nonblocking, so a
         * slow terminal cannot delay message/ACK processing or keyboard input. */
        if (t.dirty) render(&t);
        flush_output(&t);

        struct pollfd pfd[3] = {
            {.fd = t.fd, .events = POLLIN},
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = STDOUT_FILENO,
             .events = t.output != NULL ? POLLOUT : 0}
        };
        int timeout = 100;
        if (t.banner_until > now && t.banner_until - now < timeout)
            timeout = (int)(t.banner_until - now);
        if (t.action_feedback_until > now &&
            t.action_feedback_until - now < timeout)
            timeout = (int)(t.action_feedback_until - now);
        int rc = poll(pfd, 3, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Core events are deliberately handled before keyboard input. This
         * keeps message delivery live even while a user is continuously
         * typing or pasting into the composer. */
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            qtc_strlcpy(t.status, "Background core disconnected", sizeof(t.status));
            t.dirty = true;
            t.running = false;
        } else if (pfd[0].revents & POLLIN) {
            for (;;) {
                uint8_t buf[65536];
                ssize_t n = recv(t.fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    if (qtc_ipc_reader_feed(&t.reader, buf, (size_t)n,
                                            ipc_frame, &t) != 0) {
                        t.running = false;
                        break;
                    }
                    continue;
                }
                if (n == 0) t.running = false;
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    t.running = false;
                break;
            }
        }

        if (t.running && (pfd[1].revents & POLLIN)) {
            for (;;) {
                uint8_t buf[4096];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    process_input(&t, buf, (size_t)n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    t.running = false;
                break;
            }
        }

        if (pfd[2].revents & POLLOUT) flush_output(&t);

        /* Coalesce all events from this poll wake into one newest frame, then
         * attempt the write immediately. The compose buffer and cursor are
         * rendered from state on every frame, so incoming events cannot erase
         * a draft. */
        if (t.running && t.dirty) {
            render(&t);
            flush_output(&t);
        }
    }
    leave_conversation(&t);
    restore_terminal(&t);
    close(t.fd);
    return 0;
}
