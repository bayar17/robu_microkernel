#ifndef ROBU_SHM_H
#define ROBU_SHM_H
#include "robu/types.h"

#define SHM_IPC_CREAT   01000
#define SHM_IPC_EXCL    02000
#define SHM_IPC_RMID    0
#define SHM_IPC_STAT    2
#define SHM_FLAG_RDONLY 010000

#define SHM_USER_VA_BASE 0x0000000140000000ULL

int shm_get(tid_t caller, int key, uint64_t size, uint32_t shmflg, int *out_id);
int shm_at(tid_t caller, paddr_t address_space, int shmid, uint32_t shmflg, uint64_t *out_va);
int shm_dt(paddr_t address_space, uint64_t shmaddr);
int shm_ctl(int shmid, int cmd, uint64_t out_words[6]);
void shm_detach_all_for_process(paddr_t address_space);

#endif
