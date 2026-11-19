#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

int config_load(const char *path, Config *out) {
    memset(out, 0, sizeof(*out));
    out->forward_delay_sec = 1;
    out->history_window_hours = 24;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    long nread = fread(buf, 1, len, f);
    if (nread <= 0) { free(buf); fclose(f); return -1; }
    buf[nread] = 0;
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { fprintf(stderr, "Invalid JSON in %s\n", path); return -1; }

    cJSON *v;
    v = cJSON_GetObjectItem(root, "api_id");
    if (!v || !cJSON_IsNumber(v)) { fprintf(stderr, "Missing api_id\n"); cJSON_Delete(root); return -1; }
    out->api_id = (int)v->valuedouble;

    v = cJSON_GetObjectItem(root, "api_hash");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing api_hash\n"); cJSON_Delete(root); return -1; }
    out->api_hash = strdup(v->valuestring);
    if (!out->api_hash) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "dest_channel");
    if (!v || !cJSON_IsString(v)) { fprintf(stderr, "Missing dest_channel\n"); cJSON_Delete(root); return -1; }
    out->dest_channel = strdup(v->valuestring);
    if (!out->dest_channel) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "history_file");
    out->history_file = v && cJSON_IsString(v) ? strdup(v->valuestring) : strdup("history.txt");
    if (!out->history_file) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }

    v = cJSON_GetObjectItem(root, "forward_delay_sec");
    out->forward_delay_sec = v && cJSON_IsNumber(v) ? (int)v->valuedouble : 1;

    v = cJSON_GetObjectItem(root, "history_window_hours");
    if (v && cJSON_IsNumber(v)) out->history_window_hours = (int)v->valuedouble;

    v = cJSON_GetObjectItem(root, "enable_sequential_forwarding");
    out->enable_sequential_forwarding = v && cJSON_IsBool(v) ? v->valueint : 0;

    v = cJSON_GetObjectItem(root, "enable_ordering");
    out->enable_ordering = v && cJSON_IsBool(v) ? v->valueint : 0;

    v = cJSON_GetObjectItem(root, "sequence_direction");
    if (v && cJSON_IsString(v) && strcmp(v->valuestring, "desc") == 0)
        out->sequence_direction = 1;
    else
        out->sequence_direction = 0;

    v = cJSON_GetObjectItem(root, "source_channels");
    if (!v || !cJSON_IsArray(v)) { fprintf(stderr, "Missing source_channels array\n"); cJSON_Delete(root); return -1; }
    out->num_sources = cJSON_GetArraySize(v);
    out->source_channels = calloc(out->num_sources, sizeof(char *));
    if (!out->source_channels) { fprintf(stderr, "OOM\n"); cJSON_Delete(root); return -1; }
    for (int i = 0; i < out->num_sources; i++) {
        const cJSON *elem = cJSON_GetArrayItem(v, i);
        out->source_channels[i] = elem && cJSON_IsString(elem) ? strdup(elem->valuestring) : strdup("");
        if (!out->source_channels[i]) { out->source_channels[i] = strdup(""); }
    }

    cJSON_Delete(root);
    return 0;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->api_hash);
    free(cfg->dest_channel);
    free(cfg->history_file);
    if (cfg->source_channels) {
        for (int i = 0; i < cfg->num_sources; i++) free(cfg->source_channels[i]);
        free(cfg->source_channels);
    }
    memset(cfg, 0, sizeof(*cfg));
}
