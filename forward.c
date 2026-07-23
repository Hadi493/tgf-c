#include "tgf.h"

void fwd_queue_push(long long src_chat_id, const long long *ids, int count) {
    if (fwd_queue_count >= fwd_queue_cap) {
        int new_cap = fwd_queue_cap ? fwd_queue_cap * 2 : 1024;
        ForwardJob *nq = realloc(fwd_queue, new_cap * sizeof(ForwardJob));
        if (!nq) { char b[64]; snprintf(b, sizeof(b), "OOM: fwd queue (%d)", new_cap); set_status(b); return; }
        fwd_queue = nq;
        fwd_queue_cap = new_cap;
    }
    ForwardJob *j = &fwd_queue[fwd_queue_count++];
    j->src_chat_id = src_chat_id;
    j->count = count < MAX_FWD_IDS ? count : MAX_FWD_IDS;
    for (int i = 0; i < j->count; i++) j->ids[i] = ids[i];
}

static int seq_cmp_asc(const void *a, const void *b) {
    const PendingMsg *ma = (const PendingMsg *)a, *mb = (const PendingMsg *)b;
    if (ma->date != mb->date) return ma->date > mb->date ? 1 : -1;
    if (ma->id  != mb->id)   return ma->id  > mb->id   ? 1 : -1;
    return 0;
}

static int seq_cmp_desc(const void *a, const void *b) {
    const PendingMsg *ma = (const PendingMsg *)a, *mb = (const PendingMsg *)b;
    if (ma->date != mb->date) return ma->date < mb->date ? 1 : -1;
    if (ma->id  != mb->id)   return ma->id  < mb->id   ? 1 : -1;
    return 0;
}

void process_msgs(long long src_chat_id, PendingMsg *msgs, int count) {
    if (count == 0) return;
    if (enable_sequential_forwarding && count > 1) {
        int asc = (sequence_direction == 0);
        qsort(msgs, count, sizeof(PendingMsg), asc ? seq_cmp_asc : seq_cmp_desc);
    }
    int *grouped = calloc(count, sizeof(int));
    if (!grouped) { set_status("OOM in process_msgs"); return; }
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
            history_add(src_chat_id, msgs[i].id);
            if (enable_sequential_forwarding) seq_tracker_set(src_chat_id, msgs[i].id, msgs[i].date);
            grouped[i] = 1;
            for (int k = 0; k < count; k++) {
                if (!grouped[k] && k != i && msgs[k].reply_to_msg_id == msgs[i].id) {
                    history_add(src_chat_id, msgs[k].id);
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
            long long album_ids[256] = {0};
            for (int b = 0; b < album_count; b++) album_ids[b] = msgs[album_indices[b]].id;
            fwd_queue_push(src_chat_id, album_ids, album_count);
            for (int b = 0; b < album_count; b++) {
                history_add(src_chat_id, msgs[album_indices[b]].id);
                if (enable_sequential_forwarding) seq_tracker_set(src_chat_id, msgs[album_indices[b]].id, msgs[album_indices[b]].date);
                grouped[album_indices[b]] = 1;
            }
        }
    }
    free(grouped);
    if (enable_sequential_forwarding) seq_tracker_save();
}

int process_fwd_queue(void *client) {
    if (fwd_queue_count == 0) return 0;
    double now = (double)time(NULL);
    if (now - fwd_last_time < forward_delay_sec) return 0;

    const ForwardJob *j = &fwd_queue[0];
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
