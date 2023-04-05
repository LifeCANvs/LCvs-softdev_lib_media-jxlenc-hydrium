#ifndef HYDRIUM_MATH_FUNCTIONS_H_
#define HYDRIUM_MATH_FUNCTIONS_H_

#include <stdint.h>

#define hyd_fllog2__(n) (__builtin_clzll(1) - __builtin_clzll((n)|1))

#if defined(__GNUC__) || defined(__clang__)
#define hyd_fllog2(n) hyd_fllog2__(n)
#else
#ifdef _MSC_VER
#include <inttrin.h>
static inline int __builtin_clzll(unsigned long long x) {
#ifdef _WIN64
    return (int)_lzcnt_u64(x);
#else
    return !!unsigned(x >> 32) ? __builtin_clz((unsigned)(x >> 32)) : 32 + __builtin_clz((unsigned)x);
#endif
}
#define hyd_fllog2(n) hyd_fllog2__(n)
#else /* _MSC_VER */
static inline int hyd_fllog2(unsigned long long n) {
    #define S(k) if (n >= (1ULL << k)) { i += k; n >>= k; }
    int i = -(n == 0ULL); S(32); S(16); S(8); S(4); S(2); S(1); return i;
    #undef S
}
#endif /* _MSC_VER */
#endif /* __GNUC__ || __clang__ */

static inline int hyd_cllog2(const unsigned long long n) {
    return hyd_fllog2(n) + !!(n & (n - 1));
}

static inline int32_t hyd_signed_rshift32(int32_t v, int n) {
    return v > 0 ? v >> n : -(-v >> n);
}

static inline int64_t hyd_signed_rshift64(int64_t v, int n) {
    return v > 0 ? v >> n : -(-v >> n);
}

static inline uint32_t hyd_pack_signed(int32_t v) {
    return (v << 1) ^ -(v < 0);
}

#define hyd_max(a, b) ((a) > (b) ? (a) : (b))
#define hyd_max3(a, b, c) hyd_max((a), hyd_max((b), (c)))
#define hyd_swap(type, a, b) do {\
    const type __hyd_swap_temp = (b); (b) = (a); (a) = __hyd_swap_temp;\
} while (0)

#endif /* HYDRIUM_MATH_FUNCTIONS_H_ */
