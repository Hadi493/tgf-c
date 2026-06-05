static int authorized             = 0;
static int pending_req            = 0;
static long long *source_chat_ids = NULL;
static int source_count           = 0;

static void on_auth_state(void *client, const char *json);
static void on_res(void *client, const char *json, const char *extra);
static void on_update(void *client, const char *json);
static void on_err(void *client, const char *json);
