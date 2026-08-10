#ifndef ROBU_TYPES_H
#define ROBU_TYPES_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef __uint128_t uint128_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;
typedef __int128_t int128_t;

#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned long size_t;
#endif

typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define true 1
#define false 0
typedef uint8_t bool;
typedef uint32_t tid_t;

#endif
