#include "tgf.h"

#define TD_JSON_CLIENT_HEADER <td/telegram/td_json_client.h>
#include TD_JSON_CLIENT_HEADER

static void *tdlib_create(void) { return td_json_client_create(); }
static void tdlib_send(void *client, const char *json) { td_json_client_send(client, json); }
static const char *tdlib_receive(void *client, double timeout) { return td_json_client_receive(client, timeout); }
static const char *tdlib_execute(void *client, const char *json) { return td_json_client_execute(client, json); }
static void tdlib_destroy(void *client) { td_json_client_destroy(client); }

const tgf_transport transport_tdlib = {
    tdlib_create, tdlib_send, tdlib_receive, tdlib_execute, tdlib_destroy
};

void transport_destroy(void *client) {
    td_json_client_destroy(client);
}
