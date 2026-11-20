#ifndef TGF_TRANSPORT_FAKE_H
#define TGF_TRANSPORT_FAKE_H
#include "tgf.h"
extern const tgf_transport transport_fake;
void fake_reset(void);
void fake_push(const char *json);
int fake_sent_count(void);
const char *fake_sent(int i);
#endif
