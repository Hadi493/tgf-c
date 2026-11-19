#include "tgf.h"

void fwd_queue_push(Runtime *rt, long long src_chat_id, const long long *ids, int count) {
    if (rt->fwd_queue_count >= rt->fwd_queue_cap) {
        int new_cap = rt->fwd_queue_cap ? rt->fwd_queue_cap * 2 : 1024;
        ForwardJob *nq = realloc(rt->fwd_queue, new_cap * sizeof(ForwardJob));
        if (!nq) { char b[64]; snprintf(b, sizeof(b), "OOM: fwd queue (%d)", new_cap); ui_oom(b); return; }
        rt->fwd_queue = nq;
        rt->fwd_queue_cap = new_cap;
    }
    int idx = (rt->fwd_queue_head + rt->fwd_queue_count) % rt->fwd_queue_cap;
    ForwardJob *j = &rt->fwd_queue[idx];
    j->src_chat_id = src_chat_id;
    j->count = count < MAX_FWD_IDS ? count : MAX_FWD_IDS;
    j->retries = 0;
    j->ids = malloc(j->count * sizeof(long long));
    if (!j->ids) { char b[64]; snprintf(b, sizeof(b), "OOM: fwd job ids (%d)", j->count); ui_oom(b); return; }
    for (int i = 0; i < j->count; i++) j->ids[i] = ids[i];
    rt->fwd_queue_count++;
}

void fwd_queue_save(Runtime *rt) {
    FILE *f = fopen("fwd_queue.txt.tmp", "w");
    if (!f) return;
    for (int i = 0; i < rt->fwd_queue_count; i++) {
        int idx = (rt->fwd_queue_head + i) % rt->fwd_queue_cap;
        ForwardJob *j = &rt->fwd_queue[idx];
        fprintf(f, "%lld %d", j->src_chat_id, j->retries);
        for (int k = 0; k < j->count; k++) fprintf(f, " %lld", j->ids[k]);
        fprintf(f, "\n");
    }
    if (rt->fwd_inflight_active) {
        ForwardJob *j = &rt->fwd_inflight;
        fprintf(f, "%lld %d", j->src_chat_id, j->retries);
        for (int k = 0; k < j->count; k++) fprintf(f, " %lld", j->ids[k]);
        fprintf(f, "\n");
    }
    fclose(f);
    rename("fwd_queue.txt.tmp", "fwd_queue.txt");
}

void fwd_queue_load(Runtime *rt) {
    FILE *f = fopen("fwd_queue.txt", "r");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *tok = strtok(line, " \t\r\n");
        if (!tok) continue;
        long long chat = atoll(tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok) continue;
        int retries = atoi(tok);
        if (retries < 0) retries = 0;
        long long ids[MAX_FWD_IDS];
        int n = 0;
        while ((tok = strtok(NULL, " \t\r\n")) && n < MAX_FWD_IDS) ids[n++] = atoll(tok);
        if (n == 0) continue;
        int before = rt->fwd_queue_count;
        fwd_queue_push(rt, chat, ids, n);
        if (rt->fwd_queue_count > before) {
            int tail = (rt->fwd_queue_head + rt->fwd_queue_count - 1) % rt->fwd_queue_cap;
            rt->fwd_queue[tail].retries = retries;
        }
    }
    fclose(f);
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

GroupResult group_batch(const PendingMsg *msgs, int count, int order) {
    GroupResult r = {0};
    if (count <= 0) return r;
    PendingMsg *sorted = malloc(count * sizeof(PendingMsg));
    GroupJob *jobs = calloc(count, sizeof(GroupJob));
    GroupMark *marks = calloc(count, sizeof(GroupMark));
    int *grouped = calloc(count, sizeof(int));
    if (!sorted || !jobs || !marks || !grouped) {
        free(sorted); free(jobs); free(marks); free(grouped);
        return r;
    }
    memcpy(sorted, msgs, count * sizeof(PendingMsg));
    if (count > 1 && order >= 0)
        qsort(sorted, count, sizeof(PendingMsg), order == 0 ? seq_cmp_asc : seq_cmp_desc);
    for (int i = 0; i < count; i++) {
        if (grouped[i]) continue;
        if (sorted[i].album_id == 0) {
            long long ids[MAX_FWD_IDS];
            int id_count = 0;
            if (sorted[i].reply_to_msg_id) ids[id_count++] = sorted[i].reply_to_msg_id;
            ids[id_count++] = sorted[i].id;
            for (int k = 0; k < count; k++)
                if (!grouped[k] && k != i && sorted[k].reply_to_msg_id == sorted[i].id && id_count < MAX_FWD_IDS)
                    ids[id_count++] = sorted[k].id;
            for (int a = 0; a < id_count; a++)
                for (int b = a + 1; b < id_count; b++)
                    if (ids[a] > ids[b]) { long long tmp = ids[a]; ids[a] = ids[b]; ids[b] = tmp; }
            GroupJob *j = &jobs[r.job_count++];
            j->count = id_count;
            for (int x = 0; x < id_count; x++) j->ids[x] = ids[x];
            marks[r.mark_count].id = sorted[i].id;
            marks[r.mark_count].date = sorted[i].date;
            r.mark_count++;
            grouped[i] = 1;
            for (int k = 0; k < count; k++) {
                if (!grouped[k] && k != i && sorted[k].reply_to_msg_id == sorted[i].id) {
                    marks[r.mark_count].id = sorted[k].id;
                    marks[r.mark_count].date = sorted[k].date;
                    r.mark_count++;
                    grouped[k] = 1;
                }
            }
        } else {
            long long album_id = sorted[i].album_id;
            int album_indices[MAX_FWD_IDS];
            int album_count = 0;
            for (int j = 0; j < count; j++)
                if (!grouped[j] && sorted[j].album_id == album_id) album_indices[album_count++] = j;
            for (int a = 0; a < album_count; a++)
                for (int b = a + 1; b < album_count; b++)
                    if (sorted[album_indices[a]].id > sorted[album_indices[b]].id) {
                        int tmp = album_indices[a]; album_indices[a] = album_indices[b]; album_indices[b] = tmp;
                    }
            GroupJob *j = &jobs[r.job_count++];
            j->count = album_count;
            for (int b = 0; b < album_count; b++) j->ids[b] = sorted[album_indices[b]].id;
            for (int b = 0; b < album_count; b++) {
                marks[r.mark_count].id = sorted[album_indices[b]].id;
                marks[r.mark_count].date = sorted[album_indices[b]].date;
                r.mark_count++;
                grouped[album_indices[b]] = 1;
            }
        }
    }
    free(grouped);
    free(sorted);
    r.jobs = jobs;
    r.marks = marks;
    return r;
}

void group_result_free(GroupResult *r) {
    free(r->jobs);
    free(r->marks);
    r->jobs = NULL;
    r->marks = NULL;
    r->job_count = 0;
    r->mark_count = 0;
}

void process_msgs(Runtime *rt, long long src_chat_id, PendingMsg *msgs, int count) {
    if (count == 0) return;
    int order = rt->cfg.enable_sequential_forwarding ? (rt->cfg.sequence_direction == 0 ? 0 : 1) : -1;
    GroupResult r = group_batch(msgs, count, order);
    if (!r.jobs) { ui_oom("OOM in process_msgs"); return; }
    for (int i = 0; i < r.job_count; i++)
        fwd_queue_push(rt, src_chat_id, r.jobs[i].ids, r.jobs[i].count);
    for (int i = 0; i < r.mark_count; i++) {
        history_store_add(&rt->history, &rt->cfg, src_chat_id, r.marks[i].id);
        if (rt->cfg.enable_sequential_forwarding) tracker_store_set(&rt->tracker, src_chat_id, r.marks[i].id, r.marks[i].date);
    }
    group_result_free(&r);
    if (rt->cfg.enable_sequential_forwarding) tracker_store_save(&rt->tracker);
    fwd_queue_save(rt);
}

static void fwd_inflight_clear(Runtime *rt) {
    free(rt->fwd_inflight.ids);
    rt->fwd_inflight.ids = NULL;
    rt->fwd_inflight_active = false;
    fwd_queue_save(rt);
}

static void fwd_requeue(Runtime *rt, int consume_budget) {
    ForwardJob *j = &rt->fwd_inflight;
    if (consume_budget && ++j->retries >= MAX_FWD_ATTEMPTS) {
        char b[80]; snprintf(b, sizeof(b), "Forward dropped after %d attempts", j->retries); ui_status(b);
        fwd_inflight_clear(rt);
        return;
    }
    int before = rt->fwd_queue_count;
    fwd_queue_push(rt, j->src_chat_id, j->ids, j->count);
    if (rt->fwd_queue_count == before) { fwd_inflight_clear(rt); return; }
    int tail = (rt->fwd_queue_head + rt->fwd_queue_count - 1) % rt->fwd_queue_cap;
    rt->fwd_queue[tail].retries = j->retries;
    fwd_inflight_clear(rt);
}

int process_fwd_queue(Runtime *rt, void *client) {
    double now = (double)time(NULL);
    if (rt->fwd_inflight_active) {
        if (now - rt->fwd_inflight_time <= FWD_TIMEOUT_SEC) return 0;
        ui_status("Forward timed out");
        fwd_requeue(rt, 1);
    }
    if (rt->fwd_queue_count == 0) return 0;
    if (now - rt->fwd_last_time < rt->cfg.forward_delay_sec) return 0;
    if (now < rt->fwd_flood_until) return 0;

    ForwardJob *j = &rt->fwd_queue[rt->fwd_queue_head];
    char ids_str[8192] = "";
    int pos = 0;
    for (int i = 0; i < j->count; i++)
        APPEND_ID(ids_str, pos, sizeof(ids_str), j->ids[i]);

    char payload[16384];
    snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"from_chat_id\":%lld,\"message_ids\":[%s]", rt->dest_chat_id, j->src_chat_id, ids_str);
    RequestId id = { j->count == 1 ? REQ_FORWARD_ONE : REQ_FORWARD_BATCH, j->src_chat_id, j->ids[0] };
    transport_send(rt, client, "forwardMessages", payload, id);

    rt->fwd_inflight = *j;
    rt->fwd_inflight_active = true;
    rt->fwd_inflight_time = now;
    j->ids = NULL;
    rt->fwd_queue_head = (rt->fwd_queue_head + 1) % rt->fwd_queue_cap;
    rt->fwd_queue_count--;
    rt->fwd_last_time = now;
    fwd_queue_save(rt);
    return 1;
}

void forward_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)client; (void)argv; (void)root;
    long long src_chat_id = id.chat_id;
    for (int i = 0; i < rt->cfg.num_sources; i++) {
        if (rt->source_chat_ids[i] == src_chat_id) {
            rt->src_name = rt->cfg.source_channels[i];
            break;
        }
    }
    ui_forwarded(rt->src_name, rt->cfg.dest_channel);
    if (rt->fwd_inflight_active && id.chat_id == rt->fwd_inflight.src_chat_id)
        fwd_inflight_clear(rt);
}

void forward_error_handle(Runtime *rt, RequestId id, int code, const char *msg) {
    if (!rt->fwd_inflight_active || id.chat_id != rt->fwd_inflight.src_chat_id) return;
    if (code == 429) {
        int secs = 5;
        const char *p = msg ? strstr(msg, "retry after ") : NULL;
        if (p) secs = atoi(p + 12);
        if (secs < 1) secs = 1;
        rt->fwd_flood_until = (double)time(NULL) + secs;
        char b[80]; snprintf(b, sizeof(b), "Flood wait: pausing %ds", secs); ui_status(b);
        fwd_requeue(rt, 0);
        return;
    }
    if (code == 400 || code == 403) {
        char b[80]; snprintf(b, sizeof(b), "Forward dropped (permanent error %d)", code); ui_status(b);
        fwd_inflight_clear(rt);
        return;
    }
    fwd_requeue(rt, 1);
}
