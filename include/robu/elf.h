#ifndef ROBU_ELF_H
#define ROBU_ELF_H
#include "robu/types.h"
#include "robu/tcb.h"
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;
#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1
#define EM_X86_64   62
#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD 1
#define PF_X (1 << 0)
#define PF_W (1 << 1)
#define PF_R (1 << 2)
#define ELF_MAX_LOAD_SEGMENTS 4
typedef struct {
    vaddr_t  vaddr;
    uint64_t file_off;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t prot;
} elf_segment_t;
typedef struct {
    const uint8_t *elf_start;
    vaddr_t entry;
    uint32_t nsegs;
    elf_segment_t segs[ELF_MAX_LOAD_SEGMENTS];
    uint8_t in_use;
} elf_image_t;
typedef struct {
    uint64_t parses;
    uint64_t cache_hits;
} elf_cache_stats_t;
extern elf_cache_stats_t elf_cache_stats;
const elf_image_t *elf_parse(const uint8_t *elf_start, const uint8_t *elf_end);
const elf_image_t *elf_parse_nocache(const uint8_t *elf_start, const uint8_t *elf_end);
tcb_t *elf_load_and_spawn(const char *name, const uint8_t *elf_start,
                          const uint8_t *elf_end, uint8_t prio, tid_t pager_tid);
tcb_t *elf_load_and_spawn_argv(const char *name, const uint8_t *elf_start,
                               const uint8_t *elf_end, uint8_t prio, tid_t pager_tid,
                               int argc, const char *const *argv);
tcb_t *elf_load_and_spawn_req(const char *name, const uint8_t *elf_start,
                              const uint8_t *elf_end, uint8_t prio, tid_t pager_tid,
                              int argc, const char *const *argv,
                              int envc, const char *const *envp,
                              uint32_t nfds, const void *fds_blob, int use_cache);
int elf_exec_current(tcb_t *cur, const char *name, const uint8_t *elf_start,
                     const uint8_t *elf_end, int argc, const char *const *argv,
                     int envc, const char *const *envp,
                     uint32_t nfds, const void *fds_blob, int use_cache);
#endif
