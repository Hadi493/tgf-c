#define TGF_IMPLEMENTATION
#include "nob.h"

static void fwd_queue_push(long long src_chat_id, const long long *ids, int count) {
    if (fwd_queue_count >= 1024) { fprintf(stderr, "Queue full\n"); return; }
    ForwardJob *j = &fwd_queue[fwd_queue_count++];
    j->src_chat_id = src_chat_id;
    j->count = count < MAX_FWD_IDS ? count : MAX_FWD_IDS;
    for (int i = 0; i < j->count; i++) j->ids[i] = ids[i];
}

static void send_req(void *client, const char *type, const char *payload, const char *extra) {
    char buf[8192];
    int n;
    if (payload && payload[0])
        n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload, extra);
    else
        n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",\"@extra\":\"%s\"}", type, extra);
    if (n >= (int)sizeof(buf)) fprintf(stderr, "WARNING: request truncated (%d)\n", n);
    td_json_client_send(client, buf);
    pending_req++;
}

static long long cJSON_GetInt64(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) return 0;
    if (cJSON_IsNumber(v)) return (long long)v->valuedouble;
    if (cJSON_IsString(v)) return atoll(v->valuestring);
    return 0;
}

static const char *cJSON_GetStr(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static void history_load(void) {
    history_keys = calloc(50000, sizeof(char *));
    if (!history_keys) return;
    FILE *f = fopen(history_file, "r");
    if (!f) return;
    char line[128];
    while (history_count < 50000 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) history_keys[history_count++] = strdup(line);
    }
    fclose(f);
}

static int history_has(long long chat_id, long long msg_id) {
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    for (int i = 0; i < history_count; i++)
        if (strcmp(history_keys[i], key) == 0) return 1;
    return 0;
}

static void history_add(long long chat_id, long long msg_id) {
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    if (history_has(chat_id, msg_id)) return;
    if (history_count >= 50000) return;
    history_keys[history_count++] = strdup(key);
    FILE *f = fopen(history_file, "a");
    if (f) { fprintf(f, "%s\n", key); fclose(f); }
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
        printf("Phone (+xxx): "); fflush(stdout);
        if (fgets(phone, sizeof(phone), stdin)) phone[strcspn(phone, "\n")] = 0;
        char payload[256];
        snprintf(payload, sizeof(payload), "\"phone_number\":\"%s\"", phone);
        send_req(client, "setAuthenticationPhoneNumber", payload, "auth_phone");
    } else if (strstr(json, "authorizationStateWaitCode")) {
        char code[32] = {0};
        printf("Code: "); fflush(stdout);
        if (fgets(code, sizeof(code), stdin)) code[strcspn(code, "\n")] = 0;
        char payload[256];
        snprintf(payload, sizeof(payload), "\"code\":\"%s\"", code);
        send_req(client, "checkAuthenticationCode", payload, "auth_code");
    } else if (strstr(json, "authorizationStateWaitPassword")) {
        char pw[128] = {0};
        printf("2FA password: "); fflush(stdout);
        if (fgets(pw, sizeof(pw), stdin)) pw[strcspn(pw, "\n")] = 0;
        char payload[256];
        snprintf(payload, sizeof(payload), "\"password\":\"%s\"", pw);
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

static int resolve_index = 0;
static int resolve_active = 0;

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
        cJSON *type_item = cJSON_GetObjectItem(root, "@type");
        if (type_item && strcmp(type_item->valuestring, "error") == 0) {
            cJSON_Delete(root);
            return;
        }
        long long chat_id = cJSON_GetInt64(root, "id");
        const char *title = cJSON_GetStr(root, "title");
        char user_name[128] = "";
        if (!title) {
            const char *fn = cJSON_GetStr(root, "first_name");
            const char *ln = cJSON_GetStr(root, "last_name");
            if (fn) {
                snprintf(user_name, sizeof(user_name), "%s %s", fn, ln ? ln : "");
                title = user_name;
            } else { title = "(unknown)"; }
        }
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
        cJSON *type_item = cJSON_GetObjectItem(root, "@type");
        if (type_item && strcmp(type_item->valuestring, "error") == 0) {
            cJSON *m = cJSON_GetObjectItem(root, "message");
            fprintf(stderr, " Forward fail [%s]: %s\n", extra,
                    m ? m->valuestring : "?");
        } else {
            long long src_chat_id = 0;
            if (extra[4]) {
                const char *p = extra + 4;
                src_chat_id = atoll(p);
            }
            const char *src_name = "unknown";
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
    if (!root) { fprintf(stderr, "  ✗ Failed to parse history JSON\n"); return; }

    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    if (!msgs || !cJSON_IsArray(msgs)) { cJSON_Delete(root); return; }

    int total = cJSON_GetArraySize(msgs);
    if (total == 0) { cJSON_Delete(root); return; }

    typedef struct {
        long long id, album_id, reply_to_msg_id;
    } MsgInfo;
    MsgInfo *new_msgs = calloc(total, sizeof(MsgInfo));
    int new_count = 0;
    long long cutoff = history_window_hours > 0
        ? (long long)time(NULL) - history_window_hours * 3600 : 0;

    for (int i = 0; i < total; i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        if (!m) continue;
        long long msg_id   = cJSON_GetInt64(m, "id");
        long long album_id = cJSON_GetInt64(m, "media_album_id");
        long long chat_id  = cJSON_GetInt64(m, "chat_id");
        long long msg_date = cJSON_GetInt64(m, "date");
        if (msg_id == 0) continue;
        if (history_has(chat_id, msg_id)) continue;
        if (cutoff && msg_date < cutoff) continue;
        new_msgs[new_count].id       = msg_id;
        new_msgs[new_count].album_id = album_id;
        cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
        if (reply_to) new_msgs[new_count].reply_to_msg_id = cJSON_GetInt64(reply_to, "message_id");
        new_count++;
    }

    if (new_count == 0) { free(new_msgs); cJSON_Delete(root); return; }
    // printf("  [%lld] %d new messages\n", src_chat_id, new_count);

    int *grouped = calloc(new_count, sizeof(int));

    for (int i = 0; i < new_count; i++) {
        if (grouped[i]) continue;

        if (new_msgs[i].album_id == 0) {
            long long ids[256];
            int id_count = 0;

            if (new_msgs[i].reply_to_msg_id) {
                ids[id_count++] = new_msgs[i].reply_to_msg_id;
            }
            ids[id_count++] = new_msgs[i].id;

            for (int k = 0; k < new_count; k++) {
                if (!grouped[k] && k != i &&
                    new_msgs[k].reply_to_msg_id == new_msgs[i].id && id_count < 256) {
                    ids[id_count++] = new_msgs[k].id;
                }
            }

            for (int a = 0; a < id_count; a++)
                for (int b = a + 1; b < id_count; b++)
                    if (ids[a] > ids[b]) {
                        long long tmp = ids[a]; ids[a] = ids[b]; ids[b] = tmp;
                    }

            fwd_queue_push(src_chat_id, ids, id_count);
            // if (id_count > 1) printf("  → Queued %d messages (reply chain)\n", id_count);

            history_add(src_chat_id, new_msgs[i].id);
            grouped[i] = 1;
            for (int k = 0; k < new_count; k++) {
                if (!grouped[k] && k != i && new_msgs[k].reply_to_msg_id == new_msgs[i].id) {
                    history_add(src_chat_id, new_msgs[k].id);
                    grouped[k] = 1;
                }
            }

        } else {
            long long album_id = new_msgs[i].album_id;
            int album_indices[256];
            int album_count = 0;
            for (int j = 0; j < new_count; j++)
                if (!grouped[j] && new_msgs[j].album_id == album_id) album_indices[album_count++] = j;

            for (int a = 0; a < album_count; a++)
                for (int b = a + 1; b < album_count; b++)
                    if (new_msgs[album_indices[a]].id > new_msgs[album_indices[b]].id) {
                        int tmp = album_indices[a];
                        album_indices[a] = album_indices[b];
                        album_indices[b] = tmp;
                    }

            long long album_ids[256];
            for (int b = 0; b < album_count; b++) album_ids[b] = new_msgs[album_indices[b]].id;
            fwd_queue_push(src_chat_id, album_ids, album_count);
            // printf("  → Queued album (%d msgs)\n", album_count);

            for (int b = 0; b < album_count; b++) {
                history_add(src_chat_id, new_msgs[album_indices[b]].id);
                grouped[album_indices[b]] = 1;
            }
        }
    }

    free(grouped);
    free(new_msgs);
    cJSON_Delete(root);
}

static void poll_channels(void *client) {
    for (int i = 0; i < num_sources; i++) {
        long long chat_id = source_chat_ids[i];
        if (chat_id == 0) continue;
        char extra[64];
        snprintf(extra, sizeof(extra), "history_%lld", chat_id);
        char payload[512];
        snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"limit\":%d,\"from_message_id\":0,\"offset\":0", chat_id, MSG_LIMIT);
        send_req(client, "getChatHistory", payload, extra);
    }
}

static int process_fwd_queue(void *client) {
    if (fwd_queue_count == 0) return 0;
    double now = (double)time(NULL);
    if (now - fwd_last_time < forward_delay_sec) return 0;

    ForwardJob *j = &fwd_queue[0];
    char ids_str[8192] = "";
    int pos = 0;
    for (int i = 0; i < j->count; i++)
        APPEND_ID(ids_str, pos, sizeof(ids_str), j->ids[i]);

    char extra[64];
    if (j->count == 1)
        snprintf(extra, sizeof(extra), "fwd_%lld_%lld", j->src_chat_id, j->ids[0]);
    else
        snprintf(extra, sizeof(extra), "fwd_%lld_batch", j->src_chat_id);

    char payload[16384];
    snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"from_chat_id\":%lld,\"message_ids\":[%s]", dest_chat_id, j->src_chat_id, ids_str);
    send_req(client, "forwardMessages", payload, extra);

    fwd_queue_count--;
    for (int i = 0; i < fwd_queue_count; i++) fwd_queue[i] = fwd_queue[i + 1];
    fwd_last_time = now;
    return 1;
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
    fprintf(stderr, "✗ ERROR [extra=%s] code=%d: %s\n",
            extra ? extra : "(none)", code, msg ? msg : "?");
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
    fread(buf, 1, len, f);
    buf[len] = 0;
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

    v = cJSON_GetObjectItem(root, "dest_channel");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing dest_channel\n"); cJSON_Delete(root); return -1; }
    dest_channel = strdup(v->valuestring);

    v = cJSON_GetObjectItem(root, "history_file");
    history_file = v && cJSON_IsString(v) ? strdup(v->valuestring) : strdup("history.txt");

    v = cJSON_GetObjectItem(root, "forward_delay_sec");
    forward_delay_sec = v && cJSON_IsNumber(v) ? (int)v->valuedouble : 1;

    v = cJSON_GetObjectItem(root, "history_window_hours");
    if (v && cJSON_IsNumber(v)) history_window_hours = (int)v->valuedouble;

    v = cJSON_GetObjectItem(root, "source_channels");
    if (!v || !cJSON_IsArray(v)) { fprintf(stderr, "Missing source_channels array\n"); cJSON_Delete(root); return -1; }
    num_sources = cJSON_GetArraySize(v);
    source_channels = calloc(num_sources, sizeof(char *));
    for (int i = 0; i < num_sources; i++) {
        cJSON *elem = cJSON_GetArrayItem(v, i);
        source_channels[i] = elem && cJSON_IsString(elem) ? strdup(elem->valuestring) : strdup("");
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
        if (argv[1] == NULL) {
            static time_t last_bashboard_update = 0;
            time_t current_time = time(NULL);

            if (current_time - last_bashboard_update >= 1) {
                print_dashboard(dest_channel, source_count, forward_delay_sec);
                last_bashboard_update = current_time;
            }
        }
        process_fwd_queue(client);
    }

    for (int i = 0; i < history_count; i++) free(history_keys[i]);
    free(history_keys);
    free(source_chat_ids);
    for (int i = 0; i < num_sources; i++) free(source_channels[i]);
    free(source_channels);
    free(api_hash);
    free(dest_channel);
    free(history_file);
    td_json_client_destroy(client);
    return 0;
}
