#include "tgf.h"

void seq_tracker_load(void) {
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

void seq_tracker_save(void) {
    FILE *f = fopen("seq_tracker.txt.tmp", "w");
    if (!f) return;
    for (int i = 0; i < seq_tracker_count; i++)
        fprintf(f, "%lld %lld %lld %lld %d\n", seq_tracker[i].chat_id, seq_tracker[i].last_msg_id, seq_tracker[i].last_date, seq_tracker[i].scan_from_id, seq_tracker[i].backfill_done);
    fclose(f);
    rename("seq_tracker.txt.tmp", "seq_tracker.txt");
}

void seq_tracker_set(long long chat_id, long long msg_id, long long date) {
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

long long seq_tracker_get_scan_from(long long chat_id) {
    for (int i = 0; i < seq_tracker_count; i++)
        if (seq_tracker[i].chat_id == chat_id)
            return seq_tracker[i].scan_from_id;
    return 0;
}

void seq_tracker_set_scan_from(long long chat_id, long long msg_id) {
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

int seq_tracker_is_backfill_done(long long chat_id) {
    for (int i = 0; i < seq_tracker_count; i++)
        if (seq_tracker[i].chat_id == chat_id)
            return seq_tracker[i].backfill_done;
    return 0;
}

void seq_tracker_set_backfill_done(long long chat_id, int done) {
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

void seq_tracker_pending_append(long long chat_id, PendingMsg msg) {
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

void flush_pending(long long src_chat_id) {
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
