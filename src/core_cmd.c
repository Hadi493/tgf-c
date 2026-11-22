#include "tgf.h"
#include "core_priv.h"

#include <stdlib.h>
#include <string.h>

/* ---- commands ---------------------------------------------------------- */

static tgf_status cmd_push(tgf_ctx *c, int kind, const char *s, int64_t id, int enabled) {
    mtx_lock(&c->cmd_mtx);
    if (c->cmd_tail - c->cmd_head == c->cmd_cap) {
        mtx_unlock(&c->cmd_mtx);
        return TGF_ERR_BUSY;
    }
    tgf_cmd *k = &c->cmd[c->cmd_tail % c->cmd_cap];
    k->kind = kind;
    k->s = s ? strdup(s) : NULL;
    k->id = id;
    k->enabled = enabled;
    k->has_cfg = 0;
    memset(&k->cfg, 0, sizeof(k->cfg));
    c->cmd_tail++;
    mtx_unlock(&c->cmd_mtx);
    return TGF_OK;
}

static int count_resolved(const Runtime *rt) {
    int n = 0;
    for (int i = 0; i < rt->cfg.num_sources; i++)
        if (rt->source_chat_ids[i] != 0) n++;
    return n;
}

static void disabled_add(Runtime *rt, const char *handle) {
    if (source_disabled(&rt->cfg, handle)) return;
    int n = rt->cfg.num_disabled;
    char **a = realloc(rt->cfg.disabled_channels, (n + 1) * sizeof(char *));
    if (!a) return;
    rt->cfg.disabled_channels = a;
    a[n] = strdup(handle);
    if (a[n]) rt->cfg.num_disabled = n + 1;
}

static void disabled_remove(Runtime *rt, const char *handle) {
    for (int i = 0; i < rt->cfg.num_disabled; i++) {
        if (!handle_matches(rt->cfg.disabled_channels[i], handle)) continue;
        free(rt->cfg.disabled_channels[i]);
        for (int j = i; j < rt->cfg.num_disabled - 1; j++)
            rt->cfg.disabled_channels[j] = rt->cfg.disabled_channels[j + 1];
        rt->cfg.num_disabled--;
        if (rt->cfg.num_disabled == 0) {
            free(rt->cfg.disabled_channels);
            rt->cfg.disabled_channels = NULL;
        }
        return;
    }
}

static void exec_cmd(tgf_ctx *c, void *client, const tgf_cmd *k) {
    Runtime *rt = &c->rt;
    switch (k->kind) {
    case CMD_PHONE:
        auth_submit(rt, client, TGF_AUTH_PHONE, k->s);
        break;
    case CMD_CODE:
        auth_submit(rt, client, TGF_AUTH_CODE, k->s);
        break;
    case CMD_PW:
        auth_submit(rt, client, TGF_AUTH_PASSWORD, k->s);
        break;
    case CMD_PAUSE:
        atomic_store(&c->paused, true);
        {
            tgf_event *e = ev_new(TGF_EV_PIPELINE_STATE);
            if (e) {
                e->pipe = TGF_PIPE_PAUSED;
                ev_emit(c, e);
            }
        }
        break;
    case CMD_RESUME:
        atomic_store(&c->paused, false);
        {
            tgf_event *e = ev_new(TGF_EV_PIPELINE_STATE);
            if (e) {
                e->pipe = TGF_PIPE_RUNNING;
                ev_emit(c, e);
            }
        }
        break;
    case CMD_ADD_SOURCE: {
        for (int i = 0; i < rt->cfg.num_sources; i++) {
            if (handle_matches(rt->cfg.source_channels[i], k->s)) return;
        }
        int old = rt->cfg.num_sources;
        char *handle = strdup(k->s);
        char **ns = malloc((old + 1) * sizeof(char *));
        long long *ids = malloc((old + 1) * sizeof(long long));
        if (!handle || !ns || !ids) {
            free(handle);
            free(ns);
            free(ids);
            ui_oom("OOM: add source");
            break;
        }
        for (int i = 0; i < old; i++) {
            ns[i] = rt->cfg.source_channels[i];
            ids[i] = rt->source_chat_ids[i];
        }
        ns[old] = handle;
        ids[old] = 0;
        free(rt->cfg.source_channels);
        free(rt->source_chat_ids);
        rt->cfg.source_channels = ns;
        rt->source_chat_ids = ids;
        rt->cfg.num_sources = old + 1;
        rt->source_count = count_resolved(rt);
        rt->resolve_index = old;
        rt->resolve_active = 0;
        if (rt->dest_resolved) resolve_next(rt, client);
        break;
    }
    case CMD_REMOVE_SOURCE: {
        int idx = -1;
        if (k->s)
            idx = source_index_by_handle(&rt->cfg, k->s);
        else
            for (int i = 0; i < rt->cfg.num_sources; i++)
                if (rt->source_chat_ids[i] == k->id) {
                    idx = i;
                    break;
                }
        if (idx < 0) break;
        int was_resolved = rt->source_chat_ids[idx] != 0;
        disabled_remove(rt, rt->cfg.source_channels[idx]);
        free(rt->cfg.source_channels[idx]);
        for (int j = idx; j < rt->cfg.num_sources - 1; j++) {
            rt->cfg.source_channels[j] = rt->cfg.source_channels[j + 1];
            rt->source_chat_ids[j] = rt->source_chat_ids[j + 1];
        }
        rt->cfg.num_sources--;
        if (was_resolved) rt->source_count--;
        rt->src_name = "unknown";
        if (rt->cfg.num_sources == 0) {
            free(rt->cfg.source_channels);
            rt->cfg.source_channels = NULL;
            free(rt->source_chat_ids);
            rt->source_chat_ids = NULL;
        } else {
            char **ns = realloc(rt->cfg.source_channels, rt->cfg.num_sources * sizeof(char *));
            long long *ids = realloc(rt->source_chat_ids, rt->cfg.num_sources * sizeof(long long));
            if (ns) rt->cfg.source_channels = ns;
            if (ids) rt->source_chat_ids = ids;
        }
        rt->resolve_index = rt->cfg.num_sources;
        rt->resolve_active = 0;
        break;
    }
    case CMD_SET_ENABLED: {
        int idx = -1;
        if (k->s)
            idx = source_index_by_handle(&rt->cfg, k->s);
        else
            for (int i = 0; i < rt->cfg.num_sources; i++)
                if (rt->source_chat_ids[i] == k->id) {
                    idx = i;
                    break;
                }
        if (idx < 0) break;
        const char *handle = rt->cfg.source_channels[idx];
        int is_disabled = source_disabled(&rt->cfg, handle);
        if (k->enabled) {
            if (!is_disabled && rt->source_chat_ids[idx] != 0) break;
            disabled_remove(rt, handle);
            rt->source_chat_ids[idx] = 0;
            rt->resolve_index = idx;
            rt->resolve_active = 0;
            if (rt->dest_resolved) resolve_next(rt, client);
        } else {
            if (is_disabled) break;
            disabled_add(rt, handle);
            if (rt->source_chat_ids[idx]) rt->source_count--;
            rt->source_chat_ids[idx] = 0;
            {
                tgf_event *e = ev_new(TGF_EV_SOURCE_STATE);
                if (e) {
                    e->src_state = TGF_SRC_DISABLED;
                    e->total = rt->source_count;
                    e->source = strdup(handle);
                    ev_emit(c, e);
                }
            }
        }
        break;
    }
    case CMD_RESCAN: {
        int idx = -1;
        if (k->s)
            idx = source_index_by_handle(&rt->cfg, k->s);
        else
            for (int i = 0; i < rt->cfg.num_sources; i++)
                if (rt->source_chat_ids[i] == k->id) {
                    idx = i;
                    break;
                }
        if (idx < 0 || rt->source_chat_ids[idx] == 0) break;
        long long id = rt->source_chat_ids[idx];
        tracker_store_set_backfill_done(&rt->tracker, id, 0);
        tracker_store_set_scan_from(&rt->tracker, id, 0);
        tracker_store_save(&rt->tracker);
        break;
    }
    case CMD_APPLY_CONFIG: {
        runtime_config_load(rt, &k->cfg);
        tgf_config_free(&c->cfg);
        config_copy(&c->cfg, &k->cfg);
        free(rt->source_chat_ids);
        rt->source_chat_ids =
            calloc(rt->cfg.num_sources > 0 ? rt->cfg.num_sources : 1, sizeof(long long));
        if (!rt->source_chat_ids) {
            ui_oom("OOM: apply config");
            rt->fatal = 1;
            break;
        }
        rt->source_count = 0;
        rt->resolve_index = 0;
        rt->resolve_active = 0;
        rt->dest_resolved = 0;
        rt->dest_chat_id = 0;
        if (rt->authorized) resolve_destination(rt, client);
        break;
    }
    case CMD_LOGOUT:
        transport_send(rt, client, "logOut", "", (RequestId){REQ_NONE, 0, 0});
        break;
    }
}

void drain_cmds(tgf_ctx *c, void *client) {
    for (;;) {
        mtx_lock(&c->cmd_mtx);
        if (c->cmd_head == c->cmd_tail) {
            mtx_unlock(&c->cmd_mtx);
            break;
        }
        tgf_cmd k = c->cmd[c->cmd_head % c->cmd_cap];
        c->cmd_head++;
        mtx_unlock(&c->cmd_mtx);
        exec_cmd(c, client, &k);
        if (k.has_cfg) tgf_config_free(&k.cfg);
        if (k.s) {
            memset(k.s, 0, strlen(k.s));
            free(k.s);
        }
    }
}

/* ---- command entry points --------------------------------------------- */

tgf_status tgf_submit_phone(tgf_ctx *c, const char *p) {
    return c && p ? cmd_push(c, CMD_PHONE, p, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_submit_code(tgf_ctx *c, const char *v) {
    return c && v ? cmd_push(c, CMD_CODE, v, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_submit_password(tgf_ctx *c, const char *p) {
    return c && p ? cmd_push(c, CMD_PW, p, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_pause(tgf_ctx *c) {
    return c ? cmd_push(c, CMD_PAUSE, NULL, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_resume(tgf_ctx *c) {
    return c ? cmd_push(c, CMD_RESUME, NULL, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_add_source(tgf_ctx *c, const char *h) {
    return c && h ? cmd_push(c, CMD_ADD_SOURCE, h, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_remove_source(tgf_ctx *c, int64_t id) {
    return c ? cmd_push(c, CMD_REMOVE_SOURCE, NULL, id, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_set_source_enabled(tgf_ctx *c, int64_t id, bool e) {
    return c ? cmd_push(c, CMD_SET_ENABLED, NULL, id, e) : TGF_ERR_INVALID;
}
tgf_status tgf_rescan_source(tgf_ctx *c, int64_t id) {
    return c ? cmd_push(c, CMD_RESCAN, NULL, id, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_remove_source_by_handle(tgf_ctx *c, const char *h) {
    return c && h ? cmd_push(c, CMD_REMOVE_SOURCE, h, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_set_source_enabled_by_handle(tgf_ctx *c, const char *h, bool e) {
    return c && h ? cmd_push(c, CMD_SET_ENABLED, h, 0, e) : TGF_ERR_INVALID;
}
tgf_status tgf_rescan_source_by_handle(tgf_ctx *c, const char *h) {
    return c && h ? cmd_push(c, CMD_RESCAN, h, 0, 0) : TGF_ERR_INVALID;
}
tgf_status tgf_apply_config(tgf_ctx *c, const tgf_config *cfg) {
    if (!c || !cfg) return TGF_ERR_INVALID;
    mtx_lock(&c->cmd_mtx);
    if (c->cmd_tail - c->cmd_head == c->cmd_cap) {
        mtx_unlock(&c->cmd_mtx);
        return TGF_ERR_BUSY;
    }
    tgf_cmd *k = &c->cmd[c->cmd_tail % c->cmd_cap];
    k->kind = CMD_APPLY_CONFIG;
    k->s = NULL;
    k->id = 0;
    k->enabled = 0;
    config_copy(&k->cfg, cfg);
    k->has_cfg = 1;
    c->cmd_tail++;
    mtx_unlock(&c->cmd_mtx);
    return TGF_OK;
}
tgf_status tgf_logout(tgf_ctx *c) {
    return c ? cmd_push(c, CMD_LOGOUT, NULL, 0, 0) : TGF_ERR_INVALID;
}
