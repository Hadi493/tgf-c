#include "tgf.h"

static unsigned int history_hash(const char *key) {
    unsigned int h = 2166136261u;
    while (*key) {h ^= (unsigned char)*key++; h *= 16777619u; }
    return h;
}

static void history_rehash(HistoryStore *hs, int new_size) {
    char **ns = calloc(new_size, sizeof(char *));
    time_t *nt = calloc(new_size, sizeof(time_t));
    if (!ns || !nt) { free(ns); free(nt); ui_oom("OOM: history rehash"); return; }
    for (int i = 0; i < hs->set_size; i++) {
        if (hs->set[i]) {
            unsigned int idx = history_hash(hs->set[i]) & (new_size - 1);
            while (ns[idx]) idx = (idx + 1) & (new_size - 1);
            ns[idx] = hs->set[i];
            nt[idx] = hs->time[i];
        }
    }
    free(hs->set);
    free(hs->time);
    hs->set = ns;
    hs->time = nt;
    hs->set_size = new_size;
}

void history_store_load(HistoryStore *hs, const Config *cfg) {
    FILE *f = fopen(cfg->history_file, "r");
    if (!f) return;
    if (!hs->set) {
        hs->set = calloc(16384, sizeof(char *));
        hs->time = calloc(16384, sizeof(time_t));
        if (!hs->set || !hs->time) { free(hs->set); free(hs->time); hs->set = NULL; hs->time = NULL; ui_oom("OOM: history init"); fclose(f); return; }
        hs->set_size = 16384;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char key[64]; long long ts = 0;
        sscanf(line, "%63s %lld", key, &ts);
        if (hs->count >= hs->set_size * 3 / 4)
            history_rehash(hs, hs->set_size * 2);
        unsigned int idx = history_hash(key) & (hs->set_size - 1);
        while (hs->set[idx]) {
            if (strcmp(hs->set[idx], key) == 0) break;
            idx = (idx + 1) & (hs->set_size - 1);
        }
        if (hs->set[idx]) continue;
        hs->set[idx] = strdup(key);
        if (!hs->set[idx]) { ui_oom("OOM: history strdup"); break; }
        hs->time[idx] = (time_t)ts;
        hs->count++;
    }
    fclose(f);
}

int history_store_has(const HistoryStore *hs, const Config *cfg, long long chat_id, long long msg_id) {
    if (!hs->set) return 0;
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key) & (hs->set_size - 1);
    time_t now = time(NULL);
    while (hs->set[idx]) {
        if (strcmp(hs->set[idx], key) == 0) {
            if (cfg->history_window_hours <= 0) return 1;
            if (hs->time[idx] == 0) return 1;
            if (now - hs->time[idx] < cfg->history_window_hours * 3600) return 1;
            return 0;
        }
        idx = (idx + 1) & (hs->set_size - 1);
    }
    return 0;
}

void history_store_add(HistoryStore *hs, const Config *cfg, long long chat_id, long long msg_id) {
    if (!hs->set) {
        hs->set = calloc(16384, sizeof(char *));
        hs->time = calloc(16384, sizeof(time_t));
        if (!hs->set || !hs->time) { free(hs->set); free(hs->time); hs->set = NULL; hs->time = NULL; ui_oom("OOM: history init"); return; }
        hs->set_size = 16384;
    }
    if (hs->count >= hs->set_size * 3 / 4)
        history_rehash(hs, hs->set_size * 2);
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key) & (hs->set_size - 1);
    while (hs->set[idx]) {
        if (strcmp(hs->set[idx], key) == 0) {
            hs->time[idx] = time(NULL);
            return;
        }
        idx = (idx + 1) & (hs->set_size - 1);
    }
    hs->set[idx] = strdup(key);
    if (!hs->set[idx]) { ui_oom("OOM: history strdup"); return; }
    hs->time[idx] = time(NULL);
    hs->count++;
    FILE *f = fopen(cfg->history_file, "a");
    if (f) { fprintf(f, "%s %lld\n", key, (long long)hs->time[idx]); fclose(f); }
}

void history_store_prune(HistoryStore *hs, const Config *cfg) {
    if (!hs->set || cfg->history_window_hours <= 0) return;
    time_t now = time(NULL);
    time_t cutoff = now - cfg->history_window_hours * 3600;
    int pruned = 0;
    for (int i = 0; i < hs->set_size; i++) {
        if (hs->set[i] && hs->time[i] > 0 && hs->time[i] < cutoff) {
            free(hs->set[i]);
            hs->set[i] = NULL;
            hs->time[i] = 0;
            hs->count--;
            pruned++;
        }
    }
    if (!pruned) return;
    FILE *f = fopen(cfg->history_file, "w");
    if (!f) return;
    for (int i = 0; i < hs->set_size; i++) {
        if (hs->set[i])
            fprintf(f, "%s %lld\n", hs->set[i], (long long)hs->time[i]);
    }
    fclose(f);
    char b[80]; snprintf(b, sizeof(b), "History pruned %d old entries", pruned); ui_status(b);
}
