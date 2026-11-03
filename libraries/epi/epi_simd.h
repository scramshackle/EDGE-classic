#pragma once

#include <stdint.h>
#include <string.h>

#if defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)))
#define EPI_SIMD_SSE2
#include <emmintrin.h>
#endif

#if defined(__ARM_NEON) || (defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64)))
#define EPI_SIMD_NEON
#include <arm_neon.h>
#endif

#if defined(__wasm_simd128__) && !defined(__SSE2__)
#define EPI_SIMD_WASM
#include <wasm_simd128.h>
#endif

namespace epi
{

#if defined(EPI_SIMD_SSE2) || defined(EPI_SIMD_NEON) || defined(EPI_SIMD_WASM)
constexpr bool kSimdAvailable = true;
#else
constexpr bool kSimdAvailable = false;
#endif

#if defined(EPI_SIMD_SSE2)

typedef __m128  SimdF32x4;
typedef __m128i SimdI32x4;
typedef __m128i SimdI16x8;
typedef __m128i SimdU8x16;
typedef __m128d SimdF64x2;

#elif defined(EPI_SIMD_NEON)

typedef float32x4_t SimdF32x4;
typedef int32x4_t   SimdI32x4;
typedef int16x8_t   SimdI16x8;
typedef uint8x16_t  SimdU8x16;
#if defined(__aarch64__)
typedef float64x2_t SimdF64x2;
#else
struct SimdF64x2 { double v[2]; };
#endif

#elif defined(EPI_SIMD_WASM)

typedef v128_t SimdF32x4;
typedef v128_t SimdI32x4;
typedef v128_t SimdI16x8;
typedef v128_t SimdU8x16;
typedef v128_t SimdF64x2;

#else

struct SimdF32x4 { float   v[4]; };
struct SimdI32x4 { int32_t v[4]; };
struct SimdI16x8 { int16_t v[8]; };
struct SimdU8x16 { uint8_t v[16]; };
struct SimdF64x2 { double  v[2]; };

#endif

inline void StoreU8x16(uint8_t *p, SimdU8x16 v)
{
#if defined(EPI_SIMD_SSE2)
    _mm_storeu_si128((__m128i *)p, v);
#elif defined(EPI_SIMD_NEON)
    vst1q_u8(p, v);
#elif defined(EPI_SIMD_WASM)
    wasm_v128_store(p, v);
#else
    for (int i = 0; i < 16; i++)
        p[i] = v.v[i];
#endif
}

inline void EnableFastFloats()
{
#if defined(EPI_SIMD_SSE2)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(EPI_SIMD_NEON)
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__arm__)
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1U << 24);
    __asm__ volatile("vmsr fpscr, %0" ::"r"(fpscr));
#endif
#endif
}

inline SimdU8x16 LoadU8x16(const uint8_t *p)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_loadu_si128((const __m128i *)p);
#elif defined(EPI_SIMD_NEON)
    return vld1q_u8(p);
#elif defined(EPI_SIMD_WASM)
    return wasm_v128_load(p);
#else
    SimdU8x16 r;
    memcpy(r.v, p, 16);
    return r;
#endif
}

inline SimdU8x16 SplatU8x16(uint8_t x)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_set1_epi8((char)x);
#elif defined(EPI_SIMD_NEON)
    return vdupq_n_u8(x);
#elif defined(EPI_SIMD_WASM)
    return wasm_i8x16_splat((int8_t)x);
#else
    SimdU8x16 r;
    for (int i = 0; i < 16; i++)
        r.v[i] = x;
    return r;
#endif
}

inline SimdU8x16 SubSaturateU8x16(SimdU8x16 a, SimdU8x16 b)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_subs_epu8(a, b);
#elif defined(EPI_SIMD_NEON)
    return vqsubq_u8(a, b);
#elif defined(EPI_SIMD_WASM)
    return wasm_u8x16_sub_sat(a, b);
#else
    SimdU8x16 r;
    for (int i = 0; i < 16; i++)
        r.v[i] = (uint8_t)(a.v[i] > b.v[i] ? a.v[i] - b.v[i] : 0);
    return r;
#endif
}

inline SimdU8x16 OrU8x16(SimdU8x16 a, SimdU8x16 b)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_or_si128(a, b);
#elif defined(EPI_SIMD_NEON)
    return vorrq_u8(a, b);
#elif defined(EPI_SIMD_WASM)
    return wasm_v128_or(a, b);
#else
    SimdU8x16 r;
    for (int i = 0; i < 16; i++)
        r.v[i] = (uint8_t)(a.v[i] | b.v[i]);
    return r;
#endif
}

inline SimdU8x16 CmpEqU8x16(SimdU8x16 a, SimdU8x16 b)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_cmpeq_epi8(a, b);
#elif defined(EPI_SIMD_NEON)
    return vceqq_u8(a, b);
#elif defined(EPI_SIMD_WASM)
    return wasm_i8x16_eq(a, b);
#else
    SimdU8x16 r;
    for (int i = 0; i < 16; i++)
        r.v[i] = (uint8_t)(a.v[i] == b.v[i] ? 0xFF : 0);
    return r;
#endif
}

inline SimdU8x16 AndNotU8x16(SimdU8x16 a, SimdU8x16 b)
{
#if defined(EPI_SIMD_SSE2)
    return _mm_andnot_si128(b, a);
#elif defined(EPI_SIMD_NEON)
    return vbicq_u8(a, b);
#elif defined(EPI_SIMD_WASM)
    return wasm_v128_andnot(a, b);
#else
    SimdU8x16 r;
    for (int i = 0; i < 16; i++)
        r.v[i] = (uint8_t)(a.v[i] & ~b.v[i]);
    return r;
#endif
}

} // namespace epi
