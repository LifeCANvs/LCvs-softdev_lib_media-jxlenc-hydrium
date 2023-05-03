#ifndef HYDRIUM_MATH_FUNCTIONS_H_
#define HYDRIUM_MATH_FUNCTIONS_H_

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define hyd_fllog2(n) (__builtin_clzll(1) - __builtin_clzll((n)|1))
#else
#ifdef _MSC_VER
#include <inttrin.h>
static inline int __builtin_clzll(const unsigned long long x) {
#ifdef _WIN64
    return (int)_lzcnt_u64(x);
#else
    return !!unsigned(x >> 32) ? __builtin_clz((unsigned)(x >> 32)) : 32 + __builtin_clz((unsigned)x);
#endif
}
#define hyd_fllog2(n) (__builtin_clzll(1) - __builtin_clzll(n))
#else /* _MSC_VER */
static inline int hyd_fllog2(unsigned long long n) {
    #define S(k) if (n >= (1ULL << k)) { i += k; n >>= k; }
    int i = -(n == 0ULL); S(32); S(16); S(8); S(4); S(2); S(1); return i;
    #undef S
}
#endif /* _MSC_VER */
#endif /* __GNUC__ || __clang__ */

/* ceil(log2(n)) */
static inline int hyd_cllog2(const unsigned long long n) {
    return hyd_fllog2(n) + !!(n & (n - 1));
}

/* v / (1 << n) */
static inline int32_t hyd_signed_rshift32(const int32_t v, const int n) {
    return v >= 0 ? v >> n : -(-v >> n);
}

/* v / (1 << n) */
static inline int64_t hyd_signed_rshift64(const int64_t v, const int n) {
    return v >= 0 ? v >> n : -(-v >> n);
}

static inline uint32_t hyd_pack_signed(const int32_t v) {
    return (v << 1) ^ -(v < 0);
}

static const uint32_t br_lut[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF,
};
static inline uint32_t hyd_bitswap32(const uint32_t b) {
    uint32_t c = 0;
    for (unsigned i = 0; i < 32; i += 4)
        c |= br_lut[(b >> i) & 0xF] << (28 - i);
    return c;
}

#define hyd_max(a, b) ((a) > (b) ? (a) : (b))
#define hyd_min(a, b) ((a) < (b) ? (a) : (b))
#define hyd_clamp(v, min, max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))
#define hyd_max3(a, b, c) hyd_max((a), hyd_max((b), (c)))
#define hyd_swap(type, a, b) do {\
    const type __hyd_swap_temp = (b); (b) = (a); (a) = __hyd_swap_temp;\
} while (0)

#endif /* HYDRIUM_MATH_FUNCTIONS_H_ */
