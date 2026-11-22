#include "tgf.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void emit_skipped(Runtime *rt, long long src_chat_id, tgf_skip_reason why, int n) {
    if (n <= 0 || !rt->emit) return;
    tgf_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TGF_EV_SKIPPED;
    ev.skip = why;
    ev.total = n;
    for (int i = 0; i < rt->cfg.num_sources; i++)
        if (rt->source_chat_ids[i] == src_chat_id) {
            ev.source = rt->cfg.source_channels[i];
            break;
        }
    rt->emit(rt, &ev);
}

void poll_channels(Runtime *rt, void *client) {
    for (int i = 0; i < rt->cfg.num_sources; i++) {
        long long chat_id = rt->source_chat_ids[i];
        if (chat_id == 0) continue;
        long long from_id = rt->cfg.enable_sequential_forwarding
                                ? tracker_store_get_scan_from(&rt->tracker, chat_id)
                                : 0;
        char payload[512];
        snprintf(payload, sizeof(payload),
                 "\"chat_id\":%lld,\"limit\":%d,\"from_message_id\":%lld,\"offset\":0", chat_id,
                 MSG_LIMIT, from_id);
        transport_send(rt, client, "getChatHistory", payload, (RequestId){REQ_HISTORY, chat_id, 0});
    }
}

void history_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)argv;
    history_response(rt, client, root, id.chat_id);
}

void history_response(Runtime *rt, void *client, cJSON *root, long long src_chat_id) {
    (void)client;

    const cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    if (!msgs || !cJSON_IsArray(msgs)) {
        return;
    }

    int total = cJSON_GetArraySize(msgs);
    if (total == 0) {
        if (rt->cfg.enable_sequential_forwarding &&
            !tracker_store_is_backfill_done(&rt->tracker, src_chat_id)) {
            flush_pending(rt, src_chat_id);
            tracker_store_set_backfill_done(&rt->tracker, src_chat_id, 1);
            tracker_store_set_scan_from(&rt->tracker, src_chat_id, 0);
            tracker_store_save(&rt->tracker);
        }
        return;
    }

    if (rt->cfg.enable_sequential_forwarding) {
        char st[64];
        snprintf(st, sizeof(st), "History: %d msgs from %lld", total, src_chat_id);
        ui_status(st);
    }

    long long cutoff = rt->cfg.history_window_hours > 0
                           ? (long long)time(NULL) - rt->cfg.history_window_hours * 3600
                           : 0;

    long long batch_oldest_id = 0;
    int i = 0;
    int new_count = 0;
    int cutoff_filtered = 0;
    int tracker_full = 0;
    int cursor_skipped = 0;

    int bd = rt->cfg.enable_sequential_forwarding &&
             tracker_store_is_backfill_done(&rt->tracker, src_chat_id);

    if (!bd && rt->cfg.enable_sequential_forwarding) {
        if (tracker_store_get_scan_from(&rt->tracker, src_chat_id) == 0) {
            int found = 0;
            for (int j = 0; j < rt->tracker.count; j++)
                if (rt->tracker.entries[j].chat_id == src_chat_id) {
                    found = 1;
                    break;
                }
            if (!found && rt->tracker.count < MAX_SEQ_TRACKER) {
                rt->tracker.entries[rt->tracker.count].chat_id = src_chat_id;
                rt->tracker.entries[rt->tracker.count].last_msg_id = 0;
                rt->tracker.entries[rt->tracker.count].last_date = 0;
                rt->tracker.entries[rt->tracker.count].scan_from_id = 0;
                rt->tracker.entries[rt->tracker.count].backfill_done = 0;
                rt->tracker.entries[rt->tracker.count].pending = NULL;
                rt->tracker.entries[rt->tracker.count].pending_count = 0;
                rt->tracker.entries[rt->tracker.count].pending_cap = 0;
                rt->tracker.count++;
            }
            if (!found && rt->tracker.count >= MAX_SEQ_TRACKER) tracker_full = 1;
        }
        for (const cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
            long long msg_id = cJSON_GetInt64(m, "id");
            batch_oldest_id = msg_id;
            long long album_id = cJSON_GetInt64(m, "media_album_id");
            long long msg_date = cJSON_GetInt64(m, "date");
            if (msg_id == 0) continue;
            if (tracker_store_get_scan_from(&rt->tracker, src_chat_id) == msg_id) {
                cursor_skipped++;
                continue;
            }
            if (cutoff && msg_date < cutoff) {
                cutoff_filtered++;
                continue;
            }
            PendingMsg pm;
            pm.id = msg_id;
            pm.album_id = album_id;
            pm.date = msg_date;
            pm.reply_to_msg_id = 0;
            const cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
            if (reply_to) pm.reply_to_msg_id = cJSON_GetInt64(reply_to, "message_id");
            tracker_store_pending_append(&rt->tracker, src_chat_id, pm);
            new_count++;
        }

        {
            char st[64];
            snprintf(st, sizeof(st), "Backfill: %d msgs, cutoff=%lld", new_count, cutoff);
            ui_status(st);
        }
        if (tracker_full) ui_status("WARNING: tracker full (128); chat not tracked");
        emit_skipped(rt, src_chat_id, TGF_SKIP_DUPLICATE, cursor_skipped);
        emit_skipped(rt, src_chat_id, TGF_SKIP_WINDOW, cutoff_filtered);

        int complete = 0;
        if (cutoff && cutoff_filtered > 0 && new_count == 0) complete = 1;
        if (new_count == 0 && total > 0 && cutoff_filtered == 0) complete = 1;

        if (complete) {
            int has_pending = 0;
            for (int j = 0; j < rt->tracker.count; j++)
                if (rt->tracker.entries[j].chat_id == src_chat_id &&
                    rt->tracker.entries[j].pending_count > 0) {
                    has_pending = 1;
                    break;
                }
            if (has_pending) {
                flush_pending(rt, src_chat_id);
                tracker_store_set_backfill_done(&rt->tracker, src_chat_id, 1);
                tracker_store_set_scan_from(&rt->tracker, src_chat_id, 0);
            } else if (cutoff_filtered > 0) {
                tracker_store_set_backfill_done(&rt->tracker, src_chat_id, 1);
                tracker_store_set_scan_from(&rt->tracker, src_chat_id, 0);
            } else {
                tracker_store_set_scan_from(&rt->tracker, src_chat_id, 0);
            }
            tracker_store_save(&rt->tracker);
            return;
        }

        if (new_count > 0 && batch_oldest_id) {
            tracker_store_set_scan_from(&rt->tracker, src_chat_id, batch_oldest_id);
            tracker_store_save(&rt->tracker);
        } else if (new_count == 0) {
            tracker_store_save(&rt->tracker);
        }

        return;
    }

    if (total >= MSG_LIMIT)
        ui_status("WARNING: history page full (10000); older messages may be missed");
    PendingMsg *new_msgs = calloc(total, sizeof(PendingMsg));
    if (!new_msgs) {
        ui_oom("OOM in history handler");
        return;
    }
    new_count = 0;
    cutoff_filtered = 0;
    int duplicates = 0;
    i = 0;
    for (const cJSON *m = msgs->child; m && i < total; m = m->next, i++) {
        long long msg_id = cJSON_GetInt64(m, "id");
        long long album_id = cJSON_GetInt64(m, "media_album_id");
        long long chat_id = cJSON_GetInt64(m, "chat_id");
        long long msg_date = cJSON_GetInt64(m, "date");
        if (msg_id == 0) continue;
        if (history_store_has(&rt->history, &rt->cfg, chat_id, msg_id)) {
            duplicates++;
            continue;
        }
        if (cutoff && msg_date < cutoff) {
            cutoff_filtered++;
            continue;
        }
        new_msgs[new_count].id = msg_id;
        new_msgs[new_count].album_id = album_id;
        new_msgs[new_count].date = msg_date;
        const cJSON *reply_to = cJSON_GetObjectItem(m, "reply_to");
        if (reply_to) new_msgs[new_count].reply_to_msg_id = cJSON_GetInt64(reply_to, "message_id");
        new_count++;
    }

    if (rt->cfg.enable_sequential_forwarding) {
        char st[64];
        snprintf(st, sizeof(st), "New: %d msgs, cutoff=%lld", new_count, cutoff);
        ui_status(st);
    }
    emit_skipped(rt, src_chat_id, TGF_SKIP_DUPLICATE, duplicates);
    emit_skipped(rt, src_chat_id, TGF_SKIP_WINDOW, cutoff_filtered);

    if (new_count == 0) {
        free(new_msgs);
        return;
    }

    process_msgs(rt, src_chat_id, new_msgs, new_count);
    free(new_msgs);
}
