#include "tgf.h"
#include "core_priv.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- config ------------------------------------------------------------ */

static char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    long got = (long)fread(buf, 1, n, f);
    fclose(f);
    if (got < 0) {
        free(buf);
        return NULL;
    }
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

static char g_cfg_error[160];

const char *tgf_config_error(void) {
    return g_cfg_error[0] ? g_cfg_error : NULL;
}

static tgf_status cfg_fail(tgf_status st, const char *msg) {
    snprintf(g_cfg_error, sizeof(g_cfg_error), "%s", msg);
    return st;
}

static char **str_array_dup(char *const *src, int n) {
    if (n <= 0) return NULL;
    char **a = calloc(n, sizeof(char *));
    if (!a) return NULL;
    for (int i = 0; i < n; i++) a[i] = strdup(src[i] ? src[i] : "");
    return a;
}

static void str_array_free(char **a, int n) {
    if (!a) return;
    for (int i = 0; i < n; i++) free(a[i]);
    free(a);
}

static int config_from_cjson(tgf_config *out, const cJSON *root) {
    memset(out, 0, sizeof(*out));
    out->forward_delay_sec = 1;
    out->history_window_hours = 24;
    const cJSON *v;
    v = cJSON_GetObjectItem(root, "api_id");
    if (!v || !cJSON_IsNumber(v)) return cfg_fail(TGF_ERR_PARSE, "Missing api_id"), 0;
    out->api_id = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "api_hash");
    if (!v || !cJSON_IsString(v)) return cfg_fail(TGF_ERR_PARSE, "Missing api_hash"), 0;
    out->api_hash = strdup(v->valuestring);
    v = cJSON_GetObjectItem(root, "dest_channel");
    if (!v || !cJSON_IsString(v)) return cfg_fail(TGF_ERR_PARSE, "Missing dest_channel"), 0;
    out->dest_channel = strdup(v->valuestring);
    v = cJSON_GetObjectItem(root, "history_file");
    out->history_file = strdup(v && cJSON_IsString(v) ? v->valuestring : "history.txt");
    if (!out->api_hash || !out->dest_channel || !out->history_file)
        return cfg_fail(TGF_ERR_OOM, "OOM"), 0;
    v = cJSON_GetObjectItem(root, "forward_delay_sec");
    if (v && cJSON_IsNumber(v)) out->forward_delay_sec = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "history_window_hours");
    if (v && cJSON_IsNumber(v)) out->history_window_hours = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "enable_sequential_forwarding");
    if (v && cJSON_IsBool(v)) out->enable_sequential_forwarding = v->valueint;
    v = cJSON_GetObjectItem(root, "enable_ordering");
    if (v && cJSON_IsBool(v)) out->enable_ordering = v->valueint;
    v = cJSON_GetObjectItem(root, "sequence_direction");
    out->sequence_direction =
        (v && cJSON_IsString(v) && strcmp(v->valuestring, "desc") == 0) ? 1 : 0;
    v = cJSON_GetObjectItem(root, "source_channels");
    if (!v || !cJSON_IsArray(v)) return cfg_fail(TGF_ERR_PARSE, "Missing source_channels array"), 0;
    out->num_sources = cJSON_GetArraySize(v);
    out->source_channels = calloc(out->num_sources > 0 ? out->num_sources : 1, sizeof(char *));
    if (!out->source_channels) return cfg_fail(TGF_ERR_OOM, "OOM"), 0;
    for (int i = 0; i < out->num_sources; i++) {
        const cJSON *el = cJSON_GetArrayItem(v, i);
        out->source_channels[i] = strdup(el && cJSON_IsString(el) ? el->valuestring : "");
        if (!out->source_channels[i]) return cfg_fail(TGF_ERR_OOM, "OOM"), 0;
    }
    v = cJSON_GetObjectItem(root, "disabled_channels");
    if (v && cJSON_IsArray(v)) {
        out->num_disabled = cJSON_GetArraySize(v);
        out->disabled_channels =
            calloc(out->num_disabled > 0 ? out->num_disabled : 1, sizeof(char *));
        if (!out->disabled_channels) return cfg_fail(TGF_ERR_OOM, "OOM"), 0;
        for (int i = 0; i < out->num_disabled; i++) {
            const cJSON *el = cJSON_GetArrayItem(v, i);
            out->disabled_channels[i] = strdup(el && cJSON_IsString(el) ? el->valuestring : "");
            if (!out->disabled_channels[i]) return cfg_fail(TGF_ERR_OOM, "OOM"), 0;
        }
    }
    g_cfg_error[0] = 0;
    return 1;
}

tgf_status tgf_config_load(const char *path, tgf_config *out) {
    if (!path || !out) return TGF_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    long len;
    char *buf = read_file(path, &len);
    if (!buf) {
        char b[96];
        snprintf(b, sizeof(b), "Cannot open %.60s", path);
        return cfg_fail(TGF_ERR_IO, b);
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        char b[96];
        snprintf(b, sizeof(b), "Invalid JSON in %.60s", path);
        return cfg_fail(TGF_ERR_PARSE, b);
    }
    int ok = config_from_cjson(out, root);
    cJSON_Delete(root);
    if (!ok) {
        tgf_config_free(out);
        return TGF_ERR_PARSE;
    }
    return TGF_OK;
}

static cJSON *config_to_json(const tgf_config *cfg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "api_id", cfg->api_id);
    cJSON_AddStringToObject(r, "api_hash", cfg->api_hash ? cfg->api_hash : "");
    cJSON_AddStringToObject(r, "dest_channel", cfg->dest_channel ? cfg->dest_channel : "");
    cJSON_AddStringToObject(r, "history_file",
                            cfg->history_file ? cfg->history_file : "history.txt");
    cJSON_AddNumberToObject(r, "forward_delay_sec", cfg->forward_delay_sec);
    cJSON_AddNumberToObject(r, "history_window_hours", cfg->history_window_hours);
    cJSON_AddBoolToObject(r, "enable_sequential_forwarding", cfg->enable_sequential_forwarding);
    cJSON_AddBoolToObject(r, "enable_ordering", cfg->enable_ordering);
    cJSON_AddStringToObject(r, "sequence_direction", cfg->sequence_direction ? "desc" : "asc");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < cfg->num_sources; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(cfg->source_channels[i]));
    cJSON_AddItemToObject(r, "source_channels", arr);
    if (cfg->num_disabled > 0) {
        cJSON *dis = cJSON_CreateArray();
        for (int i = 0; i < cfg->num_disabled; i++)
            cJSON_AddItemToArray(dis, cJSON_CreateString(cfg->disabled_channels[i]));
        cJSON_AddItemToObject(r, "disabled_channels", dis);
    }
    return r;
}

tgf_status tgf_config_save(const char *path, const tgf_config *cfg) {
    if (!path || !cfg) return TGF_ERR_INVALID;
    /* Preserve unknown keys: start from the existing file's JSON if it parses. */
    cJSON *root = NULL;
    long len;
    char *existing = read_file(path, &len);
    if (existing) {
        root = cJSON_Parse(existing);
        free(existing);
        if (!root) return TGF_ERR_PARSE; /* never overwrite an unparseable file */
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return TGF_ERR_OOM;

    const char *keys[] = {"api_id",
                          "api_hash",
                          "dest_channel",
                          "history_file",
                          "forward_delay_sec",
                          "history_window_hours",
                          "enable_sequential_forwarding",
                          "enable_ordering",
                          "sequence_direction",
                          "source_channels",
                          "disabled_channels"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        cJSON_DeleteItemFromObject(root, keys[i]);
    cJSON *fresh = config_to_json(cfg);
    if (!fresh) {
        cJSON_Delete(root);
        return TGF_ERR_OOM;
    }
    cJSON *child, *next;
    for (child = fresh->child; child; child = next) {
        next = child->next;
        cJSON_DetachItemViaPointer(fresh, child);
        cJSON_AddItemToObject(root, child->string, child);
    }
    cJSON_Delete(fresh);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return TGF_ERR_OOM;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        free(text);
        return TGF_ERR_IO;
    }
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        free(text);
        remove(tmp);
        return TGF_ERR_IO;
    }
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    fsync(fileno(f));
    if (fclose(f) != 0) w = 0;
    free(text);
    if (w != n) {
        remove(tmp);
        return TGF_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return TGF_ERR_IO;
    }
    return TGF_OK;
}

void tgf_config_free(tgf_config *cfg) {
    if (!cfg) return;
    free(cfg->api_hash);
    free(cfg->dest_channel);
    free(cfg->history_file);
    str_array_free(cfg->source_channels, cfg->num_sources);
    str_array_free(cfg->disabled_channels, cfg->num_disabled);
    memset(cfg, 0, sizeof(*cfg));
}

/* ---- lifecycle --------------------------------------------------------- */

void config_copy(tgf_config *dst, const tgf_config *src) {
    *dst = *src;
    dst->api_hash = src->api_hash ? strdup(src->api_hash) : NULL;
    dst->dest_channel = src->dest_channel ? strdup(src->dest_channel) : NULL;
    dst->history_file = src->history_file ? strdup(src->history_file) : NULL;
    dst->source_channels = str_array_dup(src->source_channels, src->num_sources);
    dst->disabled_channels = str_array_dup(src->disabled_channels, src->num_disabled);
}

/* Replaces rt->cfg wholesale (frees the previous value). */
void runtime_config_load(Runtime *rt, const tgf_config *cfg) {
    config_free(&rt->cfg);
    rt->cfg.api_id = cfg->api_id;
    rt->cfg.api_hash = cfg->api_hash ? strdup(cfg->api_hash) : NULL;
    rt->cfg.dest_channel = cfg->dest_channel ? strdup(cfg->dest_channel) : NULL;
    rt->cfg.history_file = strdup(cfg->history_file ? cfg->history_file : "history.txt");
    rt->cfg.forward_delay_sec = cfg->forward_delay_sec;
    rt->cfg.history_window_hours = cfg->history_window_hours;
    rt->cfg.enable_sequential_forwarding = cfg->enable_sequential_forwarding;
    rt->cfg.enable_ordering = cfg->enable_ordering;
    rt->cfg.sequence_direction = cfg->sequence_direction;
    rt->cfg.num_sources = cfg->num_sources;
    rt->cfg.source_channels = str_array_dup(cfg->source_channels, cfg->num_sources);
    rt->cfg.num_disabled = cfg->num_disabled;
    rt->cfg.disabled_channels = str_array_dup(cfg->disabled_channels, cfg->num_disabled);
    rt->src_name = "unknown";
}

int handle_matches(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (!strncmp(a, "t.me/", 5)) a += 5;
    if (!strncmp(b, "t.me/", 5)) b += 5;
    if (*a == '@') a++;
    if (*b == '@') b++;
    return strcmp(a, b) == 0;
}

int source_disabled(const Config *cfg, const char *handle) {
    for (int i = 0; i < cfg->num_disabled; i++)
        if (handle_matches(cfg->disabled_channels[i], handle)) return 1;
    return 0;
}

int source_index_by_handle(const Config *cfg, const char *handle) {
    for (int i = 0; i < cfg->num_sources; i++)
        if (handle_matches(cfg->source_channels[i], handle)) return i;
    return -1;
}
