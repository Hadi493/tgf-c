#include "tgf.h"
#include "config.h"

#include <fcntl.h>
#include <unistd.h>

static Runtime rt;

static int checks   = 0;
static int failures = 0;

static char captured_type[64];
static char captured_payload[16384];
static char captured_extra[128];

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_LL(actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    checks++; \
    if (a_ != e_) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n", \
                __FILE__, __LINE__, #actual, a_, e_); \
    } \
} while (0)

#define CHECK_STR_CONTAINS(haystack, needle) do { \
    checks++; \
    if (!strstr((haystack), (needle))) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: \"%s\" does not contain \"%s\"\n", \
                __FILE__, __LINE__, (haystack), (needle)); \
    } \
} while (0)

void ui_status(const char *msg) {
    snprintf(rt.status_msg, sizeof(rt.status_msg), "%s", msg);
}

void ui_oom(const char *msg) {
    ui_status(msg);
}

void ui_forwarded(const char *source, const char *dest) {
    (void)source; (void)dest;
}

void transport_send(Runtime *rt, void *client, const char *type, const char *payload, RequestId id) {
    (void)client;
    (void)rt;
    char extra_buf[64];
    const char *extra = request_extra(id, extra_buf, sizeof(extra_buf));
    snprintf(captured_type, sizeof(captured_type), "%s", type);
    snprintf(captured_payload, sizeof(captured_payload), "%s", payload ? payload : "");
    snprintf(captured_extra, sizeof(captured_extra), "%s", extra ? extra : "");
}

void transport_destroy(void *client) {
    (void)client;
}

static void reset_state(void) {
    if (rt.history.set) {
        for (int i = 0; i < rt.history.set_size; i++) free(rt.history.set[i]);
    }
    free(rt.history.set);
    free(rt.history.time);
    rt.history.set = NULL;
    rt.history.time = NULL;
    rt.history.set_size = 0;
    rt.history.count = 0;

    for (int i = 0; i < rt.tracker.count; i++) free(rt.tracker.entries[i].pending);
    memset(rt.tracker.entries, 0, sizeof(rt.tracker.entries));
    rt.tracker.count = 0;

    for (int i = 0; i < rt.fwd_queue_count; i++) {
        int idx = (rt.fwd_queue_head + i) % rt.fwd_queue_cap;
        free(rt.fwd_queue[idx].ids);
    }
    free(rt.fwd_queue);
    rt.fwd_queue = NULL;
    rt.fwd_queue_count = 0;
    rt.fwd_queue_cap = 0;
    rt.fwd_queue_head = 0;
    rt.fwd_last_time = 0;
    free(rt.fwd_inflight.ids);
    rt.fwd_inflight.ids = NULL;
    rt.fwd_inflight.retries = 0;
    rt.fwd_inflight_active = false;
    rt.fwd_inflight_time = 0;
    rt.fwd_flood_until = 0;

    rt.cfg.history_window_hours = 24;
    rt.cfg.enable_sequential_forwarding = 0;
    rt.cfg.sequence_direction = 0;
    rt.cfg.forward_delay_sec = 0;
    rt.dest_chat_id = -100777;

    free(rt.source_chat_ids);
    rt.source_chat_ids = NULL;
    for (int i = 0; i < rt.cfg.num_sources; i++) free(rt.cfg.source_channels[i]);
    free(rt.cfg.source_channels);
    rt.cfg.source_channels = NULL;
    rt.cfg.num_sources = 0;
    rt.source_count = 0;
    rt.resolve_index = 0;
    rt.resolve_active = 0;
    rt.authorized = 0;
    rt.dest_resolved = 0;

    captured_type[0] = 0;
    captured_payload[0] = 0;
    captured_extra[0] = 0;
    rt.status_msg[0] = 0;

    remove("history.txt");
    remove("seq_tracker.txt");
    remove("config.json");
    remove("stderr.txt");
    remove("fwd_queue.txt");
    remove("fwd_queue.txt.tmp");
    remove("history.txt.tmp");
    remove("seq_pending.txt");
    remove("seq_pending.txt.tmp");
}

static PendingMsg make_msg(long long id, long long album, long long reply, long long date) {
    PendingMsg m;
    m.id = id;
    m.album_id = album;
    m.reply_to_msg_id = reply;
    m.date = date;
    return m;
}

static unsigned int test_hash(const char *key) {
    unsigned int h = 2166136261u;
    while (*key) {
        h ^= (unsigned char)*key++;
        h *= 16777619u;
    }
    return h;
}

static int history_slot(long long chat_id, long long msg_id) {
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = test_hash(key) & (rt.history.set_size - 1);
    for (int i = 0; i < rt.history.set_size; i++) {
        if (!rt.history.set[idx]) return -1;
        if (strcmp(rt.history.set[idx], key) == 0) return (int)idx;
        idx = (idx + 1) & (rt.history.set_size - 1);
    }
    return -1;
}

static int history_find_physical(long long chat_id, long long msg_id) {
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    for (int i = 0; i < rt.history.set_size; i++)
        if (rt.history.set[i] && strcmp(rt.history.set[i], key) == 0) return i;
    return -1;
}

static void set_history_time(long long chat_id, long long msg_id, time_t ts) {
    int slot = history_slot(chat_id, msg_id);
    if (slot >= 0) rt.history.time[slot] = ts;
}

static SeqEntry *find_tracker(long long chat_id) {
    for (int i = 0; i < rt.tracker.count; i++)
        if (rt.tracker.entries[i].chat_id == chat_id) return &rt.tracker.entries[i];
    return NULL;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

static int find_hash_collision(long long *first, long long *second) {
    static int seen[16384];
    char key[64];
    for (int i = 0; i < 16384; i++) seen[i] = -1;
    for (long long id = 1; id < 200000; id++) {
        snprintf(key, sizeof(key), "%lld_%lld", (long long)-500, id);
        unsigned int idx = test_hash(key) & 16383u;
        if (seen[idx] >= 0) {
            *first = seen[idx];
            *second = id;
            return 1;
        }
        seen[idx] = (int)id;
    }
    return 0;
}

static void test_empty_batch(void) {
    process_msgs(&rt, -1, NULL, 0);
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK_LL(rt.history.count, 0);
}

static void test_single_message(void) {
    PendingMsg m = make_msg(42, 0, 0, 100);
    process_msgs(&rt, -100123, &m, 1);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, 1);
    CHECK_LL(rt.fwd_queue[0].ids[0], 42);
    CHECK(history_store_has(&rt.history, &rt.cfg, -100123, 42));
}

static void test_two_messages_wire_order(void) {
    PendingMsg msgs[2] = {
        make_msg(2, 0, 0, 200),
        make_msg(1, 0, 0, 100),
    };
    process_msgs(&rt, -100123, msgs, 2);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[0].ids[0], 2);
    CHECK_LL(rt.fwd_queue[1].ids[0], 1);
}

static void test_sequential_asc(void) {
    PendingMsg msgs[3] = {
        make_msg(3, 0, 0, 30),
        make_msg(1, 0, 0, 10),
        make_msg(2, 0, 0, 20),
    };
    rt.cfg.enable_sequential_forwarding = 1;
    rt.cfg.sequence_direction = 0;
    process_msgs(&rt, -100123, msgs, 3);
    CHECK_LL(rt.fwd_queue_count, 3);
    CHECK_LL(rt.fwd_queue[0].ids[0], 1);
    CHECK_LL(rt.fwd_queue[1].ids[0], 2);
    CHECK_LL(rt.fwd_queue[2].ids[0], 3);
}

static void test_sequential_desc(void) {
    PendingMsg msgs[3] = {
        make_msg(1, 0, 0, 10),
        make_msg(2, 0, 0, 20),
        make_msg(3, 0, 0, 30),
    };
    rt.cfg.enable_sequential_forwarding = 1;
    rt.cfg.sequence_direction = 1;
    process_msgs(&rt, -100123, msgs, 3);
    CHECK_LL(rt.fwd_queue_count, 3);
    CHECK_LL(rt.fwd_queue[0].ids[0], 3);
    CHECK_LL(rt.fwd_queue[1].ids[0], 2);
    CHECK_LL(rt.fwd_queue[2].ids[0], 1);
}

static void test_reply_chain_and_parent_duplicate(void) {
    PendingMsg msgs[2] = {
        make_msg(10, 0, 3, 200),
        make_msg(3, 0, 0, 100),
    };
    process_msgs(&rt, -100123, msgs, 2);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[0].count, 2);
    CHECK_LL(rt.fwd_queue[0].ids[0], 3);
    CHECK_LL(rt.fwd_queue[0].ids[1], 10);
    CHECK_LL(rt.fwd_queue[1].count, 1);
    CHECK_LL(rt.fwd_queue[1].ids[0], 3);
}

static void test_album_batch_ordering(void) {
    PendingMsg msgs[3] = {
        make_msg(7, 77, 0, 30),
        make_msg(3, 77, 0, 10),
        make_msg(5, 77, 0, 20),
    };
    process_msgs(&rt, -100123, msgs, 3);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, 3);
    CHECK_LL(rt.fwd_queue[0].ids[0], 3);
    CHECK_LL(rt.fwd_queue[0].ids[1], 5);
    CHECK_LL(rt.fwd_queue[0].ids[2], 7);
}

static void test_album_split_across_batches(void) {
    PendingMsg first[2] = {
        make_msg(5, 77, 0, 20),
        make_msg(3, 77, 0, 10),
    };
    process_msgs(&rt, -100123, first, 2);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, 2);

    PendingMsg second[1] = { make_msg(7, 77, 0, 30) };
    process_msgs(&rt, -100123, second, 1);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[1].count, 1);
    CHECK_LL(rt.fwd_queue[1].ids[0], 7);
}

static void test_album_ignores_reply_parent(void) {
    PendingMsg m = make_msg(9, 5, 8, 100);
    process_msgs(&rt, -100123, &m, 1);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, 1);
    CHECK_LL(rt.fwd_queue[0].ids[0], 9);
}

static void test_reply_child_cap(void) {
    PendingMsg msgs[301];
    msgs[0] = make_msg(1, 0, 0, 100);
    for (int i = 1; i < 301; i++)
        msgs[i] = make_msg(i + 1, 0, 1, 100 + i);

    process_msgs(&rt, -100123, msgs, 301);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, 256);
    CHECK_LL(rt.fwd_queue[0].ids[255], 256);
    CHECK(history_store_has(&rt.history, &rt.cfg, -100123, 301));
    CHECK_LL(rt.history.count, 301);
}

static void test_queue_id_cap(void) {
    long long ids[300];
    for (int i = 0; i < 300; i++) ids[i] = i + 1;
    fwd_queue_push(&rt, -100123, ids, 300);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[0].count, MAX_FWD_IDS);
    CHECK_LL(rt.fwd_queue[0].ids[MAX_FWD_IDS - 1], MAX_FWD_IDS);
}

static void test_enqueue_marks_history(void) {
    PendingMsg m = make_msg(55, 0, 0, 100);
    process_msgs(&rt, -100123, &m, 1);
    CHECK(history_store_has(&rt.history, &rt.cfg, -100123, 55));
    CHECK_LL(rt.fwd_queue_count, 1);
}

static void test_history_window_expiry(void) {
    history_store_add(&rt.history, &rt.cfg, -1, 100);
    CHECK(history_store_has(&rt.history, &rt.cfg, -1, 100));

    rt.cfg.history_window_hours = 0;
    CHECK(history_store_has(&rt.history, &rt.cfg, -1, 100));

    rt.cfg.history_window_hours = 24;
    set_history_time(-1, 100, time(NULL) - 25 * 3600);
    CHECK(!history_store_has(&rt.history, &rt.cfg, -1, 100));
}

static void test_history_prune_removes(void) {
    history_store_add(&rt.history, &rt.cfg, -1, 1);
    history_store_add(&rt.history, &rt.cfg, -1, 2);
    set_history_time(-1, 1, time(NULL) - 25 * 3600);
    set_history_time(-1, 2, time(NULL) - 25 * 3600);

    history_store_prune(&rt.history, &rt.cfg);
    CHECK_LL(rt.history.count, 0);
    CHECK(!history_store_has(&rt.history, &rt.cfg, -1, 1));
    CHECK(!history_store_has(&rt.history, &rt.cfg, -1, 2));
}

static void test_history_prune_repairs_chains(void) {
    long long first = 0, second = 0;
    CHECK(find_hash_collision(&first, &second));
    if (!first || !second) return;

    history_store_add(&rt.history, &rt.cfg, -500, first);
    history_store_add(&rt.history, &rt.cfg, -500, second);
    CHECK(history_store_has(&rt.history, &rt.cfg, -500, first));
    CHECK(history_store_has(&rt.history, &rt.cfg, -500, second));

    set_history_time(-500, first, time(NULL) - 25 * 3600);
    history_store_prune(&rt.history, &rt.cfg);

    CHECK_LL(rt.history.count, 1);
    CHECK(history_find_physical(-500, second) >= 0);
    CHECK(history_store_has(&rt.history, &rt.cfg, -500, second));
    CHECK(!history_store_has(&rt.history, &rt.cfg, -500, first));
}

static void test_history_load_skips_malformed(void) {
    char content[128];
    snprintf(content, sizeof(content), "   \n\t\n-100_5\n-100_6 %lld\n", (long long)time(NULL));
    write_file("history.txt", content);
    history_store_load(&rt.history, &rt.cfg);
    CHECK_LL(rt.history.count, 2);
    CHECK(history_store_has(&rt.history, &rt.cfg, -100, 5));
    CHECK(history_store_has(&rt.history, &rt.cfg, -100, 6));
}

static cJSON *msg_obj(long long id, long long date, long long album, long long reply, long long chat) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", (double)id);
    cJSON_AddNumberToObject(m, "date", (double)date);
    cJSON_AddNumberToObject(m, "chat_id", (double)chat);
    if (album) cJSON_AddNumberToObject(m, "media_album_id", (double)album);
    if (reply) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "message_id", (double)reply);
        cJSON_AddItemToObject(m, "reply_to", r);
    }
    return m;
}

static cJSON *root_with(cJSON *arr) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "messages", arr);
    return r;
}

static void test_history_normal_filters(void) {
    time_t now = time(NULL);
    history_store_add(&rt.history, &rt.cfg, -100, 20);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(0, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(20, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(10, now - 25 * 3600, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(30, now, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 30);
    CHECK(history_store_has(&rt.history, &rt.cfg, -100, 30));
}

static void test_history_normal_wire_order(void) {
    time_t now = time(NULL);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(40, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(30, now, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 40);
    CHECK_LL(rt.fwd_queue[(rt.fwd_queue_head + 1) % rt.fwd_queue_cap].ids[0], 30);
}

static void test_history_normal_empty_noop(void) {
    cJSON *root = root_with(cJSON_CreateArray());
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK_LL(rt.tracker.count, 0);
}

static void test_history_sequential_new_status(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_backfill_done(&rt.tracker, -100, 1);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_STR_CONTAINS(rt.status_msg, "New: 1 msgs");
    CHECK_LL(rt.fwd_queue_count, 1);
}

static void test_backfill_collect_and_page(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    time_t now = time(NULL);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(20, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(10, now, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    SeqEntry *e = find_tracker(-100);
    CHECK(e != NULL);
    CHECK_LL(e->pending_count, 3);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 10);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 0);
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK_STR_CONTAINS(rt.status_msg, "Backfill: 3 msgs");
}

static void test_backfill_echo_skip(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 50);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(50, time(NULL), 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(40, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(find_tracker(-100)->pending_count, 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 40);
}

static void test_backfill_no_dedupe(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    history_store_add(&rt.history, &rt.cfg, -100, 30);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(find_tracker(-100)->pending_count, 1);
}

static void test_backfill_completion_a(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    time_t old = time(NULL) - 25 * 3600;
    tracker_store_set_scan_from(&rt.tracker, -100, 50);
    tracker_store_pending_append(&rt.tracker, -100, make_msg(60, 0, 0, old + 3600));
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(40, old, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(30, old, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 60);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 0);
    CHECK_LL(find_tracker(-100)->pending_count, 0);
}

static void test_backfill_completion_b(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 50);
    tracker_store_pending_append(&rt.tracker, -100, make_msg(60, 0, 0, time(NULL)));
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(50, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 0);
}

static void test_backfill_reset_without_done(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 50);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(50, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 0);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 0);
    CHECK_LL(rt.fwd_queue_count, 0);
}

static void test_backfill_total_zero(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 50);
    tracker_store_pending_append(&rt.tracker, -100, make_msg(60, 0, 0, time(NULL)));
    cJSON *root = root_with(cJSON_CreateArray());
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 0);
}

static void test_backfill_straddle(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    time_t now = time(NULL);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(20, now - 25 * 3600, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -100), 20);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 0);
    CHECK_LL(find_tracker(-100)->pending_count, 1);

    cJSON *arr2 = cJSON_CreateArray();
    cJSON_AddItemToArray(arr2, msg_obj(10, now - 25 * 3600, 0, 0, -100));
    cJSON *root2 = root_with(arr2);
    history_response(&rt, NULL, root2, -100);
    cJSON_Delete(root2);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 1);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 30);
}

static void test_history_handle_routes(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, time(NULL), 0, 0, -100));
    cJSON *root = root_with(arr);
    RequestId id = { REQ_HISTORY, -100, 0 };
    char *argv[] = { NULL, NULL };
    history_handle(&rt, NULL, root, id, argv);
    cJSON_Delete(root);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 30);
}

static void test_tracker_load_fields(void) {
    write_file("seq_tracker.txt",
               "-111 10 100 5 1\n"
               "-222 20 200 7\n"
               "-333 30 300\n");
    tracker_store_load(&rt.tracker);
    CHECK_LL(rt.tracker.count, 3);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -111), 5);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -111), 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -222), 7);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -222), 0);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -333), 0);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -333), 0);
}

static void test_tracker_save_roundtrip(void) {
    tracker_store_set_scan_from(&rt.tracker, -111, 5);
    tracker_store_set_backfill_done(&rt.tracker, -111, 1);
    tracker_store_set(&rt.tracker, -111, 9, 100);
    tracker_store_save(&rt.tracker);

    for (int i = 0; i < rt.tracker.count; i++) free(rt.tracker.entries[i].pending);
    memset(rt.tracker.entries, 0, sizeof(rt.tracker.entries));
    rt.tracker.count = 0;

    tracker_store_load(&rt.tracker);
    CHECK_LL(rt.tracker.count, 1);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -111), 5);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -111), 1);
    CHECK_LL(rt.tracker.entries[0].last_msg_id, 9);
    CHECK_LL(rt.tracker.entries[0].last_date, 100);
}

static void test_tracker_capacity(void) {
    for (int i = 0; i < 130; i++)
        tracker_store_set_scan_from(&rt.tracker, -1000 - i, i + 1);
    CHECK_LL(rt.tracker.count, MAX_SEQ_TRACKER);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -1000 - 127), 128);
    CHECK_LL(tracker_store_get_scan_from(&rt.tracker, -1000 - 128), 0);
}

static void test_tracker_pending_flush(void) {
    tracker_store_set_scan_from(&rt.tracker, -444, 0);
    rt.cfg.enable_sequential_forwarding = 1;
    rt.cfg.sequence_direction = 0;

    tracker_store_pending_append(&rt.tracker, -444, make_msg(20, 0, 0, 200));
    tracker_store_pending_append(&rt.tracker, -444, make_msg(10, 0, 0, 100));
    SeqEntry *e = find_tracker(-444);
    CHECK(e != NULL);
    CHECK_LL(e->pending_count, 2);

    flush_pending(&rt, -444);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[0].ids[0], 10);
    CHECK_LL(rt.fwd_queue[1].ids[0], 20);
    e = find_tracker(-444);
    CHECK(e != NULL);
    CHECK_LL(e->pending_count, 0);
    CHECK(e->pending == NULL);
}

static void test_pending_journal_roundtrip(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 0);
    tracker_store_pending_append(&rt.tracker, -100, make_msg(5, 0, 0, 100));
    tracker_store_pending_append(&rt.tracker, -100, make_msg(6, 0, 0, 101));
    CHECK_LL(find_tracker(-100)->pending_count, 2);
    free(rt.tracker.entries[0].pending);
    rt.tracker.entries[0].pending = NULL;
    rt.tracker.entries[0].pending_count = 0;
    rt.tracker.entries[0].pending_cap = 0;
    seq_pending_load(&rt);
    CHECK_LL(find_tracker(-100)->pending_count, 2);
    CHECK_LL(find_tracker(-100)->pending[1].id, 6);
}

static void test_pending_compact_on_flush(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 0);
    tracker_store_set_scan_from(&rt.tracker, -200, 0);
    tracker_store_pending_append(&rt.tracker, -100, make_msg(5, 0, 0, 100));
    tracker_store_pending_append(&rt.tracker, -200, make_msg(9, 0, 0, 100));
    flush_pending(&rt, -100);
    CHECK_LL(find_tracker(-100)->pending_count, 0);
    CHECK_LL(find_tracker(-200)->pending_count, 1);
    FILE *f = fopen("seq_pending.txt", "r");
    CHECK(f != NULL);
    char line[256];
    int lines = 0;
    long long chat = 0;
    while (f && fgets(line, sizeof(line), f)) { lines++; sscanf(line, "%lld", &chat); }
    if (f) fclose(f);
    CHECK_LL(lines, 1);
    CHECK_LL(chat, -200);
}

static void test_pending_load_corrupt_skipped(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    tracker_store_set_scan_from(&rt.tracker, -100, 0);
    write_file("seq_pending.txt", "-100 5\n-100 6 0 0 101\n");
    seq_pending_load(&rt);
    CHECK_LL(find_tracker(-100)->pending_count, 1);
    CHECK_LL(find_tracker(-100)->pending[0].id, 6);
}

static void test_backfill_resume_after_crash(void) {
    rt.cfg.enable_sequential_forwarding = 1;
    time_t now = time(NULL);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, now, 0, 0, -100));
    cJSON_AddItemToArray(arr, msg_obj(20, now, 0, 0, -100));
    cJSON *root = root_with(arr);
    history_response(&rt, NULL, root, -100);
    cJSON_Delete(root);
    CHECK_LL(find_tracker(-100)->pending_count, 2);
    free(rt.tracker.entries[0].pending);
    rt.tracker.entries[0].pending = NULL;
    rt.tracker.entries[0].pending_count = 0;
    rt.tracker.entries[0].pending_cap = 0;
    seq_pending_load(&rt);
    CHECK_LL(find_tracker(-100)->pending_count, 2);
    cJSON *arr2 = cJSON_CreateArray();
    cJSON_AddItemToArray(arr2, msg_obj(20, now, 0, 0, -100));
    cJSON *root2 = root_with(arr2);
    history_response(&rt, NULL, root2, -100);
    cJSON_Delete(root2);
    CHECK_LL(tracker_store_is_backfill_done(&rt.tracker, -100), 1);
    CHECK_LL(rt.fwd_queue_count, 2);
}

static void test_dispatch_single(void) {
    long long ids[1] = { 42 };
    fwd_queue_push(&rt, -100123, ids, 1);
    int sent = process_fwd_queue(&rt, NULL);

    CHECK_LL(sent, 1);
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK(strcmp(captured_type, "forwardMessages") == 0);
    CHECK(strcmp(captured_extra, "fwd_-100123_42") == 0);
    CHECK_STR_CONTAINS(captured_payload, "\"chat_id\":-100777");
    CHECK_STR_CONTAINS(captured_payload, "\"from_chat_id\":-100123");
    CHECK_STR_CONTAINS(captured_payload, "\"message_ids\":[42]");
}

static void test_dispatch_batch(void) {
    long long ids[2] = { 3, 5 };
    fwd_queue_push(&rt, -100123, ids, 2);
    int sent = process_fwd_queue(&rt, NULL);

    CHECK_LL(sent, 1);
    CHECK(strcmp(captured_extra, "fwd_-100123_batch") == 0);
    CHECK_STR_CONTAINS(captured_payload, "\"message_ids\":[3,5]");
}

static void test_dispatch_delay_gate(void) {
    long long ids[1] = { 1 };
    rt.cfg.forward_delay_sec = 3600;

    fwd_queue_push(&rt, -100123, ids, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    fwd_queue_push(&rt, -100123, ids, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 0);
    CHECK_LL(rt.fwd_queue_count, 1);

    rt.cfg.forward_delay_sec = 0;
    char *argv[] = { NULL, NULL };
    RequestId rid = { REQ_FORWARD_ONE, -100123, 1 };
    forward_handle(&rt, NULL, NULL, rid, argv);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    CHECK_LL(rt.fwd_queue_count, 0);
}

static void test_dispatch_inflight_blocks(void) {
    long long a[1] = { 1 }, b[1] = { 2 };
    rt.cfg.forward_delay_sec = 0;
    fwd_queue_push(&rt, -100123, a, 1);
    fwd_queue_push(&rt, -100123, b, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 0);
    CHECK_LL(rt.fwd_queue_count, 1);
    char *argv[] = { NULL, NULL };
    RequestId rid = { REQ_FORWARD_ONE, -100123, 1 };
    forward_handle(&rt, NULL, NULL, rid, argv);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    CHECK_STR_CONTAINS(captured_payload, "\"message_ids\":[2]");
}

static void test_forward_retry_then_drop(void) {
    long long a[1] = { 7 };
    rt.cfg.forward_delay_sec = 0;
    fwd_queue_push(&rt, -100123, a, 1);
    RequestId rid = { REQ_FORWARD_ONE, -100123, 7 };
    for (int i = 0; i < 3; i++) {
        CHECK_LL(process_fwd_queue(&rt, NULL), 1);
        forward_error_handle(&rt, rid, 500, "Internal server error");
    }
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK_LL(rt.fwd_inflight_active, false);
    CHECK_STR_CONTAINS(rt.status_msg, "Forward dropped after 3 attempts");
}

static void test_forward_permanent_drop(void) {
    long long a[1] = { 7 };
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    RequestId rid = { REQ_FORWARD_ONE, -100123, 7 };
    forward_error_handle(&rt, rid, 403, "Forbidden");
    CHECK_LL(rt.fwd_queue_count, 0);
    CHECK_LL(rt.fwd_inflight_active, false);
    CHECK_STR_CONTAINS(rt.status_msg, "Forward dropped (permanent error 403)");
}

static void test_forward_flood_pause(void) {
    long long a[1] = { 7 };
    rt.cfg.forward_delay_sec = 0;
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    RequestId rid = { REQ_FORWARD_ONE, -100123, 7 };
    forward_error_handle(&rt, rid, 429, "Too Many Requests: retry after 30");
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK(rt.fwd_flood_until > (double)time(NULL) + 25);
    CHECK_STR_CONTAINS(rt.status_msg, "Flood wait: pausing 30s");
    CHECK_LL(process_fwd_queue(&rt, NULL), 0);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].retries, 0);
    rt.fwd_flood_until = 0;
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
}

static void test_forward_timeout_retries(void) {
    long long a[1] = { 7 };
    rt.cfg.forward_delay_sec = 0;
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    rt.fwd_inflight_time = (double)time(NULL) - FWD_TIMEOUT_SEC - 1;
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    CHECK_STR_CONTAINS(rt.status_msg, "Forward timed out");
    CHECK_LL(rt.fwd_inflight.retries, 1);
}

static void test_forward_stale_ignored(void) {
    long long a[1] = { 7 };
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    RequestId rid = { REQ_FORWARD_ONE, -999999, 7 };
    forward_error_handle(&rt, rid, 500, "x");
    CHECK_LL(rt.fwd_inflight_active, true);
    CHECK_LL(rt.fwd_queue_count, 0);
}

static void test_forward_retry_tail_order(void) {
    long long a[1] = { 1 }, b[1] = { 2 };
    rt.cfg.forward_delay_sec = 0;
    fwd_queue_push(&rt, -100123, a, 1);
    fwd_queue_push(&rt, -100123, b, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    RequestId rid = { REQ_FORWARD_ONE, -100123, 1 };
    forward_error_handle(&rt, rid, 500, "x");
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    CHECK_STR_CONTAINS(captured_payload, "\"message_ids\":[2]");
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].retries, 1);
}

static void test_queue_save_load_roundtrip(void) {
    long long a[2] = { 5, 6 }, b[1] = { 9 };
    fwd_queue_push(&rt, -100123, a, 2);
    fwd_queue_push(&rt, -100456, b, 1);
    rt.fwd_queue[(rt.fwd_queue_head + 1) % rt.fwd_queue_cap].retries = 2;
    fwd_queue_save(&rt);
    for (int i = 0; i < rt.fwd_queue_count; i++)
        free(rt.fwd_queue[(rt.fwd_queue_head + i) % rt.fwd_queue_cap].ids);
    rt.fwd_queue_count = 0;
    rt.fwd_queue_head = 0;
    fwd_queue_load(&rt);
    CHECK_LL(rt.fwd_queue_count, 2);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].src_chat_id, -100123);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].count, 2);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[1], 6);
    int t = (rt.fwd_queue_head + 1) % rt.fwd_queue_cap;
    CHECK_LL(rt.fwd_queue[t].src_chat_id, -100456);
    CHECK_LL(rt.fwd_queue[t].retries, 2);
}

static void test_queue_load_inflight_requeued(void) {
    long long a[1] = { 7 };
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    free(rt.fwd_inflight.ids);
    rt.fwd_inflight.ids = NULL;
    rt.fwd_inflight_active = false;
    rt.fwd_queue_count = 0;
    rt.fwd_queue_head = 0;
    fwd_queue_load(&rt);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].ids[0], 7);
}

static void test_queue_load_corrupt_skipped(void) {
    write_file("fwd_queue.txt", "\n-100\n-100 0\n-100 1 5 6\n");
    fwd_queue_load(&rt);
    CHECK_LL(rt.fwd_queue_count, 1);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].count, 2);
    CHECK_LL(rt.fwd_queue[rt.fwd_queue_head].retries, 1);
}

static void test_queue_persist_on_dispatch_and_confirm(void) {
    long long a[1] = { 7 };
    fwd_queue_push(&rt, -100123, a, 1);
    CHECK_LL(process_fwd_queue(&rt, NULL), 1);
    FILE *f = fopen("fwd_queue.txt", "r");
    CHECK(f != NULL);
    char line[256];
    char *got = f ? fgets(line, sizeof(line), f) : NULL;
    if (f) fclose(f);
    CHECK(got != NULL);
    if (got) CHECK_STR_CONTAINS(line, "-100123 0 7");
    char *argv[] = { NULL, NULL };
    RequestId rid = { REQ_FORWARD_ONE, -100123, 7 };
    forward_handle(&rt, NULL, NULL, rid, argv);
    f = fopen("fwd_queue.txt", "r");
    CHECK(f != NULL);
    got = f ? fgets(line, sizeof(line), f) : NULL;
    if (f) fclose(f);
    CHECK(got == NULL);
}

static void test_history_prune_leaves_no_tmp(void) {
    history_store_add(&rt.history, &rt.cfg, -1, 1);
    set_history_time(-1, 1, time(NULL) - 25 * 3600);
    history_store_prune(&rt.history, &rt.cfg);
    CHECK_LL(rt.history.count, 0);
    FILE *f = fopen("history.txt.tmp", "r");
    CHECK(f == NULL);
    if (f) fclose(f);
    f = fopen("history.txt", "r");
    CHECK(f != NULL);
    if (f) fclose(f);
}

static int captured_stderr_fd = -1;

static void capture_stderr_begin(void) {
    fflush(stderr);
    captured_stderr_fd = dup(STDERR_FILENO);
    int fd = open("stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    dup2(fd, STDERR_FILENO);
    close(fd);
}

static void capture_stderr_end(char *out, size_t n) {
    fflush(stderr);
    if (captured_stderr_fd >= 0) {
        dup2(captured_stderr_fd, STDERR_FILENO);
        close(captured_stderr_fd);
        captured_stderr_fd = -1;
    }
    out[0] = 0;
    FILE *f = fopen("stderr.txt", "r");
    if (!f) return;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
}

static void expect_config_error(const char *json, const char *expected) {
    write_file("config.json", json);
    char err[256];
    capture_stderr_begin();
    Config cfg;
    int rc = config_load("config.json", &cfg);
    capture_stderr_end(err, sizeof(err));
    CHECK_LL(rc, -1);
    CHECK(strcmp(err, expected) == 0);
    config_free(&cfg);
}

static void test_config_valid_full(void) {
    write_file("config.json", "{\"api_id\":123456,\"api_hash\":\"dummy\","
        "\"dest_channel\":\"@me\",\"source_channels\":[\"@a\",\"b\"],"
        "\"history_file\":\"h.txt\",\"forward_delay_sec\":7,"
        "\"history_window_hours\":3,\"enable_sequential_forwarding\":true,"
        "\"sequence_direction\":\"desc\"}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.api_id, 123456);
    CHECK(strcmp(cfg.api_hash, "dummy") == 0);
    CHECK(strcmp(cfg.dest_channel, "@me") == 0);
    CHECK_LL(cfg.num_sources, 2);
    CHECK(strcmp(cfg.source_channels[0], "@a") == 0);
    CHECK(strcmp(cfg.source_channels[1], "b") == 0);
    CHECK(strcmp(cfg.history_file, "h.txt") == 0);
    CHECK_LL(cfg.forward_delay_sec, 7);
    CHECK_LL(cfg.history_window_hours, 3);
    CHECK_LL(cfg.enable_sequential_forwarding, 1);
    CHECK_LL(cfg.sequence_direction, 1);
    config_free(&cfg);
}

static void test_config_defaults(void) {
    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[\"one\"]}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK(strcmp(cfg.history_file, "history.txt") == 0);
    CHECK_LL(cfg.forward_delay_sec, 1);
    CHECK_LL(cfg.history_window_hours, 24);
    CHECK_LL(cfg.enable_sequential_forwarding, 0);
    CHECK_LL(cfg.sequence_direction, 0);
    config_free(&cfg);
}

static void test_config_missing_file(void) {
    remove("absent.json");
    char err[256];
    capture_stderr_begin();
    Config cfg;
    int rc = config_load("absent.json", &cfg);
    capture_stderr_end(err, sizeof(err));
    CHECK_LL(rc, -1);
    CHECK(strcmp(err, "Cannot open absent.json\n") == 0);
    config_free(&cfg);
}

static void test_config_invalid_json(void) {
    expect_config_error("{ not json", "Invalid JSON in config.json\n");
}

static void test_config_empty_file(void) {
    expect_config_error("", "");
}

static void test_config_missing_api_id(void) {
    expect_config_error("{\"api_hash\":\"x\",\"dest_channel\":\"d\",\"source_channels\":[\"a\"]}",
                        "Missing api_id\n");
}

static void test_config_missing_api_hash(void) {
    expect_config_error("{\"api_id\":1,\"dest_channel\":\"d\",\"source_channels\":[\"a\"]}",
                        "Missing api_hash\n");
}

static void test_config_missing_dest_channel(void) {
    expect_config_error("{\"api_id\":1,\"api_hash\":\"x\",\"source_channels\":[\"a\"]}",
                        "Missing dest_channel\n");
}

static void test_config_missing_sources(void) {
    expect_config_error("{\"api_id\":1,\"api_hash\":\"x\",\"dest_channel\":\"d\"}",
                        "Missing source_channels array\n");
}

static void test_config_key_case_insensitive(void) {
    write_file("config.json", "{\"API_ID\":2,\"API_HASH\":\"y\","
        "\"DEST_CHANNEL\":\"d\",\"SOURCE_CHANNELS\":[\"a\"]}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.api_id, 2);
    CHECK(strcmp(cfg.api_hash, "y") == 0);
    config_free(&cfg);
}

static void test_config_boolean_is_strict(void) {
    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[\"a\"],"
        "\"enable_sequential_forwarding\":\"true\"}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.enable_sequential_forwarding, 0);
    config_free(&cfg);
}

static void test_config_direction_values(void) {
    Config cfg;
    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[\"a\"],"
        "\"sequence_direction\":\"ASC\"}");
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.sequence_direction, 0);
    config_free(&cfg);

    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[\"a\"],"
        "\"sequence_direction\":\"desc\"}");
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.sequence_direction, 1);
    config_free(&cfg);
}

static void test_config_non_string_sources(void) {
    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[1,null,\"ok\"]}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.num_sources, 3);
    CHECK(strcmp(cfg.source_channels[0], "") == 0);
    CHECK(strcmp(cfg.source_channels[1], "") == 0);
    CHECK(strcmp(cfg.source_channels[2], "ok") == 0);
    config_free(&cfg);
}

static void test_config_fractional_api_id(void) {
    write_file("config.json", "{\"api_id\":1.9,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[\"a\"]}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.api_id, 1);
    config_free(&cfg);
}

static void test_config_empty_sources(void) {
    write_file("config.json", "{\"api_id\":1,\"api_hash\":\"x\","
        "\"dest_channel\":\"d\",\"source_channels\":[]}");
    Config cfg;
    CHECK_LL(config_load("config.json", &cfg), 0);
    CHECK_LL(cfg.num_sources, 0);
    config_free(&cfg);
}

static void set_sources(const char *const *names, int n) {
    rt.cfg.num_sources = n;
    rt.cfg.source_channels = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) rt.cfg.source_channels[i] = strdup(names[i]);
    rt.source_chat_ids = calloc(n, sizeof(long long));
}

static void test_resolve_next_requests(void) {
    const char *names[] = { "@a", "b" };
    set_sources(names, 2);
    rt.source_count = 2;
    rt.source_chat_ids[0] = -100111;
    rt.source_chat_ids[1] = -100222;

    resolve_next(&rt, NULL);
    CHECK(strcmp(captured_type, "searchPublicChat") == 0);
    CHECK_STR_CONTAINS(captured_payload, "\"username\":\"a\"");
    CHECK(strcmp(captured_extra, "resolve_0") == 0);
    CHECK_LL(rt.resolve_index, 1);
    CHECK_LL(rt.resolve_active, 1);

    resolve_next(&rt, NULL);
    CHECK_STR_CONTAINS(captured_payload, "\"username\":\"b\"");
    CHECK(strcmp(captured_extra, "resolve_1") == 0);
    CHECK_LL(rt.resolve_index, 2);
    CHECK_LL(rt.resolve_active, 1);

    resolve_next(&rt, NULL);
    CHECK_LL(rt.resolve_active, 0);
    CHECK(strcmp(captured_extra, "resolve_1") == 0);
}

static void test_resolve_source_handle(void) {
    const char *names[] = { "a" };
    set_sources(names, 1);
    char *argv[] = { NULL, NULL };
    cJSON *root = cJSON_Parse("{\"id\":-100123}");
    RequestId id = { REQ_RESOLVE_SOURCE, 0, 0 };
    resolve_source_handle(&rt, NULL, root, id, argv);
    cJSON_Delete(root);
    CHECK_LL(rt.source_chat_ids[0], -100123);
    CHECK_LL(rt.source_count, 1);
    CHECK_LL(rt.resolve_index, 1);
    CHECK_LL(rt.resolve_active, 1);
    resolve_next(&rt, NULL);
    CHECK_LL(rt.resolve_active, 0);
}

static void test_resolve_partial_summary(void) {
    const char *names[] = { "good", "bad" };
    set_sources(names, 2);
    rt.source_count = 1;
    rt.source_chat_ids[0] = -100111;
    rt.resolve_index = 2;
    char err[512];
    capture_stderr_begin();
    resolve_next(&rt, NULL);
    capture_stderr_end(err, sizeof(err));
    CHECK_LL(rt.resolve_active, 0);
    CHECK_STR_CONTAINS(err, "WARNING: 1 of 2 source channels failed to resolve:");
    CHECK_STR_CONTAINS(err, "bad");
    CHECK(strstr(err, "good") == NULL);
}

static void test_resolve_dest_handle(void) {
    const char *names[] = { "a" };
    set_sources(names, 1);
    rt.source_count = 1;
    rt.source_chat_ids[0] = -100111;
    rt.resolve_index = 1;
    char *argv[] = { NULL, NULL };
    cJSON *root = cJSON_Parse("{\"id\":-100999}");
    RequestId id = { REQ_RESOLVE_DEST, 0, 0 };
    resolve_dest_handle(&rt, NULL, root, id, argv);
    cJSON_Delete(root);
    CHECK_LL(rt.dest_chat_id, -100999);
    CHECK_LL(rt.dest_resolved, 1);
    CHECK_LL(rt.resolve_index, 1);
    CHECK(strcmp(captured_type, "searchPublicChat") == 0);
    CHECK(strcmp(captured_extra, "resolve_0") == 0);
}

static void test_getme_handle(void) {
    char *argv[] = { NULL, NULL };
    cJSON *root = cJSON_Parse("{\"id\":555}");
    RequestId id = { REQ_GETME, 0, 0 };
    getme_handle(&rt, NULL, root, id, argv);
    cJSON_Delete(root);
    CHECK(strcmp(captured_type, "createPrivateChat") == 0);
    CHECK_STR_CONTAINS(captured_payload, "\"user_id\":555");
    CHECK_STR_CONTAINS(captured_payload, "\"force\":true");
    CHECK(strcmp(captured_extra, "resolve_dest") == 0);
}

typedef void (*test_fn)(void);

static void run_test(const char *name, test_fn fn) {
    int before = failures;
    reset_state();
    fn();
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main(void) {
    char tmpl[] = "/tmp/tgf-tests-XXXXXX";
    char dir[256];

    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s", tmpl);
    if (chdir(dir) != 0) {
        perror("chdir");
        return 1;
    }

    rt.cfg.history_file = strdup("history.txt");
    if (!rt.cfg.history_file) return 1;

    run_test("empty_batch", test_empty_batch);
    run_test("single_message", test_single_message);
    run_test("two_messages_wire_order", test_two_messages_wire_order);
    run_test("sequential_asc", test_sequential_asc);
    run_test("sequential_desc", test_sequential_desc);
    run_test("reply_chain_and_parent_duplicate", test_reply_chain_and_parent_duplicate);
    run_test("album_batch_ordering", test_album_batch_ordering);
    run_test("album_split_across_batches", test_album_split_across_batches);
    run_test("album_ignores_reply_parent", test_album_ignores_reply_parent);
    run_test("reply_child_cap", test_reply_child_cap);
    run_test("queue_id_cap", test_queue_id_cap);
    run_test("enqueue_marks_history", test_enqueue_marks_history);
    run_test("history_window_expiry", test_history_window_expiry);
    run_test("history_prune_removes", test_history_prune_removes);
    run_test("history_prune_repairs_chains", test_history_prune_repairs_chains);
    run_test("history_prune_leaves_no_tmp", test_history_prune_leaves_no_tmp);
    run_test("history_load_skips_malformed", test_history_load_skips_malformed);
    run_test("history_normal_filters", test_history_normal_filters);
    run_test("history_normal_wire_order", test_history_normal_wire_order);
    run_test("history_normal_empty_noop", test_history_normal_empty_noop);
    run_test("history_sequential_new_status", test_history_sequential_new_status);
    run_test("backfill_collect_and_page", test_backfill_collect_and_page);
    run_test("backfill_echo_skip", test_backfill_echo_skip);
    run_test("backfill_no_dedupe", test_backfill_no_dedupe);
    run_test("backfill_completion_a", test_backfill_completion_a);
    run_test("backfill_completion_b", test_backfill_completion_b);
    run_test("backfill_reset_without_done", test_backfill_reset_without_done);
    run_test("backfill_total_zero", test_backfill_total_zero);
    run_test("backfill_straddle", test_backfill_straddle);
    run_test("history_handle_routes", test_history_handle_routes);
    run_test("tracker_load_fields", test_tracker_load_fields);
    run_test("tracker_save_roundtrip", test_tracker_save_roundtrip);
    run_test("tracker_capacity", test_tracker_capacity);
    run_test("tracker_pending_flush", test_tracker_pending_flush);
    run_test("pending_journal_roundtrip", test_pending_journal_roundtrip);
    run_test("pending_compact_on_flush", test_pending_compact_on_flush);
    run_test("pending_load_corrupt_skipped", test_pending_load_corrupt_skipped);
    run_test("backfill_resume_after_crash", test_backfill_resume_after_crash);
    run_test("dispatch_single", test_dispatch_single);
    run_test("dispatch_batch", test_dispatch_batch);
    run_test("dispatch_delay_gate", test_dispatch_delay_gate);
    run_test("dispatch_inflight_blocks", test_dispatch_inflight_blocks);
    run_test("forward_retry_then_drop", test_forward_retry_then_drop);
    run_test("forward_permanent_drop", test_forward_permanent_drop);
    run_test("forward_flood_pause", test_forward_flood_pause);
    run_test("forward_timeout_retries", test_forward_timeout_retries);
    run_test("forward_stale_ignored", test_forward_stale_ignored);
    run_test("forward_retry_tail_order", test_forward_retry_tail_order);
    run_test("queue_save_load_roundtrip", test_queue_save_load_roundtrip);
    run_test("queue_load_inflight_requeued", test_queue_load_inflight_requeued);
    run_test("queue_load_corrupt_skipped", test_queue_load_corrupt_skipped);
    run_test("queue_persist_on_dispatch_and_confirm", test_queue_persist_on_dispatch_and_confirm);
    run_test("config_valid_full", test_config_valid_full);
    run_test("config_defaults", test_config_defaults);
    run_test("config_missing_file", test_config_missing_file);
    run_test("config_invalid_json", test_config_invalid_json);
    run_test("config_empty_file", test_config_empty_file);
    run_test("config_missing_api_id", test_config_missing_api_id);
    run_test("config_missing_api_hash", test_config_missing_api_hash);
    run_test("config_missing_dest_channel", test_config_missing_dest_channel);
    run_test("config_missing_sources", test_config_missing_sources);
    run_test("config_key_case_insensitive", test_config_key_case_insensitive);
    run_test("config_boolean_is_strict", test_config_boolean_is_strict);
    run_test("config_direction_values", test_config_direction_values);
    run_test("config_non_string_sources", test_config_non_string_sources);
    run_test("config_fractional_api_id", test_config_fractional_api_id);
    run_test("config_empty_sources", test_config_empty_sources);
    run_test("resolve_next_requests", test_resolve_next_requests);
    run_test("resolve_source_handle", test_resolve_source_handle);
    run_test("resolve_partial_summary", test_resolve_partial_summary);
    run_test("resolve_dest_handle", test_resolve_dest_handle);
    run_test("getme_handle", test_getme_handle);

    reset_state();
    free(rt.cfg.history_file);
    rt.cfg.history_file = NULL;
    chdir("/");
    rmdir(dir);

    printf("\n%lld checks, %d failure(s)\n", (long long)checks, failures);
    return failures == 0 ? 0 : 1;
}
