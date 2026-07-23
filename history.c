#include "tgf.h"

static unsigned int history_hash(const char *key) {
    unsigned int h = 2166136261u;
    while (*key) {h ^= (unsigned char)*key++; h *= 16777619u; }
    return h;
}

static void history_rehash(int new_size) {
    char **ns = calloc(new_size, sizeof(char *));
    time_t *nt = calloc(new_size, sizeof(time_t));
    if (!ns || !nt) { free(ns); free(nt); return; }
    for (int i = 0; i < history_set_size; i++) {
        if (history_set[i]) {
            unsigned int idx = history_hash(history_set[i]) & (new_size - 1);
            while (ns[idx]) idx = (idx + 1) & (new_size - 1);
            ns[idx] = history_set[i];
            nt[idx] = history_time[i];
        }
    }
    free(history_set);
    free(history_time);
    history_set = ns;
    history_time = nt;
    history_set_size = new_size;
}

void history_load(void) {
    FILE *f = fopen(history_file, "r");
    if (!f) return;
    if (!history_set) {
        history_set = calloc(16384, sizeof(char *));
        history_time = calloc(16384, sizeof(time_t));
        if (!history_set || !history_time) { free(history_set); free(history_time); history_set = NULL; history_time = NULL; fclose(f); return; }
        history_set_size = 16384;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char key[64]; long long ts = 0;
        sscanf(line, "%63s %lld", key, &ts);
        if (history_count >= history_set_size * 3 / 4)
            history_rehash(history_set_size * 2);
        unsigned int idx = history_hash(key) & (history_set_size - 1);
        while (history_set[idx]) {
            if (strcmp(history_set[idx], key) == 0) break;
            idx = (idx + 1) & (history_set_size - 1);
        }
        if (history_set[idx]) continue;
        history_set[idx] = strdup(key);
        if (!history_set[idx]) break;
        history_time[idx] = (time_t)ts;
        history_count++;
    }
    fclose(f);
}

int history_has(long long chat_id, long long msg_id) {
    if (!history_set) return 0;
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key) & (history_set_size - 1);
    time_t now = time(NULL);
    while (history_set[idx]) {
        if (strcmp(history_set[idx], key) == 0) {
            if (history_window_hours <= 0) return 1;
            if (history_time[idx] == 0) return 1;
            if (now - history_time[idx] < history_window_hours * 3600) return 1;
            return 0;
        }
        idx = (idx + 1) & (history_set_size - 1);
    }
    return 0;
}

void history_add(long long chat_id, long long msg_id) {
    if (!history_set) {
        history_set = calloc(16384, sizeof(char *));
        history_time = calloc(16384, sizeof(time_t));
        if (!history_set || !history_time) { free(history_set); free(history_time); history_set = NULL; history_time = NULL; return; }
        history_set_size = 16384;
    }
    if (history_count >= history_set_size * 3 / 4)
        history_rehash(history_set_size * 2);
    char key[64];
    snprintf(key, sizeof(key), "%lld_%lld", chat_id, msg_id);
    unsigned int idx = history_hash(key) & (history_set_size - 1);
    while (history_set[idx]) {
        if (strcmp(history_set[idx], key) == 0) {
            history_time[idx] = time(NULL);
            return;
        }
        idx = (idx + 1) & (history_set_size - 1);
    }
    history_set[idx] = strdup(key);
    if (!history_set[idx]) return;
    history_time[idx] = time(NULL);
    history_count++;
    FILE *f = fopen(history_file, "a");
    if (f) { fprintf(f, "%s %lld\n", key, (long long)history_time[idx]); fclose(f); }
}

void history_prune(void) {
    if (!history_set || history_window_hours <= 0) return;
    time_t now = time(NULL);
    time_t cutoff = now - history_window_hours * 3600;
    int pruned = 0;
    for (int i = 0; i < history_set_size; i++) {
        if (history_set[i] && history_time[i] > 0 && history_time[i] < cutoff) {
            free(history_set[i]);
            history_set[i] = NULL;
            history_time[i] = 0;
            history_count--;
            pruned++;
        }
    }
    if (!pruned) return;
    FILE *f = fopen(history_file, "w");
    if (!f) return;
    for (int i = 0; i < history_set_size; i++) {
        if (history_set[i])
            fprintf(f, "%s %lld\n", history_set[i], (long long)history_time[i]);
    }
    fclose(f);
    char b[80]; snprintf(b, sizeof(b), "History pruned %d old entries", pruned); set_status(b);
}
