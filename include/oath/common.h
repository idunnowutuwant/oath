#ifndef OATH_COMMON_H
#define OATH_COMMON_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

typedef __int128_t int128_t;
typedef __uint128_t uint128_t;

#define OATH_LIKELY(x)   __builtin_expect(!!(x), 1)
#define OATH_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif