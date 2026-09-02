#ifndef ROBU_RANDOM_H
#define ROBU_RANDOM_H

#include "robu/types.h"

#define ROBU_RANDOM_MAX_REQUEST 256

void random_init(void);
int random_available(void);
int random_fill(void *buf, uint64_t len);
int random_u32(uint32_t *out);

#endif
