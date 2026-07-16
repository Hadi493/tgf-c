#define TGF_IMPLEMENTATION
#include "nob.h"
#include <signal.h>

static void sig_handler(int sig) {
    (void)sig;
    keep_running = false;
}

static void fwd_queue_push(long long src_chat_id, const long long *ids, int count) {
    if (fwd_queue_count >= 1024) { char b[64]; snprintf(b, sizeof(b), "Queue full: %lld msgs dropped", src_chat_id); set_status(b); return; }
    ForwardJob *j = &fwd_queue[fwd_queue_count++];
    j->src_chat_id = src_chat_id;
    j->count = count < MAX_FWD_IDS ? count : MAX_FWD_IDS;
    for (int i = 0; i < j->count; i++) j->ids[i] = ids[i];
}

#define MAX_PENDING_FWD 64
static struct {
    long long src_chat_id;
    long long ids[MAX_FWD_IDS];
    int count;
    long long seq;
    int in_use;
} pending_fwd[MAX_PENDING_FWD];
static int pending_fwd_count = 0;
static long long fwd_seq = 0;

static void send_req(void *client, const char *type, const char *payload, const char *extra) {
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

static unsigned int history_hash(const char *key) {
    unsigned int h = 2166136261u;
    while (*key) {h ^= (unsigned char)*key++; h *= 16777619u; }
    return h & (HISTORY_SET_SIZE - 1);
}

static void history_load(void) {
    FILE *f = fopen(history_file, "r");
    if (!f) return;
    char line[128];
    while (history_count < 50000 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        unsigned int idx = history_hash(line);
        while (history_set[idx]) {
            if (strcmp(history_set[idx], line) == 0) break;
            idx = (idx + 1) & (HISTORY_SET_SIZE - 1);
        }
        if (history_set[idx]) continue;
        history_set[idx] = strdup(line);
        if (!history_set[idx]) break;
        history_count++;
    }
    fclose(f);
}

static int history_has(long long chat_id, long long msg_id) {
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key);
    while (history_set[idx]) {
        if (strcmp(history_set[idx], key) == 0) return 1;
        idx = (idx + 1) & (HISTORY_SET_SIZE - 1);
    }
    return 0;
}

static void history_add(long long chat_id, long long msg_id) {
    if (history_count >= 50000) return;
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key);
    while (history_set[idx]) {
        if (strcmp(history_set[idx], key) == 0) return;
        idx = (idx + 1) & (HISTORY_SET_SIZE - 1);
    }
    history_set[idx] = strdup(key);
    if (!history_set[idx]) return;
    history_count++;
    FILE *f = fopen(history_file, "a");
    if (f) { fprintf(f, "%s\n", key); fclose(f); }
}

static void seq_tracker_load(void) {
    FILE *f = fopen("seq_tracker.txt", "r");
    if (!f) return;
    char line[128];
    while (seq_tracker_count < MAX_SEQ_TRACKER && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        long long chat_id, msg_id, date, scan_id, done;
        int n = sscanf(line, "%lld %lld %lld %lld %lld", &chat_id, &msg_id, &date, &scan_id, &done);
        if (n >= 3) {
            seq_tracker[seq_tracker_count].chat_id = chat_id;
            seq_tracker[seq_tracker_count].last_msg_id = msg_id;
            seq_tracker[seq_tracker_count].last_date = date;
            seq_tracker[seq_tracker_count].scan_from_id = (n >= 4) ? scan_id : 0;
            seq_tracker[seq_tracker_count].backfill_done = (n >= 5) ? (int)done : 0;
            seq_tracker_count++;
        }
    }
    fclose(f);
}

static void seq_tracker_save(void) {
    FILE *f = fopen("seq_tracker.txt.tmp", "w");
    if (!f) return;
    for (int i = 0; i < seq_tracker_count; i++)
        fprintf(f, "%lld %lld %lld %lld %d\n", seq_tracker[i].chat_id, seq_tracker[i].last_msg_id, seq_tracker[i].last_date, seq_tracker[i].scan_from_id, seq_tracker[i].backfill_done);
    fclose(f);
    rename("seq_tracker.txt.tmp", "seq_tracker.txt");
}

static void seq_tracker_set(long long chat_id, long long msg_id, long long date) {
    for (int i = 0; i < seq_tracker_count; i++) {
        if (seq_tracker[i].chat_id == chat_id) {
            if (msg_id > seq_tracker[i].last_msg_id) seq_tracker[i].last_msg_id = msg_id;
            if (date > seq_tracker[i].last_date) seq_tracker[i].last_date = date;
            return;
        }
    }
    if (seq_tracker_count < MAX_SEQ_TRACKER) {
        seq_tracker[seq_tracker_count].chat_id = chat_id;
        seq_tracker[seq_tracker_count].last_msg_id = msg_id;
        seq_tracker[seq_tracker_count].last_date = date;
        seq_tracker[seq_tracker_count].scan_from_id = 0;
        seq_tracker[seq_tracker_count].pending = NULL;
        seq_tracker[seq_tracker_count].pending_count = 0;
        seq_tracker[seq_tracker_count].pending_cap = 0;
        seq_tracker_count++;
    }
}

static long long seq_tracker_get_scan_from(long long chat_id) {
    for (int i = 0; i < seq_tracker_count; i++)
        if (seq_tracker[i].chat_id == chat_id)
            return seq_tracker[i].scan_from_id;
    return 0;
}

static void seq_tracker_set_scan_from(long long chat_id, long long msg_id) {
    for (int i = 0; i < seq_tracker_count; i++) {
        if (seq_tracker[i].chat_id == chat_id) {
            seq_tracker[i].scan_from_id = msg_id;
            return;
        }
    }
    if (seq_tracker_count < MAX_SEQ_TRACKER) {
        seq_tracker[seq_tracker_count].chat_id = chat_id;
        seq_tracker[seq_tracker_count].last_msg_id = 0;
        seq_tracker[seq_tracker_count].last_date = 0;
        seq_tracker[seq_tracker_count].scan_from_id = msg_id;
        seq_tracker[seq_tracker_count].backfill_done = 0;
        seq_tracker[seq_tracker_count].pending = NULL;
        seq_tracker[seq_tracker_count].pending_count = 0;
        seq_tracker[seq_tracker_count].pending_cap = 0;
        seq_tracker_count++;
    }
}

static int seq_tracker_is_backfill_done(long long chat_id) {
    for (int i = 0; i < seq_tracker_count; i++)
        if (seq_tracker[i].chat_id == chat_id)
            return seq_tracker[i].backfill_done;
    return 0;
}

static void seq_tracker_set_backfill_done(long long chat_id, int done) {
    for (int i = 0; i < seq_tracker_count; i++) {
        if (seq_tracker[i].chat_id == chat_id) {
            seq_tracker[i].backfill_done = done;
            return;
        }
    }
    if (seq_tracker_count < MAX_SEQ_TRACKER) {
        seq_tracker[seq_tracker_count].chat_id = chat_id;
        seq_tracker[seq_tracker_count].last_msg_id = 0;
        seq_tracker[seq_tracker_count].last_date = 0;
        seq_tracker[seq_tracker_count].scan_from_id = 0;
        seq_tracker[seq_tracker_count].backfill_done = done;
        seq_tracker[seq_tracker_count].pending = NULL;
        seq_tracker[seq_tracker_count].pending_count = 0;
        seq_tracker[seq_tracker_count].pending_cap = 0;
        seq_tracker_count++;
    }
}

static void seq_tracker_pending_append(long long chat_id, PendingMsg msg) {
    for (int i = 0; i < seq_tracker_count; i++) {
        if (seq_tracker[i].chat_id == chat_id) {
            int n = seq_tracker[i].pending_count;
            int cap = seq_tracker[i].pending_cap;
            if (n >= cap) {
                cap = cap ? cap * 2 : 512;
                PendingMsg *np = realloc(seq_tracker[i].pending, cap * sizeof(PendingMsg));
                if (!np) { set_status("OOM in pending_append"); return; }
                seq_tracker[i].pending = np;
                seq_tracker[i].pending_cap = cap;
            }
            seq_tracker[i].pending[n] = msg;
            seq_tracker[i].pending_count = n + 1;
            return;
        }
    }
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
        if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "error") == 0) {
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
        int is_error = (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "error") == 0);
        if (is_error) {
            cJSON *m = cJSON_GetObjectItem(root, "message");
            { char b[128]; snprintf(b, sizeof(b), "Forward fail [%s]: %s", extra, m ? m->valuestring : "?"); set_status(b); }
        }
        long long src_chat_id = 0;
        long long msg_id = 0;
        long long batch_seq = 0;
        int is_batch = 0;
        if (extra[4]) {
            const char *p = extra + 4;
            const char *sep = strchr(p, '_');
            if (sep) {
                char tmp[64];
                size_t len = (size_t)(sep - p);
                if (len < sizeof(tmp)) {
                    memcpy(tmp, p, len); tmp[len] = 0;
                    src_chat_id = atoll(tmp);
                }
                if (strncmp(sep + 1, "batch_", 6) == 0) {
                    is_batch = 1;
                    batch_seq = atoll(sep + 7);
                } else {
                    msg_id = atoll(sep + 1);
                }
            } else {
                src_chat_id = atoll(p);
            }
        }
        for (int i = 0; i < MAX_PENDING_FWD; i++) {
            if (!pending_fwd[i].in_use) continue;
            int match = 0;
            if (is_batch) {
                if (pending_fwd[i].src_chat_id == src_chat_id && pending_fwd[i].seq == batch_seq) match = 1;
            } else if (msg_id) {
                if (pending_fwd[i].src_chat_id == src_chat_id
                    && pending_fwd[i].count == 1 && pending_fwd[i].ids[0] == msg_id) match = 1;
            }
            if (match) {
                if (!is_error) {
                    for (int j = 0; j < pending_fwd[i].count; j++)
                        history_add(src_chat_id, pending_fwd[i].ids[j]);
                }
                pending_fwd[i].in_use = 0;
                pending_fwd_count--;
                break;
            }
        }
        if (!is_error) {
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

typedef PendingMsg MsgInfo;

static int seq_cmp_asc(const void *a, const void *b) {
    const MsgInfo *ma = (const MsgInfo *)a, *mb = (const MsgInfo *)b;
    if (ma->date != mb->date) return ma->date > mb->date ? 1 : -1;
    if (ma->id  != mb->id)   return ma->id  > mb->id   ? 1 : -1;
    return 0;
}

static int seq_cmp_desc(const void *a, const void *b) {
    const MsgInfo *ma = (const MsgInfo *)a, *mb = (const MsgInfo *)b;
    if (ma->date != mb->date) return ma->date < mb->date ? 1 : -1;
    if (ma->id  != mb->id)   return ma->id  < mb->id   ? 1 : -1;
    return 0;
}

static void process_msgs(long long src_chat_id, MsgInfo *msgs, int count) {
    if (count == 0) return;
    if (enable_sequential_forwarding && count > 1) {
        int asc = (sequence_direction == 0);
        qsort(msgs, count, sizeof(MsgInfo), asc ? seq_cmp_asc : seq_cmp_desc);
    }
    int *grouped = calloc(count, sizeof(int));
    for (int i = 0; i < count; i++) {
        if (grouped[i]) continue;
        if (msgs[i].album_id == 0) {
            long long ids[256];
            int id_count = 0;
            if (msgs[i].reply_to_msg_id) ids[id_count++] = msgs[i].reply_to_msg_id;
            ids[id_count++] = msgs[i].id;
            for (int k = 0; k < count; k++) {
                if (!grouped[k] && k != i && msgs[k].reply_to_msg_id == msgs[i].id && id_count < 256) {
                    ids[id_count++] = msgs[k].id;
                }
            }
            for (int a = 0; a < id_count; a++)
                for (int b = a + 1; b < id_count; b++)
                    if (ids[a] > ids[b]) { long long tmp = ids[a]; ids[a] = ids[b]; ids[b] = tmp; }
            fwd_queue_push(src_chat_id, ids, id_count);
            if (enable_sequential_forwarding) seq_tracker_set(src_chat_id, msgs[i].id, msgs[i].date);
            grouped[i] = 1;
            for (int k = 0; k < count; k++) {
                if (!grouped[k] && k != i && msgs[k].reply_to_msg_id == msgs[i].id) {
                    if (enable_sequential_forwarding) seq_tracker_set(src_chat_id, msgs[k].id, msgs[k].date);
                    grouped[k] = 1;
                }
            }
        } else {
            long long album_id = msgs[i].album_id;
            int album_indices[256];
            int album_count = 0;
            for (int j = 0; j < count; j++)
                if (!grouped[j] && msgs[j].album_id == album_id) album_indices[album_count++] = j;
            for (int a = 0; a < album_count; a++)
                for (int b = a + 1; b < album_count; b++)
                    if (msgs[album_indices[a]].id > msgs[album_indices[b]].id) {
                        int tmp = album_indices[a]; album_indices[a] = album_indices[b]; album_indices[b] = tmp;
                    }
            long long album_ids[256];
            for (int b = 0; b < album_count; b++) album_ids[b] = msgs[album_indices[b]].id;
            fwd_queue_push(src_chat_id, album_ids, album_count);
            for (int b = 0; b < album_count; b++) {
                if (enable_sequential_forwarding) seq_tracker_set(src_chat_id, msgs[album_indices[b]].id, msgs[album_indices[b]].date);
                grouped[album_indices[b]] = 1;
            }
        }
    }
    free(grouped);
    if (enable_sequential_forwarding) seq_tracker_save();
}

static void flush_pending(long long src_chat_id) {
    for (int i = 0; i < seq_tracker_count; i++) {
        if (seq_tracker[i].chat_id == src_chat_id) {
            if (seq_tracker[i].pending_count > 0)
                process_msgs(src_chat_id, seq_tracker[i].pending, seq_tracker[i].pending_count);
            free(seq_tracker[i].pending);
            seq_tracker[i].pending = NULL;
            seq_tracker[i].pending_count = 0;
            seq_tracker[i].pending_cap = 0;
            return;
        }
    }
}

static void handle_history_response(void *client, const char *json, long long src_chat_id) {
    (void)client;
    cJSON *root = cJSON_Parse(json);
    if (!root) { set_status("Failed to parse history JSON"); return; }

    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
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
        for (cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
            long long msg_id = cJSON_GetInt64(m, "id");
            batch_oldest_id = msg_id;
            long long album_id = cJSON_GetInt64(m, "media_album_id");
            long long msg_date = cJSON_GetInt64(m, "date");
            if (msg_id == 0) continue;
            if (seq_tracker_get_scan_from(src_chat_id) == msg_id) continue;
            if (cutoff && msg_date < cutoff) { cutoff_filtered++; continue; }
            PendingMsg pm;
            pm.id = msg_id; pm.album_id = album_id; pm.date = msg_date; pm.reply_to_msg_id = 0;
            cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
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

    MsgInfo *new_msgs = calloc(total, sizeof(MsgInfo));
    new_count = 0;
    cutoff_filtered = 0;
    i = 0;
    for (cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
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
        cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
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
    long long my_seq = fwd_seq++;
    if (j->count == 1)
        snprintf(extra, sizeof(extra), "fwd_%lld_%lld", j->src_chat_id, j->ids[0]);
    else
        snprintf(extra, sizeof(extra), "fwd_%lld_batch_%lld", j->src_chat_id, my_seq);

    char payload[16384];
    snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"from_chat_id\":%lld,\"message_ids\":[%s]", dest_chat_id, j->src_chat_id, ids_str);
    send_req(client, "forwardMessages", payload, extra);

    if (pending_fwd_count < MAX_PENDING_FWD) {
        int slot = -1;
        for (int i = 0; i < MAX_PENDING_FWD; i++) {
            if (!pending_fwd[i].in_use) { slot = i; break; }
        }
        if (slot >= 0) {
            pending_fwd[slot].src_chat_id = j->src_chat_id;
            pending_fwd[slot].count = j->count;
            pending_fwd[slot].seq = my_seq;
            for (int i = 0; i < j->count; i++) pending_fwd[slot].ids[i] = j->ids[i];
            pending_fwd[slot].in_use = 1;
            pending_fwd_count++;
        }
    }

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

    v = cJSON_GetObjectItem(root, "dest_channel");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing dest_channel\n"); cJSON_Delete(root); return -1; }
    dest_channel = strdup(v->valuestring);

    v = cJSON_GetObjectItem(root, "history_file");
    history_file = v && cJSON_IsString(v) ? strdup(v->valuestring) : strdup("history.txt");

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
    }

    for (int i = 0; i < HISTORY_SET_SIZE; i++) free(history_set[i]);
    free(source_chat_ids);
    for (int i = 0; i < num_sources; i++) free(source_channels[i]);
    free(source_channels);
    free(api_hash);
    free(dest_channel);
    free(history_file);
    td_json_client_destroy(client);
    return 0;
}
