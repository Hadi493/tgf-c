#ifndef TGF_CONFIG_H_
#define TGF_CONFIG_H_

typedef struct {
    int api_id;
    char *api_hash;
    char **source_channels;
    int num_sources;
    char **disabled_channels;
    int num_disabled;
    char *dest_channel;
    char *history_file;
    int forward_delay_sec;
    int history_window_hours;
    int enable_sequential_forwarding;
    int enable_ordering;
    int sequence_direction;
} Config;

int config_load(const char *path, Config *out);
void config_free(Config *cfg);

#endif
