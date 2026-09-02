#include "robu/random.h"

static int g_random_available;

void random_init(void) {
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    g_random_available = (ecx & (1u << 30)) != 0;
}

int random_available(void) {
    return g_random_available;
}

static int random_u64(uint64_t *out) {
    if (!g_random_available) {
        return -1;
    }
    for (int attempt = 0; attempt < 16; attempt++) {
        uint64_t value;
        uint8_t ready;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ready));
        if (ready) {
            *out = value;
            return 0;
        }
    }
    return -1;
}

int random_fill(void *buf, uint64_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint64_t offset = 0;
    while (offset < len) {
        uint64_t value;
        if (random_u64(&value) != 0) {
            return -1;
        }
        uint64_t chunk = len - offset;
        if (chunk > sizeof(value)) {
            chunk = sizeof(value);
        }
        for (uint64_t i = 0; i < chunk; i++) {
            out[offset + i] = (uint8_t)(value >> (i * 8));
        }
        offset += chunk;
    }
    return 0;
}

int random_u32(uint32_t *out) {
    uint64_t value;
    if (random_u64(&value) != 0) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}
