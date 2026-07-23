#include "nob.h"

#define TD_JSON_CLIENT_HEADER <td/telegram/td_json_client.h>
#include TD_JSON_CLIENT_HEADER
#include <signal.h>
#include "tgf.h"

int history_window_hours         = 24;
int enable_sequential_forwarding = 0;
int sequence_direction           = 0;

int      api_id;
char    *api_hash;
char   **source_channels;
int      num_sources;
char    *dest_channel;
char    *history_file;
int      forward_delay_sec;

int       authorized       = 0;
int       dest_resolved    = 0;
long long dest_chat_id     = 0;
long long *source_chat_ids = NULL;
char      *src_name        = "unknown";
int       source_count     = 0;
int       pending_req      = 0;

char **history_set = NULL;
int  history_set_size = 0;
int  history_count = 0;
time_t *history_time = NULL;

SeqEntry seq_tracker[MAX_SEQ_TRACKER];
int      seq_tracker_count = 0;

ForwardJob *fwd_queue = NULL;
int        fwd_queue_count = 0;
int        fwd_queue_cap = 0;
double     fwd_last_time   = 0;

volatile bool keep_running = true;
bool dashboard_active = false;
char status_msg[256] = "";

static int resolve_index = 0;
static int resolve_active = 0;

#ifndef TGF_NOGUI
#include <curses.h>
static bool ncurses_ready = false;

static void ncurses_init() {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);
    ncurses_ready = true;
    dashboard_active = true;
}

static void ncurses_cleanup() {
    if(ncurses_ready) endwin();
    dashboard_active = false;
}

static int  action_msg_row = 0;

static void print_dashboard(const char *dest, const char *source, int src_count, int delay) {
    if (!ncurses_ready) return;
    int row = 1;
    clear();
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "           -->TGF - TELEGRAM FEED MONITOR-->          ");
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    mvprintw(row++, 0, "       Target Channel ........ %s ", dest              );
    mvprintw(row++, 0, "       Source Channels ....... %d ", src_count         );
    mvprintw(row++, 0, "       Forward Delay ......... %dsec ", delay          );
    mvprintw(row++, 0, "       Status ................ %s ", "ACTIVE"          );
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    action_msg_row = row;
    mvprintw(row++, 0, "       FORWARDED from %s -> %s", source, dest        );
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    if (status_msg[0]) {
        mvprintw(row++, 0, "       %s", status_msg);
        status_msg[0] = '\0';
    }
    mvprintw(row++, 0, "       [TGF] Running (PID: %d)", getpid());
    mvprintw(row++, 0, "       [TGF] Stop with: Ctrl+C | kill %d | pkill tgf  ", getpid());
    refresh();
}

static void update_action_msg(const char *source, const char *dest) {
    if (!ncurses_ready) return;
    mvprintw(action_msg_row, 0, "       FORWARDED from %s -> %s", source, dest);
    refresh();
}
#endif

void set_status(const char *msg)
{
    snprintf(status_msg, sizeof(status_msg), "%s", msg);
    if (!dashboard_active) fprintf(stderr, "%s\n", msg);
}

// forward declarations
static void on_auth_state(void *client, const char *json);
static void on_response(void *client, const char *json, const char *extra, char *argv[]);
static void on_update(void *client, const char *json);
static void on_error(void *client, const char *json);
static void resolve_next(void *client);
static void poll_channels(void *client);
static void handle_history_response(void *client, const char *json, long long src_chat_id);
static int  load_config(const char *path);

static void sig_handler(int sig) {
    (void)sig;
    keep_running = false;
}

void send_req(void *client, const char *type, const char *payload, const char *extra) {
    char buf[8192];
    int n;
    if (payload && payload[0])
        n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload, extra);
    else
        n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",\"@extra\":\"%s\"}", type, extra);
    if (n >= (int)sizeof(buf)) set_status("WARNING: request truncated");
    td_json_client_send(client, buf);
    pending_req++;
}

static long long cJSON_GetInt64(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) return 0;
    if (cJSON_IsNumber(v)) return (long long)v->valuedouble;
    if (cJSON_IsString(v)) return atoll(v->valuestring);
    return 0;
}

static const char *cJSON_GetStr(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static void json_escape_build(char *buf, size_t size, const char *key, const char *val) {
    if (size == 0) return;
    int pos = snprintf(buf, size, "\"%s\":\"", key);
    while (*val && pos < (int)size - 4) {
        if ((*val == '"' || *val == '\\') && pos < (int)size - 3) buf[pos++] = '\\';
        buf[pos++] = *val++;
    }
    if (pos < (int)size - 2) { buf[pos++] = '"'; }
    buf[size - 1] = 0;
}

static void on_auth_state(void *client, const char *json) {
    if (!strstr(json, "\"@type\":\"updateAuthorizationState\"")) return;

    if (strstr(json, "authorizationStateWaitTdlibParameters")) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "\"database_directory\":\"tdlib_db\","
            "\"files_directory\":\"tdlib_files\","
            "\"use_message_database\":true,"
            "\"use_secret_chats\":false,"
            "\"api_id\":%d,"
            "\"api_hash\":\"%s\","
            "\"system_language_code\":\"en\","
            "\"device_model\":\"Desktop\","
            "\"application_version\":\"1.0\"",
            api_id, api_hash);
        send_req(client, "setTdlibParameters", buf, "auth_params");
    } else if (strstr(json, "authorizationStateWaitPhoneNumber")) {
        char phone[32] = {0};
        printf("\033[2J\033[H");
        printf("Welcome to the TGF\n");
        printf("Enter your phone number with country code\n");
        printf("Phone (+xxx): "); fflush(stdout);
        if (fgets(phone, sizeof(phone), stdin)) phone[strcspn(phone, "\n")] = 0;
        char payload[256];
        json_escape_build(payload, sizeof(payload), "phone_number", phone);
        send_req(client, "setAuthenticationPhoneNumber", payload, "auth_phone");
    } else if (strstr(json, "authorizationStateWaitCode")) {
        char code[32] = {0};
        printf("Code: "); fflush(stdout);
        if (fgets(code, sizeof(code), stdin)) code[strcspn(code, "\n")] = 0;
        char payload[256];
        json_escape_build(payload, sizeof(payload), "code", code);
        send_req(client, "checkAuthenticationCode", payload, "auth_code");
    } else if (strstr(json, "authorizationStateWaitPassword")) {
        char pw[128] = {0};
        printf("2FA password: "); fflush(stdout);
        if (fgets(pw, sizeof(pw), stdin)) pw[strcspn(pw, "\n")] = 0;
        char payload[256];
        json_escape_build(payload, sizeof(payload), "password", pw);
        send_req(client, "checkAuthenticationPassword", payload, "auth_pw");
    } else if (strstr(json, "authorizationStateReady")) {
        authorized = 1;
        printf("\n✓ Authorized!\n");
        const char *dest_name = dest_channel;
        if (dest_name[0] == '@') dest_name++;
        if (strcmp(dest_name, "me") == 0 || strcmp(dest_name, "Me") == 0) {
            send_req(client, "getMe", "", "getme");
        } else {
            char payload[256];
            snprintf(payload, sizeof(payload), "\"username\":\"%s\"", dest_name);
            send_req(client, "searchPublicChat", payload, "resolve_dest");
        }
    }
}

static void resolve_next(void *client) {
    while (resolve_index < num_sources) {
        const char *name = source_channels[resolve_index];
        if (name[0] == '@') name++;
        char extra[32];
        snprintf(extra, sizeof(extra), "resolve_%d", resolve_index);
        char payload[256];
        snprintf(payload, sizeof(payload), "\"username\":\"%s\"", name);
        send_req(client, "searchPublicChat", payload, extra);
        resolve_index++;
        resolve_active = 1;
        return;
    }
    resolve_active = 0;
}

static void on_response(void *client, const char *json, const char *extra, char *argv[]) {
    pending_req--;

    if (strcmp(extra, "auth_params") == 0 ||
        strcmp(extra, "auth_phone")  == 0 ||
        strcmp(extra, "auth_code")   == 0 ||
        strcmp(extra, "auth_pw")     == 0) return;

    if (strcmp(extra, "getme") == 0) {
        cJSON *root = cJSON_Parse(json);
        if (!root) return;
        long long user_id = cJSON_GetInt64(root, "id");
        cJSON_Delete(root);
        char payload[256];
        snprintf(payload, sizeof(payload), "\"user_id\":%lld,\"force\":true", user_id);
        send_req(client, "createPrivateChat", payload, "resolve_dest");
        return;
    }

    if (strncmp(extra, "resolve_", 8) == 0) {
        cJSON *root = cJSON_Parse(json);
        if (!root) return;
        const cJSON *type_item = cJSON_GetObjectItem(root, "@type");
        if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "error") == 0) {
            cJSON_Delete(root);
            return;
        }
        long long chat_id = cJSON_GetInt64(root, "id");
        if (strcmp(extra, "resolve_dest") == 0) {
            dest_chat_id = chat_id;
            dest_resolved = 1;
            resolve_index = 0;
            resolve_next(client);
        } else {
            int idx = atoi(extra + 8);
            source_chat_ids[idx] = chat_id;
            source_count++;
            if (argv[1] != NULL && strcmp(argv[1], "-d") == 0) {
                printf("\r  Resolving channels... %d/%d\n", source_count, num_sources);
            }
            fflush(stdout);
            resolve_next(client);
        }
        cJSON_Delete(root);
        return;
    }

    if (strncmp(extra, "history_", 8) == 0) {
        long long src_chat_id = atoll(extra + 8);
        handle_history_response(client, json, src_chat_id);
        return;
    }

    if (strncmp(extra, "fwd_", 4) == 0) {
        cJSON *root = cJSON_Parse(json);
        if (!root) return;
        const cJSON *type_item = cJSON_GetObjectItem(root, "@type");
        if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "error") == 0) {
            const cJSON *m = cJSON_GetObjectItem(root, "message");
            { char b[128]; snprintf(b, sizeof(b), "Forward fail [%s]: %s", extra, m ? m->valuestring : "?"); set_status(b); }
        } else {
            long long src_chat_id = 0;
            if (extra[4]) src_chat_id = atoll(extra + 4);
            for (int i = 0; i < num_sources; i++) {
                if (source_chat_ids[i] == src_chat_id) {
                    src_name = source_channels[i];
                    break;
                }
            }
            if (argv[1] != NULL && strcmp(argv[1], "-d") == 0) {
                printf(" FORWARDED from %s → %s\n", src_name, dest_channel);
            }
        }
        cJSON_Delete(root);
        return;
    }
}

static void handle_history_response(void *client, const char *json, long long src_chat_id) {
    (void)client;
    cJSON *root = cJSON_Parse(json);
    if (!root) { set_status("Failed to parse history JSON"); return; }

    const cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    if (!msgs || !cJSON_IsArray(msgs)) { cJSON_Delete(root); return; }

    int total = cJSON_GetArraySize(msgs);
    if (total == 0) {
        if (enable_sequential_forwarding && !seq_tracker_is_backfill_done(src_chat_id)) {
            flush_pending(src_chat_id);
            seq_tracker_set_backfill_done(src_chat_id, 1);
            seq_tracker_set_scan_from(src_chat_id, 0);
            seq_tracker_save();
        }
        cJSON_Delete(root); return;
    }

    if (enable_sequential_forwarding)
        { char st[64]; snprintf(st, sizeof(st), "History: %d msgs from %lld", total, src_chat_id); set_status(st); }

    long long cutoff = history_window_hours > 0
        ? (long long)time(NULL) - history_window_hours * 3600 : 0;

    long long batch_oldest_id = 0;
    int i = 0;
    int new_count = 0;
    int cutoff_filtered = 0;

    int bd = enable_sequential_forwarding && seq_tracker_is_backfill_done(src_chat_id);

    if (!bd && enable_sequential_forwarding) {
        if (seq_tracker_get_scan_from(src_chat_id) == 0) {
            int found = 0;
            for (int j = 0; j < seq_tracker_count; j++)
                if (seq_tracker[j].chat_id == src_chat_id) { found = 1; break; }
            if (!found && seq_tracker_count < MAX_SEQ_TRACKER) {
                seq_tracker[seq_tracker_count].chat_id = src_chat_id;
                seq_tracker[seq_tracker_count].last_msg_id = 0;
                seq_tracker[seq_tracker_count].last_date = 0;
                seq_tracker[seq_tracker_count].scan_from_id = 0;
                seq_tracker[seq_tracker_count].backfill_done = 0;
                seq_tracker[seq_tracker_count].pending = NULL;
                seq_tracker[seq_tracker_count].pending_count = 0;
                seq_tracker[seq_tracker_count].pending_cap = 0;
                seq_tracker_count++;
            }
        }
        for (const cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
            long long msg_id = cJSON_GetInt64(m, "id");
            batch_oldest_id = msg_id;
            long long album_id = cJSON_GetInt64(m, "media_album_id");
            long long msg_date = cJSON_GetInt64(m, "date");
            if (msg_id == 0) continue;
            if (seq_tracker_get_scan_from(src_chat_id) == msg_id) continue;
            if (cutoff && msg_date < cutoff) { cutoff_filtered++; continue; }
            PendingMsg pm;
            pm.id = msg_id; pm.album_id = album_id; pm.date = msg_date; pm.reply_to_msg_id = 0;
            const cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
            if (reply_to) pm.reply_to_msg_id = cJSON_GetInt64(reply_to, "message_id");
            seq_tracker_pending_append(src_chat_id, pm);
            new_count++;
        }

        { char st[64]; snprintf(st, sizeof(st), "Backfill: %d msgs, cutoff=%lld", new_count, cutoff); set_status(st); }

        int complete = 0;
        if (cutoff && cutoff_filtered > 0 && new_count == 0) complete = 1;
        if (new_count == 0 && total > 0 && cutoff_filtered == 0) complete = 1;

        if (complete) {
            int has_pending = 0;
            for (int j = 0; j < seq_tracker_count; j++)
                if (seq_tracker[j].chat_id == src_chat_id && seq_tracker[j].pending_count > 0)
                    { has_pending = 1; break; }
            if (has_pending) {
                flush_pending(src_chat_id);
                seq_tracker_set_backfill_done(src_chat_id, 1);
                seq_tracker_set_scan_from(src_chat_id, 0);
            } else if (cutoff_filtered > 0) {
                seq_tracker_set_backfill_done(src_chat_id, 1);
                seq_tracker_set_scan_from(src_chat_id, 0);
            } else {
                seq_tracker_set_scan_from(src_chat_id, 0);
            }
            seq_tracker_save();
            cJSON_Delete(root); return;
        }

        if (new_count > 0 && batch_oldest_id) {
            seq_tracker_set_scan_from(src_chat_id, batch_oldest_id);
            seq_tracker_save();
        } else if (new_count == 0) {
            seq_tracker_save();
        }

        cJSON_Delete(root); return;
    }

    PendingMsg *new_msgs = calloc(total, sizeof(PendingMsg));
    if (!new_msgs) { set_status("OOM in history handler"); cJSON_Delete(root); return; }
    new_count = 0;
    cutoff_filtered = 0;
    i = 0;
    for (const cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
        long long msg_id   = cJSON_GetInt64(m, "id");
        batch_oldest_id = msg_id;
        long long album_id = cJSON_GetInt64(m, "media_album_id");
        long long chat_id  = cJSON_GetInt64(m, "chat_id");
        long long msg_date = cJSON_GetInt64(m, "date");
        if (msg_id == 0) continue;
        if (history_has(chat_id, msg_id)) continue;
        if (cutoff && msg_date < cutoff) continue;
        new_msgs[new_count].id       = msg_id;
        new_msgs[new_count].album_id = album_id;
        new_msgs[new_count].date     = msg_date;
        const cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
        if (reply_to) new_msgs[new_count].reply_to_msg_id = cJSON_GetInt64(reply_to, "message_id");
        new_count++;
    }

    if (enable_sequential_forwarding)
        { char st[64]; snprintf(st, sizeof(st), "New: %d msgs, cutoff=%lld", new_count, cutoff); set_status(st); }

    if (new_count == 0) { free(new_msgs); cJSON_Delete(root); return; }

    process_msgs(src_chat_id, new_msgs, new_count);
    free(new_msgs);
    cJSON_Delete(root);
}

static void poll_channels(void *client) {
    for (int i = 0; i < num_sources; i++) {
        long long chat_id = source_chat_ids[i];
        if (chat_id == 0) continue;
        char extra[64];
        snprintf(extra, sizeof(extra), "history_%lld", chat_id);
        long long from_id = enable_sequential_forwarding ? seq_tracker_get_scan_from(chat_id) : 0;
        char payload[512];
        snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"limit\":%d,\"from_message_id\":%lld,\"offset\":0",
                 chat_id, MSG_LIMIT, from_id);
        send_req(client, "getChatHistory", payload, extra);
    }
}

static void on_update(void *client, const char *json) {
    if (strstr(json, "\"@type\":\"updateAuthorizationState\"")) on_auth_state(client, json);
}

static void on_error(void *client, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const char *extra = cJSON_GetStr(root, "@extra");
    const char *msg   = cJSON_GetStr(root, "message");
    int code          = (int)cJSON_GetInt64(root, "code");
    { char b[128]; snprintf(b, sizeof(b), "ERROR [%s] code=%d: %s", extra ? extra : "?", code, msg ? msg : "?"); set_status(b); }
    if (extra && strncmp(extra, "resolve_", 8) == 0 && strcmp(extra, "resolve_dest") != 0)
        resolve_next(client);
    cJSON_Delete(root);
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    long nread = fread(buf, 1, len, f);
    if (nread <= 0) { free(buf); fclose(f); return -1; }
    buf[nread] = 0;
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { fprintf(stderr, "Invalid JSON in %s\n", path); return -1; }

    cJSON *v;
    v = cJSON_GetObjectItem(root, "api_id");
    if (!v || !cJSON_IsNumber(v)) { fprintf(stderr, "Missing api_id\n"); cJSON_Delete(root); return -1; }
    api_id = (int)v->valuedouble;

    v = cJSON_GetObjectItem(root, "api_hash");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing api_hash\n"); cJSON_Delete(root); return -1; }
    api_hash = strdup(v->valuestring);
    if (!api_hash) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "dest_channel");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing dest_channel\n"); cJSON_Delete(root); return -1; }
    dest_channel = strdup(v->valuestring);
    if (!dest_channel) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "history_file");
    history_file = v && cJSON_IsString(v) ? strdup(v->valuestring) : strdup("history.txt");
    if (!history_file) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "forward_delay_sec");
    forward_delay_sec = v && cJSON_IsNumber(v) ? (int)v->valuedouble : 1;

    v = cJSON_GetObjectItem(root, "history_window_hours");
    if (v && cJSON_IsNumber(v)) history_window_hours = (int)v->valuedouble;

    v = cJSON_GetObjectItem(root, "enable_sequential_forwarding");
    enable_sequential_forwarding = v && cJSON_IsBool(v) ? v->valueint : 0;

    v = cJSON_GetObjectItem(root, "sequence_direction");
    if (v && cJSON_IsString(v) && strcmp(v->valuestring, "desc") == 0)
        sequence_direction = 1;
    else
        sequence_direction = 0;

    v = cJSON_GetObjectItem(root, "source_channels");
    if (!v || !cJSON_IsArray(v)) { fprintf(stderr, "Missing source_channels array\n"); cJSON_Delete(root); return -1; }
    num_sources = cJSON_GetArraySize(v);
    source_channels = calloc(num_sources, sizeof(char *));
    if (!source_channels) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }
    for (int i = 0; i < num_sources; i++) {
        const cJSON *elem = cJSON_GetArrayItem(v, i);
        source_channels[i] = elem && cJSON_IsString(elem) ? strdup(elem->valuestring) : strdup("");
        if (!source_channels[i]) { source_channels[i] = strdup(""); }
    }

    cJSON_Delete(root);
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (load_config("config.json") != 0) return 1;

    void *client = td_json_client_create();
    if (!client) { fprintf(stderr, "Failed to create TDLib client\n"); return 1; }
    td_json_client_send(client, "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":0}");

    source_chat_ids = calloc(num_sources, sizeof(long long));
    if (!source_chat_ids) { td_json_client_destroy(client); return 1; }

    history_load();
    if (enable_sequential_forwarding) seq_tracker_load();
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    td_json_client_send(client, "{\"@type\":\"getAuthorizationState\"}");

    time_t last_poll = 0;

    while (keep_running) {
        const char *result = td_json_client_receive(client, 1.0);
        if (result) {
            cJSON *root = cJSON_Parse(result);
            if (root) {
                const char *type  = cJSON_GetStr(root, "@type");
                const char *extra = cJSON_GetStr(root, "@extra");
                if (!type) type = "";
                if (strcmp(type, "updateAuthorizationState") == 0)
                    on_auth_state(client, result);
                else if (strcmp(type, "error") == 0)
                    on_error(client, result);
                else if (extra)
                    on_response(client, result, extra, argv);
                else
                    on_update(client, result);
                cJSON_Delete(root);

            }
        }

        if (authorized && dest_resolved && source_count >= 1 && !resolve_active) {
            time_t now = time(NULL);
            if (now - last_poll >= POLL_INTERVAL) {
                last_poll = now;
                poll_channels(client);
            }
        }
        if (argv[1] == NULL && authorized) {
#ifndef TGF_NOGUI
            static int ncurses_inited = 0;
            if (!ncurses_inited) {
                ncurses_init();
                atexit(ncurses_cleanup);
                ncurses_inited = 1;
            }
            static time_t last_dashboard_update = 0;
            time_t current_time = time(NULL);
            if (current_time - last_dashboard_update >= 1) {
                print_dashboard(dest_channel, src_name, source_count, forward_delay_sec);
                update_action_msg(src_name, dest_channel);
                last_dashboard_update = current_time;
            }
#endif
        }
        process_fwd_queue(client);
        { static time_t last_prune = 0; time_t now = time(NULL);
          if (now - last_prune >= 1200) { history_prune(); last_prune = now; } }
    }

    for (int i = 0; i < history_set_size; i++) free(history_set[i]);
    free(history_set);
    free(history_time);
    free(fwd_queue);
    free(source_chat_ids);
    for (int i = 0; i < num_sources; i++) free(source_channels[i]);
    free(source_channels);
    free(api_hash);
    free(dest_channel);
    free(history_file);
    td_json_client_destroy(client);
    return 0;
}
