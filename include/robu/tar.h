#ifndef ROBU_TAR_H
#define ROBU_TAR_H
#include "robu/types.h"
int tar_find(const void *archive, uint64_t archive_len, const char *name,
             const uint8_t **out_data, uint64_t *out_size);

int tar_iterate(const void *archive, uint64_t archive_len, uint64_t index,
                 char *name_out, uint64_t name_max, uint64_t *out_size);

#define TAR_ENTRY_FILE 0
#define TAR_ENTRY_SYMLINK 1

typedef struct {
    char name[100];
    char linkname[100];
    int type;
    uint64_t size;
    const uint8_t *data;
} tar_entry_t;

int tar_iterate_all(const void *archive, uint64_t archive_len, uint64_t index,
                     tar_entry_t *out);
#endif
