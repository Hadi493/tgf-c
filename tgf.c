#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <td/telegram/td_json_client.h>
#include "tgf.h"

#define DOTENV_LOAD
#include ".env.h"

#define CONFIG_LOAD
#include "config.h"

#define TOTAL_SOURCE (sizeof(source_channels) / sizeof(source_channels[0]))

static void send_request(void *client, const char *type, const char *payload, const char *extra) {
    char buff[4096];
    int n = snprintf(buff, sizeof(buff), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload, extra);
    if (n >= (int)sizeof(buff)) {
        fprintf(stderr, "WARNING: request truncated (%d > %zu)", n, sizeof(buff));
    }
    td_json_client_send(client, buff);
    pending_req++;
}

// WARNING: its an unoptimize implementation better to use jansson/cJSON library
static void *get_json_str(const char *json, const char *key, char *buff, size_t size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    size_t len = end - start;
    if (len >= size) len = size - 1;
    memcpy(buff, start, len);
    buff[len] = '\0';
    return buff;
}

static long long get_long_json(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;

    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    return atoll(p);
}

static void on_auth_state(void *client, const char *json_str) {
    if (strstr(json_str, "authorizationStateWaitTdlibParameters")) {
        char buff[1024];
        snprintf(buff, sizeof(buff),
                 "\"database_directory\":\"tdlib_db\","
                 "\"files_directory\":\"tdlib_files\","
                 "\"use_message_database\":true,"
                 "\"use_secret_chats\":false,"
                 "\"api_id\":%d,"
                 "\"api_hash\":\"%s\","
                 "\"system_language_code\":\"en\","
                 "\"device_model\":\"Desktop\","
                 "\"application_version\":\"1.0\"",
                 API_ID, API_HASH);
        send_request(client, "setTdlibParameters", buff, "auth_params");

    } else if (strstr(json_str, "authorizationStateWaitPhoneNumber")) {
        char phone_number[32] = {0};
        printf("Phone (+xxx): "); fflush(stdout);
        if (fgets(phone_number, sizeof(phone_number), stdin)) phone_number[strcspn(phone_number, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"phone_number\":\"%s\"", phone_number);
        send_request(client, "setAuthenticationPhoneNumber", payload, "auth_phone");

    } else if (strstr(json_str, "authorizationStateWaitCode")) {
        char code[32] = {0};
        printf("Code: "); fflush(stdout);
        if (fgets(code, sizeof(code), stdin)) code[strcspn(code, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"code\":\"%s\"", code);
        send_request(client, "checkAuthenticationCode", payload, "auth_code");

    } else if (strstr(json_str, "authorizationStateWaitPassword")) {
        char pw[128] = {0};
        printf("2FA Password: "); fflush(stdout);
        if (fgets(pw, sizeof(pw), stdin)) pw[strcspn(pw, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"password\":\"%s\"", pw);
        send_request(client, "checkAuthenticationPassword", payload, "auth_pw");

    } else if (strstr(json_str, "authorizationStateReady")) {
        authorized = 1;
        printf("\n✓ Authorized! Start monitoring...\n");

        for (int i = 0; i < (int)TOTAL_SOURCE; i++) {
            char extra[32];
            snprintf(extra, sizeof(extra), "resolve_%d", i);

            char payload[256];
            snprintf(payload, sizeof(payload), "\"username\":\"%s\"", source_channels[i] + 1);
            send_request(client, "searchPublicChat", payload, extra);
        }

    }
}

static void on_response(void *client, const char *json, const char *extra) {
    (void)client;
    pending_req--;

    if (strcmp(extra, "auth_params") == 0) {
        printf("\n [settdlibparameters done ] \n");
    } else if (strcmp(extra, "auth_phone") == 0 || strcmp(extra, "auth_code") == 0 || strcmp(extra, "auth_pw") == 0) {
        // wait
    } else if (strncmp(extra, "resolve_", 8) == 0) {
        char title[256] = "?";
        get_json_str(json, "title", title, sizeof(title));
        long long chat_id = get_long_json(json, "id");
        int idx = atoi(extra + 8);
        printf(" Resolved %s -> id=%lld (\"%s\")\n", source_channels[idx], chat_id, title);
        source_chat_ids[idx] = chat_id;
        source_count++;

        if (source_count == (int)TOTAL_SOURCE) printf("\n=== Monitoring %d channels ===\n\n", source_count);
    } else if (strncmp(extra, "history_", 8) == 0) {
        long long chat_id = atoll(extra + 8);
        printf(" [History for chat %lld received, processing...]\n", chat_id);

        // TODO: parse messages array, check history.txt, forward new ones

        char next_extra[64];
        snprintf(next_extra, sizeof(next_extra), "history_%lld", chat_id);
        char payload[256];
        snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"limit\":%d,\"from_message_id\":0,\"offset\":0", chat_id, MSG_LIMIT);
        // send_request(client, "getChatHistory", payload, next_extra); // Wait for implementation of parsing
    }
}

static void on_update(void *client, const char *json) {
    if (strstr(json, "\"@type\":\"updateAuthorizationState\"")) on_auth_state(client, json);
}

static void on_error(void *client, const char *json) {
    (void)client;
    pending_req--;

    char extra[256] = {0};
    get_json_str(json, "@extra", extra, sizeof(extra));
    char msg[256] = {0};
    get_json_str(json, "message", msg, sizeof(msg));
    int code = (int)get_long_json(json, "code");

    fprintf(stderr, "✗ ERROR [extra=%s] code=%d: %s\n", extra[0] ? extra : "(none)", code, msg);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    void *client = td_json_client_create();
    if (!client) fprintf(stderr, "Failed to create TDLib client\n");

    td_json_client_send(client, "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":0}");

    source_chat_ids = calloc(TOTAL_SOURCE, sizeof(long long));
    if (!source_chat_ids) {
        fprintf(stderr, "Out of memory\n");
        td_json_client_destroy(client);
    }

    // TODO: Load History

    td_json_client_send(client, "{\"@type\":\"getAuthorizationState\"}");

    time_t last_poll = 0;

    while (true) {
        const char *result = td_json_client_receive(client, 1.0);
        if (result) {
            char type[64] = {0};
            char extra[256] = {0};
            get_json_str(result, "@type", type, sizeof(type));
            get_json_str(result, "@extra", extra, sizeof(extra));

            if (strcmp(type, "updateAuthorizationState") == 0) {
                on_auth_state(client, result);
            } else if (strcmp(type, "error") == 0) {
                on_error(client, result);
            } else if (extra[0]) {
                on_response(client, result, extra);
            } else {
                on_update(client, result);
            }
        }

        if (authorized && source_count == (int)TOTAL_SOURCE) {
            time_t now = time(NULL);
            if (now - last_poll >= POLL_INTERVAL) {
                last_poll = now;

                for (int i = 0; i < (int)TOTAL_SOURCE; ++i) {
                    long long chat_id = source_chat_ids[i];
                    if (chat_id == 0) continue;

                    char extra[64];
                    snprintf(extra, sizeof(extra), "history_%lld", chat_id);
                    char payload[512];
                    snprintf(payload, sizeof(payload), "\"chat_id\":%lld,\"limit\":%d,\"from_message_id\":0,\"offset\":0", chat_id, MSG_LIMIT);
                    send_request(client, "getChatHistory", payload, extra);
                }
            }
        }
    }

    free(source_chat_ids);
    td_json_client_destroy(client);
    return 0;
}
