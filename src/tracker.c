#include "tgf.h"

void tracker_store_load(TrackerStore *ts) {
    FILE *f = fopen("seq_tracker.txt", "r");
    if (!f) return;
    char line[128];
    while (ts->count < MAX_SEQ_TRACKER && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        long long chat_id, msg_id, date, scan_id, done;
        int n = sscanf(line, "%lld %lld %lld %lld %lld", &chat_id, &msg_id, &date, &scan_id, &done);
        if (n >= 3) {
            ts->entries[ts->count].chat_id = chat_id;
            ts->entries[ts->count].last_msg_id = msg_id;
            ts->entries[ts->count].last_date = date;
            ts->entries[ts->count].scan_from_id = (n >= 4) ? scan_id : 0;
            ts->entries[ts->count].backfill_done = (n >= 5) ? (int)done : 0;
            ts->count++;
        }
    }
    fclose(f);
}

void tracker_store_save(const TrackerStore *ts) {
    FILE *f = fopen("seq_tracker.txt.tmp", "w");
    if (!f) return;
    for (int i = 0; i < ts->count; i++)
        fprintf(f, "%lld %lld %lld %lld %d\n", ts->entries[i].chat_id, ts->entries[i].last_msg_id,
                ts->entries[i].last_date, ts->entries[i].scan_from_id,
                ts->entries[i].backfill_done);
    fclose(f);
    rename("seq_tracker.txt.tmp", "seq_tracker.txt");
}

void tracker_store_set(TrackerStore *ts, long long chat_id, long long msg_id, long long date) {
    (void)msg_id;
    (void)date;
    for (int i = 0; i < ts->count; i++) {
        if (ts->entries[i].chat_id == chat_id) {
            if (msg_id > ts->entries[i].last_msg_id) ts->entries[i].last_msg_id = msg_id;
            if (date > ts->entries[i].last_date) ts->entries[i].last_date = date;
            return;
        }
    }
    if (ts->count < MAX_SEQ_TRACKER) {
        ts->entries[ts->count].chat_id = chat_id;
        ts->entries[ts->count].last_msg_id = msg_id;
        ts->entries[ts->count].last_date = date;
        ts->entries[ts->count].scan_from_id = 0;
        ts->entries[ts->count].pending = NULL;
        ts->entries[ts->count].pending_count = 0;
        ts->entries[ts->count].pending_cap = 0;
        ts->count++;
    }
}

long long tracker_store_get_scan_from(const TrackerStore *ts, long long chat_id) {
    for (int i = 0; i < ts->count; i++)
        if (ts->entries[i].chat_id == chat_id) return ts->entries[i].scan_from_id;
    return 0;
}

void tracker_store_set_scan_from(TrackerStore *ts, long long chat_id, long long msg_id) {
    for (int i = 0; i < ts->count; i++) {
        if (ts->entries[i].chat_id == chat_id) {
            ts->entries[i].scan_from_id = msg_id;
            return;
        }
    }
    if (ts->count < MAX_SEQ_TRACKER) {
        ts->entries[ts->count].chat_id = chat_id;
        ts->entries[ts->count].last_msg_id = 0;
        ts->entries[ts->count].last_date = 0;
        ts->entries[ts->count].scan_from_id = msg_id;
        ts->entries[ts->count].backfill_done = 0;
        ts->entries[ts->count].pending = NULL;
        ts->entries[ts->count].pending_count = 0;
        ts->entries[ts->count].pending_cap = 0;
        ts->count++;
    }
}

int tracker_store_is_backfill_done(const TrackerStore *ts, long long chat_id) {
    for (int i = 0; i < ts->count; i++)
        if (ts->entries[i].chat_id == chat_id) return ts->entries[i].backfill_done;
    return 0;
}

void tracker_store_set_backfill_done(TrackerStore *ts, long long chat_id, int done) {
    for (int i = 0; i < ts->count; i++) {
        if (ts->entries[i].chat_id == chat_id) {
            ts->entries[i].backfill_done = done;
            return;
        }
    }
    if (ts->count < MAX_SEQ_TRACKER) {
        ts->entries[ts->count].chat_id = chat_id;
        ts->entries[ts->count].last_msg_id = 0;
        ts->entries[ts->count].last_date = 0;
        ts->entries[ts->count].scan_from_id = 0;
        ts->entries[ts->count].backfill_done = done;
        ts->entries[ts->count].pending = NULL;
        ts->entries[ts->count].pending_count = 0;
        ts->entries[ts->count].pending_cap = 0;
        ts->count++;
    }
}

static int pending_push_ram(TrackerStore *ts, long long chat_id, PendingMsg msg) {
    for (int i = 0; i < ts->count; i++) {
        if (ts->entries[i].chat_id == chat_id) {
            int n = ts->entries[i].pending_count;
            int cap = ts->entries[i].pending_cap;
            if (n >= cap) {
                cap = cap ? cap * 2 : 512;
                PendingMsg *np = realloc(ts->entries[i].pending, cap * sizeof(PendingMsg));
                if (!np) {
                    ui_oom("OOM in pending_append");
                    return 0;
                }
                ts->entries[i].pending = np;
                ts->entries[i].pending_cap = cap;
            }
            ts->entries[i].pending[n] = msg;
            ts->entries[i].pending_count = n + 1;
            return 1;
        }
    }
    return 0;
}

void tracker_store_pending_append(TrackerStore *ts, long long chat_id, PendingMsg msg) {
    if (!pending_push_ram(ts, chat_id, msg)) return;
    FILE *f = fopen("seq_pending.txt", "a");
    if (!f) return;
    fprintf(f, "%lld %lld %lld %lld %lld\n", chat_id, msg.id, msg.album_id, msg.reply_to_msg_id,
            msg.date);
    fclose(f);
}

void seq_pending_load(Runtime *rt) {
    FILE *f = fopen("seq_pending.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long chat, id, album, reply, date;
        if (sscanf(line, "%lld %lld %lld %lld %lld", &chat, &id, &album, &reply, &date) < 5)
            continue;
        PendingMsg m = {id, album, reply, date};
        pending_push_ram(&rt->tracker, chat, m);
    }
    fclose(f);
}

static void seq_pending_compact(long long chat_id) {
    FILE *f = fopen("seq_pending.txt", "r");
    if (!f) return;
    FILE *out = fopen("seq_pending.txt.tmp", "w");
    if (!out) {
        fclose(f);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long chat;
        if (sscanf(line, "%lld", &chat) == 1 && chat == chat_id) continue;
        fputs(line, out);
    }
    fclose(f);
    fclose(out);
    rename("seq_pending.txt.tmp", "seq_pending.txt");
}

void flush_pending(Runtime *rt, long long src_chat_id) {
    for (int i = 0; i < rt->tracker.count; i++) {
        if (rt->tracker.entries[i].chat_id == src_chat_id) {
            if (rt->tracker.entries[i].pending_count > 0)
                process_msgs(rt, src_chat_id, rt->tracker.entries[i].pending,
                             rt->tracker.entries[i].pending_count);
            free(rt->tracker.entries[i].pending);
            rt->tracker.entries[i].pending = NULL;
            rt->tracker.entries[i].pending_count = 0;
            rt->tracker.entries[i].pending_cap = 0;
            seq_pending_compact(src_chat_id);
            return;
        }
    }
}
