#ifndef TGF_TRANSPORT_H
#define TGF_TRANSPORT_H

/* Low-level TDLib JSON seam. The real implementation wraps td_json_client_*;
 * a fake implementation (tests) replays scripted responses and records sends.
 * Immutable vtable: safe to share across threads. */
typedef struct {
    void *(*create)(void);
    void (*send)(void *client, const char *json);
    const char *(*receive)(void *client, double timeout);
    /* Synchronous request. Result is owned by the transport, valid until the
     * next execute() call on the same client; the caller must not free it. */
    const char *(*execute)(void *client, const char *json);
    void (*destroy)(void *client);
} Transport;

extern const Transport transport_tdlib;

#endif
