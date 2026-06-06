#define POLL_INTERVAL   5
#define MSG_LIMIT       20

static int      history_window_hours = 24;

static int      api_id;
static char    *api_hash;
static char   **source_channels;
static int      num_sources;
static char    *dest_channel;
static char    *history_file;
static int      forward_delay_sec;

static int       authorized       = 0;
static int       dest_resolved    = 0;
static long long dest_chat_id     = 0;
static long long *source_chat_ids = NULL;
static int       source_count     = 0;
static int       pending_req      = 0;

static char   **history_keys     = NULL;
static int      history_count    = 0;

#define MAX_FWD_IDS 256


typedef struct {
    long long src_chat_id;
    long long ids[MAX_FWD_IDS];
    int       count;
} ForwardJob;

static ForwardJob fwd_queue[1024];
static int        fwd_queue_count = 0;
static double     fwd_last_time   = 0;

static void on_auth_state(void *client, const char *json);
static void on_response(void *client, const char *json, const char *extra);
static void on_update(void *client, const char *json);
static void on_error(void *client, const char *json);
static void resolve_next(void *client);
static void poll_channels(void *client);
static void handle_history_response(void *client, const char *json, long long src_chat_id);


#define APPEND_ID(ids, pos, size, val) do { \
    int n_ = snprintf((ids) + (pos), (size) - (pos), "%s%lld", \
        (pos) == 0 ? "" : ",", (long long)(val)); \
    if (n_ > 0) (pos) += n_; \
} while (0)
