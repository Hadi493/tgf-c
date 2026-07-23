#ifndef TGF_H_
#define TGF_H_

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

#define POLL_INTERVAL 5
#define MSG_LIMIT     10000

#define MAX_SEQ_TRACKER 128

typedef struct {
    long long id, album_id, reply_to_msg_id, date;
} PendingMsg;

typedef struct {
    long long chat_id;
    long long last_msg_id;
    long long last_date;
    long long scan_from_id;
    int       backfill_done;
    PendingMsg *pending;
    int       pending_count;
    int       pending_cap;
} SeqEntry;

#define MAX_FWD_IDS 256

typedef struct {
    long long src_chat_id;
    long long ids[MAX_FWD_IDS];
    int       count;
} ForwardJob;

#define APPEND_ID(ids, pos, size, val) do { \
    int n_ = snprintf((ids) + (pos), (size) - (pos), "%s%lld", \
        (pos) == 0 ? "" : ",", (long long)(val)); \
    if (n_ > 0) (pos) += n_; \
} while (0)

extern int      history_window_hours;
extern int      enable_sequential_forwarding;
extern int      sequence_direction;

extern int      api_id;
extern char    *api_hash;
extern char   **source_channels;
extern int      num_sources;
extern char    *dest_channel;
extern char    *history_file;
extern int      forward_delay_sec;

extern int       authorized;
extern int       dest_resolved;
extern long long dest_chat_id;
extern long long *source_chat_ids;
extern char      *src_name;
extern int       source_count;
extern int       pending_req;

extern char **history_set;
extern int   history_set_size;
extern int   history_count;
extern time_t *history_time;

extern SeqEntry seq_tracker[MAX_SEQ_TRACKER];
extern int      seq_tracker_count;

extern ForwardJob *fwd_queue;
extern int        fwd_queue_count;
extern int        fwd_queue_cap;
extern double     fwd_last_time;

extern volatile bool keep_running;
extern bool dashboard_active;
extern char status_msg[256];

// --- functions ---

void set_status(const char *msg);

void send_req(void *client, const char *type, const char *payload, const char *extra);

void history_load(void);
int  history_has(long long chat_id, long long msg_id);
void history_add(long long chat_id, long long msg_id);
void history_prune(void);

void seq_tracker_load(void);
void seq_tracker_save(void);
void seq_tracker_set(long long chat_id, long long msg_id, long long date);
long long seq_tracker_get_scan_from(long long chat_id);
void seq_tracker_set_scan_from(long long chat_id, long long msg_id);
int seq_tracker_is_backfill_done(long long chat_id);
void seq_tracker_set_backfill_done(long long chat_id, int done);
void seq_tracker_pending_append(long long chat_id, PendingMsg msg);
void flush_pending(long long src_chat_id);

void fwd_queue_push(long long src_chat_id, const long long *ids, int count);
int  process_fwd_queue(void *client);
void process_msgs(long long src_chat_id, PendingMsg *msgs, int count);

#endif
