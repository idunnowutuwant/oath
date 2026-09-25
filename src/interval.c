#include "oath/interval.h"

OathInterval oath_interval_create(int64_t lo, int64_t hi) {
    OathInterval iv = { lo, hi };
    return iv;
}

bool oath_interval_contains_zero(OathInterval iv) {
    return (iv.lo <= 0 && iv.hi >= 0);
}

bool oath_interval_contains(OathInterval iv, int64_t val) {
    return (val >= iv.lo && val <= iv.hi);
}

OathInterval oath_interval_add(OathInterval a, OathInterval b, bool* trap) {
    int128_t l = (int128_t)a.lo + (int128_t)b.lo;
    int128_t h = (int128_t)a.hi + (int128_t)b.hi;
    if (OATH_UNLIKELY(l < INT64_MIN || h > INT64_MAX)) {
        *trap = true;
        return oath_interval_create(0, 0);
    }
    return oath_interval_create((int64_t)l, (int64_t)h);
}

OathInterval oath_interval_sub(OathInterval a, OathInterval b, bool* trap) {
    int128_t l = (int128_t)a.lo - (int128_t)b.hi;
    int128_t h = (int128_t)a.hi - (int128_t)b.lo;
    if (OATH_UNLIKELY(l < INT64_MIN || h > INT64_MAX)) {
        *trap = true;
        return oath_interval_create(0, 0);
    }
    return oath_interval_create((int64_t)l, (int64_t)h);
}

OathInterval oath_interval_mul(OathInterval a, OathInterval b, bool* trap) {
    int128_t p1 = (int128_t)a.lo * (int128_t)b.lo;
    int128_t p2 = (int128_t)a.lo * (int128_t)b.hi;
    int128_t p3 = (int128_t)a.hi * (int128_t)b.lo;
    int128_t p4 = (int128_t)a.hi * (int128_t)b.hi;

    int128_t min_p = p1 < p2 ? p1 : p2;
    if (p3 < min_p) min_p = p3;
    if (p4 < min_p) min_p = p4;

    int128_t max_p = p1 > p2 ? p1 : p2;
    if (p3 > max_p) max_p = p3;
    if (p4 > max_p) max_p = p4;

    if (OATH_UNLIKELY(min_p < INT64_MIN || max_p > INT64_MAX)) {
        *trap = true;
        return oath_interval_create(0, 0);
    }
    return oath_interval_create((int64_t)min_p, (int64_t)max_p);
}

OathInterval oath_interval_div(OathInterval a, OathInterval b, bool* div_zero_trap, bool* de_overflow_trap) {
    if (OATH_UNLIKELY(oath_interval_contains_zero(b))) {
        *div_zero_trap = true;
        return oath_interval_create(0, 0);
    }
    if (OATH_UNLIKELY(a.lo == INT64_MIN && oath_interval_contains(b, -1))) {
        *de_overflow_trap = true;
        return oath_interval_create(0, 0);
    }

    int64_t d1 = a.lo / b.lo;
    int64_t d2 = a.lo / b.hi;
    int64_t d3 = a.hi / b.lo;
    int64_t d4 = a.hi / b.hi;

    int64_t min_d = d1 < d2 ? d1 : d2;
    if (d3 < min_d) min_d = d3;
    if (d4 < min_d) min_d = d4;

    int64_t max_d = d1 > d2 ? d1 : d2;
    if (d3 > max_d) max_d = d3;
    if (d4 > max_d) max_d = d4;

    return oath_interval_create(min_d, max_d);
}