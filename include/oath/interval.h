#ifndef OATH_INTERVAL_H
#define OATH_INTERVAL_H

#include "oath/common.h"

typedef struct {
    int64_t lo;
    int64_t hi;
} OathInterval;

OathInterval oath_interval_create(int64_t lo, int64_t hi);
bool oath_interval_contains_zero(OathInterval iv);
bool oath_interval_contains(OathInterval iv, int64_t val);
OathInterval oath_interval_add(OathInterval a, OathInterval b, bool* trap);
OathInterval oath_interval_sub(OathInterval a, OathInterval b, bool* trap);
OathInterval oath_interval_mul(OathInterval a, OathInterval b, bool* trap);
OathInterval oath_interval_div(OathInterval a, OathInterval b, bool* div_zero_trap, bool* de_overflow_trap);

#endif