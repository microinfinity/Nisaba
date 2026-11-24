#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <span>
#include "nisaba/color/color.hpp"
#include "nisaba/color/color_space_lut.hpp"
#include "nisaba/color/blend_mode.hpp"

// Platform-specific SIMD headers (Standard compiler headers only)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define NISABA_HAS_SSE2 1
    #include <immintrin.h>
  #endif
  #if defined(__AVX2__)
    #define NISABA_HAS_AVX2 1
  #endif
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
  #define NISABA_HAS_NEON 1
  #include <arm_neon.h>
#endif

namespace nisaba::simd {

/// Highly optimized vectorized 32-bit solid fill.
inline void fill_solid_span(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    size_t count
) noexcept {
    if (count == 0) return;
    uint32_t val;
    std::memcpy(&val, &src, sizeof(val));

    // Fast-path: all 4 bytes identical (e.g., transparent 0 or white 0xFFFFFFFF)
    if ((val & 0xFF) == ((val >> 8) & 0xFF) &&
        (val & 0xFF) == ((val >> 16) & 0xFF) &&
        (val & 0xFF) == ((val >> 24) & 0xFF)) {
        std::memset(reinterpret_cast<void*>(dst), static_cast<int>(val & 0xFF), count * sizeof(PremultipliedColorU8));
        return;
    }

#if defined(NISABA_HAS_AVX2)
    __m256i v = _mm256_set1_epi32(static_cast<int>(val));
    size_t i = 0;
    while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 31) != 0) {
        dst[i] = src;
        ++i;
    }
    for (; i + 128 <= count; i += 128) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 0), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 8), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 16), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 24), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 32), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 40), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 48), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 56), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 64), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 72), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 80), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 88), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 96), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 104), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 112), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 120), v);
    }
    for (; i + 32 <= count; i += 32) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 0), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 8), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 16), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i + 24), v);
    }
    for (; i + 8 <= count; i += 8) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i), v);
    }
    for (; i < count; ++i) {
        dst[i] = src;
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i v = _mm_set1_epi32(static_cast<int>(val));

    if (count >= 4096) {
        size_t i = 0;
        while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 15) != 0) {
            dst[i] = src;
            ++i;
        }
        for (; i + 16 <= count; i += 16) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), v);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 4), v);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 8), v);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 12), v);
        }
        for (; i + 4 <= count; i += 4) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), v);
        }
        _mm_sfence();
        for (; i < count; ++i) {
            dst[i] = src;
        }
        return;
    }

    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 4), v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 8), v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 12), v);
    }
    for (; i + 4 <= count; i += 4) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
    }
    for (; i < count; ++i) {
        dst[i] = src;
    }
#else
    std::fill_n(dst, count, src);
#endif
}

/// Blends a solid source color over a destination span using pure scalar C++20.
inline void blend_solid_source_over_scalar(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    size_t count
) noexcept {
    if (src.alpha() == 255) {
        fill_solid_span(dst, src, count);
        return;
    }
    if (src.alpha() == 0) return;

    uint32_t inv_a = 256 - static_cast<uint32_t>(src.alpha());
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(src.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(src.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(src.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(src.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}

/// Blends a solid source color over a destination span using SSE2 vectorization (4 pixels per iteration).
#if defined(NISABA_HAS_SSE2)
inline void blend_solid_source_over_sse2(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    size_t count
) noexcept {
    if (src.alpha() == 255) {
        fill_solid_span(dst, src, count);
        return;
    }
    if (src.alpha() == 0) return;

    uint32_t inv_a = 256 - static_cast<uint32_t>(src.alpha());
    __m128i v_inv_a = _mm_set1_epi16(static_cast<short>(inv_a));
    __m128i zero = _mm_setzero_si128();

    // Broadcast src pixel to 4-pixel 16-bit expanded vector
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        // (dst * inv_a) >> 8
        __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, v_inv_a), 8);
        __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, v_inv_a), 8);

        // src + ((dst * inv_a) >> 8)
        __m128i out_lo = _mm_add_epi16(v_src_16_lo, d_mul_lo);
        __m128i out_hi = _mm_add_epi16(v_src_16_hi, d_mul_hi);

        __m128i res = _mm_packus_epi16(out_lo, out_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }

    // Scalar tail
    for (; i < count; ++i) {
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(src.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(src.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(src.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(src.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}
#endif

#if defined(NISABA_HAS_AVX2)
/// Blends a solid source color over destination span using AVX2 (8 pixels per iteration).
inline void blend_solid_source_over_avx2(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    size_t count
) noexcept {
    if (src.alpha() == 255) {
        fill_solid_span(dst, src, count);
        return;
    }
    if (src.alpha() == 0) return;

    uint32_t inv_a = 256 - static_cast<uint32_t>(src.alpha());

    if (count < 8) {
        uint32_t src_packed = *reinterpret_cast<const uint32_t*>(&src);
        for (size_t k = 0; k < count; ++k) {
            uint32_t d = *reinterpret_cast<const uint32_t*>(dst + k);
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            *reinterpret_cast<uint32_t*>(dst + k) = src_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        }
        return;
    }

    __m256i v_inv_a = _mm256_set1_epi16(static_cast<short>(inv_a));
    __m256i zero = _mm256_setzero_si256();

    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        __m256i d0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i + 8));

        __m256i d0_lo = _mm256_unpacklo_epi8(d0, zero);
        __m256i d0_hi = _mm256_unpackhi_epi8(d0, zero);
        __m256i d1_lo = _mm256_unpacklo_epi8(d1, zero);
        __m256i d1_hi = _mm256_unpackhi_epi8(d1, zero);

        __m256i d0_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d0_lo, v_inv_a), 8);
        __m256i d0_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d0_hi, v_inv_a), 8);
        __m256i d1_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d1_lo, v_inv_a), 8);
        __m256i d1_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d1_hi, v_inv_a), 8);

        __m256i out0_lo = _mm256_add_epi16(v_src_16_lo, d0_mul_lo);
        __m256i out0_hi = _mm256_add_epi16(v_src_16_hi, d0_mul_hi);
        __m256i out1_lo = _mm256_add_epi16(v_src_16_lo, d1_mul_lo);
        __m256i out1_hi = _mm256_add_epi16(v_src_16_hi, d1_mul_hi);

        __m256i res0 = _mm256_packus_epi16(out0_lo, out0_hi);
        __m256i res1 = _mm256_packus_epi16(out1_lo, out1_hi);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 8), res1);
    }
    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i d_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, v_inv_a), 8);
        __m256i d_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, v_inv_a), 8);

        __m256i out_lo = _mm256_add_epi16(v_src_16_lo, d_mul_lo);
        __m256i out_hi = _mm256_add_epi16(v_src_16_hi, d_mul_hi);

        __m256i res = _mm256_packus_epi16(out_lo, out_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }

    // Tail using SSE2 or scalar
    for (; i < count; ++i) {
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(src.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(src.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(src.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(src.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}
#endif

/// Blends a solid source color over destination span, dispatching to the fastest hardware path.
inline void blend_solid_source_over(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    size_t count
) noexcept {
#if defined(NISABA_HAS_AVX2)
    blend_solid_source_over_avx2(dst, src, count);
#elif defined(NISABA_HAS_SSE2)
    blend_solid_source_over_sse2(dst, src, count);
#else
    blend_solid_source_over_scalar(dst, src, count);
#endif
}

/// Blends a solid color with coverage modulation (anti-aliased span).
inline void blend_solid_source_over_coverage(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    uint8_t coverage,
    size_t count
) noexcept {
    if (coverage == 0 || src.alpha() == 0) return;
    if (coverage == 255) {
        blend_solid_source_over(dst, src, count);
        return;
    }

    // Modulate src by coverage
    uint32_t cov = static_cast<uint32_t>(coverage) + 1;
    uint8_t mod_r = static_cast<uint8_t>((static_cast<uint32_t>(src.red()) * cov) >> 8);
    uint8_t mod_g = static_cast<uint8_t>((static_cast<uint32_t>(src.green()) * cov) >> 8);
    uint8_t mod_b = static_cast<uint8_t>((static_cast<uint32_t>(src.blue()) * cov) >> 8);
    uint8_t mod_a = static_cast<uint8_t>((static_cast<uint32_t>(src.alpha()) * cov) >> 8);

    if (count == 1) {
        uint32_t inv_a = 256 - mod_a;
        uint32_t src_packed = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
        uint32_t d = *reinterpret_cast<const uint32_t*>(dst);
        uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
        uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
        *reinterpret_cast<uint32_t*>(dst) = src_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        return;
    }

    PremultipliedColorU8 mod_src = PremultipliedColorU8::from_rgba_unchecked(mod_r, mod_g, mod_b, mod_a);
    blend_solid_source_over(dst, mod_src, count);
}

/// Blends a span of source pixels over destination using pure scalar C++20.
inline void blend_source_over_span_scalar(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    size_t count
) noexcept {
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        if (s.alpha() == 255) {
            dst[i] = s;
        } else if (s.alpha() > 0) {
            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
            PremultipliedColorU8 d = dst[i];
            uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
            uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
            uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
            uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
        }
    }
}

#if defined(NISABA_HAS_AVX2)
/// Blends a span of source pixels over destination using AVX2 vectorization (8 pixels per iteration).
inline void blend_source_over_span_avx2(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    size_t count
) noexcept {
    __m256i zero = _mm256_setzero_si256();
    __m256i v_256 = _mm256_set1_epi16(256);
    __m256i alpha_mask = _mm256_set1_epi32(static_cast<int32_t>(0xFF000000));

    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));

        __m256i a = _mm256_and_si256(s, alpha_mask);
        __m256i cmp_opaque = _mm256_cmpeq_epi32(a, alpha_mask);
        if (_mm256_movemask_epi8(cmp_opaque) == -1) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), s);
            continue;
        }
        if (_mm256_testz_si256(a, a)) {
            continue;
        }

        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));

        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);

        __m256i sa_lo = _mm256_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        sa_lo = _mm256_shufflehi_epi16(sa_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a_lo = _mm256_sub_epi16(v_256, sa_lo);

        __m256i sa_hi = _mm256_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        sa_hi = _mm256_shufflehi_epi16(sa_hi, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a_hi = _mm256_sub_epi16(v_256, sa_hi);

        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i d_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, inv_a_lo), 8);
        __m256i d_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, inv_a_hi), 8);

        __m256i out_lo = _mm256_add_epi16(s_lo, d_mul_lo);
        __m256i out_hi = _mm256_add_epi16(s_hi, d_mul_hi);

        __m256i res = _mm256_packus_epi16(out_lo, out_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }

    // SSE2 tail for remaining 4..7 pixels
    __m128i alpha_mask128 = _mm_set1_epi32(static_cast<int32_t>(0xFF000000));
    for (; i + 4 <= count; i += 4) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));

        __m128i a128 = _mm_and_si128(s, alpha_mask128);
        __m128i cmp128 = _mm_cmpeq_epi32(a128, alpha_mask128);
        if (_mm_movemask_epi8(cmp128) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), s);
            continue;
        }
        if (_mm_testz_si128(a128, a128)) {
            continue;
        }

        __m128i zero128 = _mm_setzero_si128();
        __m128i v_256_128 = _mm_set1_epi16(256);
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));

        __m128i s_lo = _mm_unpacklo_epi8(s, zero128);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero128);

        __m128i sa_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        sa_lo = _mm_shufflehi_epi16(sa_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i inv_a_lo = _mm_sub_epi16(v_256_128, sa_lo);

        __m128i sa_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        sa_hi = _mm_shufflehi_epi16(sa_hi, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i inv_a_hi = _mm_sub_epi16(v_256_128, sa_hi);

        __m128i d_lo = _mm_unpacklo_epi8(d, zero128);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero128);

        __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, inv_a_lo), 8);
        __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, inv_a_hi), 8);

        __m128i out_lo = _mm_add_epi16(s_lo, d_mul_lo);
        __m128i out_hi = _mm_add_epi16(s_hi, d_mul_hi);

        __m128i res = _mm_packus_epi16(out_lo, out_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }

    // Scalar tail
    for (; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        if (s.alpha() == 255) {
            dst[i] = s;
        } else if (s.alpha() > 0) {
            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
            PremultipliedColorU8 d = dst[i];
            uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
            uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
            uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
            uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
        }
    }
}
#endif

#if defined(NISABA_HAS_SSE2)
/// Blends a span of source pixels over destination using SSE2 vectorization.
inline void blend_source_over_span_sse2(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    size_t count
) noexcept {
    __m128i zero = _mm_setzero_si128();
    __m128i v_256 = _mm_set1_epi16(256);

    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));

        __m128i s_lo = _mm_unpacklo_epi8(s, zero);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero);

        // Alpha is the 4th component (index 3) of each pixel
        __m128i sa_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        sa_lo = _mm_shufflehi_epi16(sa_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i inv_a_lo = _mm_sub_epi16(v_256, sa_lo);

        __m128i sa_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        sa_hi = _mm_shufflehi_epi16(sa_hi, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i inv_a_hi = _mm_sub_epi16(v_256, sa_hi);

        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, inv_a_lo), 8);
        __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, inv_a_hi), 8);

        __m128i out_lo = _mm_add_epi16(s_lo, d_mul_lo);
        __m128i out_hi = _mm_add_epi16(s_hi, d_mul_hi);

        __m128i res = _mm_packus_epi16(out_lo, out_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }

    // Scalar tail
    for (; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        if (s.alpha() == 255) {
            dst[i] = s;
        } else if (s.alpha() > 0) {
            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
            PremultipliedColorU8 d = dst[i];
            uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
            uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
            uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
            uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
        }
    }
}

inline void blend_source_over_span_uniform_alpha_sse2(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    uint8_t alpha,
    size_t count
) noexcept {
    if (alpha == 255) {
        std::memcpy(dst, src, count * sizeof(PremultipliedColorU8));
        return;
    }
    if (alpha == 0) return;

    uint32_t inv_a = 256 - static_cast<uint32_t>(alpha);
    __m128i v_inv_a = _mm_set1_epi16(static_cast<short>(inv_a));
    __m128i zero = _mm_setzero_si128();

    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));

        __m128i s_lo = _mm_unpacklo_epi8(s, zero);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero);

        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, v_inv_a), 8);
        __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, v_inv_a), 8);

        __m128i out_lo = _mm_add_epi16(s_lo, d_mul_lo);
        __m128i out_hi = _mm_add_epi16(s_hi, d_mul_hi);

        __m128i res = _mm_packus_epi16(out_lo, out_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }

    for (; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}
#endif

/// Blends a source span over destination span with uniform alpha across all source pixels.
inline void blend_source_over_span_uniform_alpha(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    uint8_t alpha,
    size_t count
) noexcept {
#if defined(NISABA_HAS_SSE2)
    blend_source_over_span_uniform_alpha_sse2(dst, src, alpha, count);
#else
    if (alpha == 255) {
        std::memcpy(dst, src, count * sizeof(PremultipliedColorU8));
        return;
    }
    if (alpha == 0) return;
    uint32_t inv_a = 256 - static_cast<uint32_t>(alpha);
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
#endif
}

/// Blends a source span over destination span, dispatching to fastest hardware path.
inline void blend_source_over_span(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    size_t count
) noexcept {
#if defined(NISABA_HAS_AVX2)
    blend_source_over_span_avx2(dst, src, count);
#elif defined(NISABA_HAS_SSE2)
    blend_source_over_span_sse2(dst, src, count);
#else
    blend_source_over_span_scalar(dst, src, count);
#endif
}

/// Checks if all pixels in a buffer are 100% opaque (alpha == 255) using hardware SIMD.
inline bool is_buffer_opaque(const PremultipliedColorU8* px, size_t count) noexcept {
    if (!px || count == 0) return true;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i mask = _mm256_set1_epi32(static_cast<int32_t>(0xFF000000));
    for (; i + 8 <= count; i += 8) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(px + i));
        __m256i a = _mm256_and_si256(v, mask);
        __m256i cmp = _mm256_cmpeq_epi32(a, mask);
        if (static_cast<uint32_t>(_mm256_movemask_epi8(cmp)) != 0xFFFFFFFF) return false;
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i mask = _mm_set1_epi32(static_cast<int32_t>(0xFF000000));
    for (; i + 4 <= count; i += 4) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(px + i));
        __m128i a = _mm_and_si128(v, mask);
        __m128i cmp = _mm_cmpeq_epi32(a, mask);
        if (_mm_movemask_epi8(cmp) != 0xFFFF) return false;
    }
#endif
    for (; i < count; ++i) {
        if (px[i].alpha() != 255) return false;
    }
    return true;
}

/// Blends a source span over destination with coverage modulation.
inline void blend_source_over_span_coverage(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    uint8_t coverage,
    size_t count
) noexcept {
    if (coverage == 0) return;
    if (coverage == 255) {
        blend_source_over_span(dst, src, count);
        return;
    }
    uint32_t cov = static_cast<uint32_t>(coverage) + 1;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 s = src[i];
        if (s.alpha() == 0) continue;
        uint8_t sr = static_cast<uint8_t>((static_cast<uint32_t>(s.red()) * cov) >> 8);
        uint8_t sg = static_cast<uint8_t>((static_cast<uint32_t>(s.green()) * cov) >> 8);
        uint8_t sb = static_cast<uint8_t>((static_cast<uint32_t>(s.blue()) * cov) >> 8);
        uint8_t sa = static_cast<uint8_t>((static_cast<uint32_t>(s.alpha()) * cov) >> 8);

        uint32_t inv_a = 256 - static_cast<uint32_t>(sa);
        PremultipliedColorU8 d = dst[i];
        uint8_t r = static_cast<uint8_t>(sr + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
        uint8_t g = static_cast<uint8_t>(sg + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
        uint8_t b = static_cast<uint8_t>(sb + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
        uint8_t a = static_cast<uint8_t>(sa + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}

/// Blends a solid source color modulated by an 8-bit coverage mask span into destination.
inline void blend_solid_mask_span(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 sc,
    const uint8_t* mask,
    size_t count
) noexcept {
    if (sc.alpha() == 0 || count == 0) return;

    uint32_t sc_r = sc.red();
    uint32_t sc_g = sc.green();
    uint32_t sc_b = sc.blue();
    uint32_t sc_a = sc.alpha();

    size_t i = 0;

    // Fast-skip 16 consecutive zero mask bytes at once (skips unshadowed background in 2 cycles)
    for (; i + 16 <= count; i += 16) {
        uint64_t m0, m1;
        std::memcpy(&m0, mask + i, sizeof(uint64_t));
        std::memcpy(&m1, mask + i + 8, sizeof(uint64_t));
        if ((m0 | m1) == 0) continue;

        // Process 16 pixels with packed 32-bit arithmetic
        for (size_t k = 0; k < 16; ++k) {
            uint8_t alpha = mask[i + k];
            if (alpha == 0) continue;

            uint32_t cov = static_cast<uint32_t>(alpha) + 1;
            uint32_t mod_r = (sc_r * cov) >> 8;
            uint32_t mod_g = (sc_g * cov) >> 8;
            uint32_t mod_b = (sc_b * cov) >> 8;
            uint32_t mod_a = (sc_a * cov) >> 8;
            uint32_t inv_a = 256 - mod_a;

            uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i + k);
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            uint32_t mod_packed = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
            *reinterpret_cast<uint32_t*>(dst + i + k) = mod_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        }
    }

    // Remaining pixels
    for (; i < count; ++i) {
        uint8_t alpha = mask[i];
        if (alpha == 0) continue;

        uint32_t cov = static_cast<uint32_t>(alpha) + 1;
        uint32_t mod_r = (sc_r * cov) >> 8;
        uint32_t mod_g = (sc_g * cov) >> 8;
        uint32_t mod_b = (sc_b * cov) >> 8;
        uint32_t mod_a = (sc_a * cov) >> 8;
        uint32_t inv_a = 256 - mod_a;

        uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i);
        uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
        uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
        uint32_t mod_packed = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
        *reinterpret_cast<uint32_t*>(dst + i) = mod_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
    }
}

/// Blends a variable source span modulated by an 8-bit coverage mask span into destination.
inline void blend_source_over_mask_span(
    PremultipliedColorU8* dst,
    const PremultipliedColorU8* src,
    const uint8_t* mask,
    size_t count
) noexcept {
    if (count == 0) return;
    size_t i = 0;

    // Fast-skip 16 consecutive zero mask bytes
    for (; i + 16 <= count; i += 16) {
        uint64_t m0, m1;
        std::memcpy(&m0, mask + i, sizeof(uint64_t));
        std::memcpy(&m1, mask + i + 8, sizeof(uint64_t));
        if ((m0 | m1) == 0) continue;

        if (m0 == 0xFFFFFFFFFFFFFFFFULL && m1 == 0xFFFFFFFFFFFFFFFFULL) {
            blend_source_over_span(dst + i, src + i, 16);
            continue;
        }

        for (size_t k = 0; k < 16; ++k) {
            uint8_t alpha = mask[i + k];
            if (alpha == 0) continue;
            PremultipliedColorU8 s = src[i + k];
            if (s.alpha() == 0) continue;
            if (alpha == 255) {
                uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
                uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i + k);
                uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                uint32_t s_raw = *reinterpret_cast<const uint32_t*>(&s);
                *reinterpret_cast<uint32_t*>(dst + i + k) = s_raw + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
            } else {
                uint32_t cov = static_cast<uint32_t>(alpha) + 1;
                uint32_t mod_r = (static_cast<uint32_t>(s.red()) * cov) >> 8;
                uint32_t mod_g = (static_cast<uint32_t>(s.green()) * cov) >> 8;
                uint32_t mod_b = (static_cast<uint32_t>(s.blue()) * cov) >> 8;
                uint32_t mod_a = (static_cast<uint32_t>(s.alpha()) * cov) >> 8;
                uint32_t inv_a = 256 - mod_a;

                uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i + k);
                uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                uint32_t mod_packed = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
                *reinterpret_cast<uint32_t*>(dst + i + k) = mod_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
            }
        }
    }

    for (; i < count; ++i) {
        uint8_t alpha = mask[i];
        if (alpha == 0) continue;
        PremultipliedColorU8 s = src[i];
        if (s.alpha() == 0) continue;
        if (alpha == 255) {
            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
            uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i);
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            uint32_t s_raw = *reinterpret_cast<const uint32_t*>(&s);
            *reinterpret_cast<uint32_t*>(dst + i) = s_raw + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        } else {
            uint32_t cov = static_cast<uint32_t>(alpha) + 1;
            uint32_t mod_r = (static_cast<uint32_t>(s.red()) * cov) >> 8;
            uint32_t mod_g = (static_cast<uint32_t>(s.green()) * cov) >> 8;
            uint32_t mod_b = (static_cast<uint32_t>(s.blue()) * cov) >> 8;
            uint32_t mod_a = (static_cast<uint32_t>(s.alpha()) * cov) >> 8;
            uint32_t inv_a = 256 - mod_a;

            uint32_t d = *reinterpret_cast<const uint32_t*>(dst + i);
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            uint32_t mod_packed = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
            *reinterpret_cast<uint32_t*>(dst + i) = mod_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        }
    }
}

// -----------------------------------------------------------------------------
// RGB565 and Alpha8 Support Routines
// -----------------------------------------------------------------------------

inline void fill_solid_span_rgb565(
    uint16_t* dst,
    uint16_t val,
    size_t count
) noexcept {
#if defined(NISABA_HAS_AVX2)
    __m256i v = _mm256_set1_epi16(static_cast<short>(val));
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);
    }
    for (; i < count; ++i) {
        dst[i] = val;
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i v = _mm_set1_epi16(static_cast<short>(val));
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
    }
    for (; i < count; ++i) {
        dst[i] = val;
    }
#else
    std::fill_n(dst, count, val);
#endif
}

inline void blend_solid_source_over_rgb565(
    uint16_t* dst,
    uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
    size_t count
) noexcept {
    if (sa == 0) return;
    if (sa == 255) {
        fill_solid_span_rgb565(dst, color_to_rgb565(sr, sg, sb), count);
        return;
    }
    uint32_t inv_a = 256 - sa;
    for (size_t i = 0; i < count; ++i) {
        uint16_t d = dst[i];
        uint32_t dr = (d >> 11) & 0x1F;
        uint32_t dg = (d >> 5) & 0x3F;
        uint32_t db = d & 0x1F;
        dr = (dr << 3) | (dr >> 2);
        dg = (dg << 2) | (dg >> 4);
        db = (db << 3) | (db >> 2);
        uint32_t r = sr + ((dr * inv_a) >> 8);
        uint32_t g = sg + ((dg * inv_a) >> 8);
        uint32_t b = sb + ((db * inv_a) >> 8);
        dst[i] = color_to_rgb565(
            static_cast<uint8_t>(std::min(r, 255u)),
            static_cast<uint8_t>(std::min(g, 255u)),
            static_cast<uint8_t>(std::min(b, 255u))
        );
    }
}

inline void blend_solid_source_over_coverage_rgb565(
    uint16_t* dst,
    uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
    uint8_t cov,
    size_t count
) noexcept {
    if (cov == 0 || sa == 0) return;
    uint32_t c = cov + 1;
    uint8_t mod_r = static_cast<uint8_t>((static_cast<uint32_t>(sr) * c) >> 8);
    uint8_t mod_g = static_cast<uint8_t>((static_cast<uint32_t>(sg) * c) >> 8);
    uint8_t mod_b = static_cast<uint8_t>((static_cast<uint32_t>(sb) * c) >> 8);
    uint8_t mod_a = static_cast<uint8_t>((static_cast<uint32_t>(sa) * c) >> 8);
    blend_solid_source_over_rgb565(dst, mod_r, mod_g, mod_b, mod_a, count);
}

inline void fill_solid_span_alpha8(
    uint8_t* dst,
    uint8_t val,
    size_t count
) noexcept {
    std::memset(dst, val, count);
}

inline void blend_solid_source_over_alpha8(
    uint8_t* dst,
    uint8_t sa,
    size_t count
) noexcept {
    if (sa == 0) return;
    if (sa == 255) {
        fill_solid_span_alpha8(dst, 255, count);
        return;
    }
    uint32_t inv_a = 256 - sa;
    for (size_t i = 0; i < count; ++i) {
        uint32_t da = dst[i];
        uint32_t a = sa + ((da * inv_a) >> 8);
        dst[i] = static_cast<uint8_t>(std::min(a, 255u));
    }
}

inline void blend_solid_source_over_coverage_alpha8(
    uint8_t* dst,
    uint8_t sa,
    uint8_t cov,
    size_t count
) noexcept {
    if (cov == 0 || sa == 0) return;
    uint32_t c = cov + 1;
    uint8_t mod_a = static_cast<uint8_t>((static_cast<uint32_t>(sa) * c) >> 8);
    blend_solid_source_over_alpha8(dst, mod_a, count);
}

/// Blends a solid source color over destination span in linear sRGB space.
inline void blend_solid_source_over_linear_span(
    PremultipliedColorU8* dst,
    PremultipliedColorU8 src,
    uint8_t coverage,
    size_t count
) noexcept {
    if (src.alpha() == 0 || coverage == 0 || count == 0) return;

    uint32_t eff_a = (static_cast<uint32_t>(src.alpha()) * (static_cast<uint32_t>(coverage) + 1)) >> 8;
    if (eff_a == 0) return;
    eff_a = std::min(eff_a, 255u);

    if (eff_a == 255) {
        fill_solid_span(dst, src, count);
        return;
    }

    uint8_t sc_unpremul_r = src.alpha() > 0 ? static_cast<uint8_t>(std::min(255u, (src.red() * 255u) / src.alpha())) : 0;
    uint8_t sc_unpremul_g = src.alpha() > 0 ? static_cast<uint8_t>(std::min(255u, (src.green() * 255u) / src.alpha())) : 0;
    uint8_t sc_unpremul_b = src.alpha() > 0 ? static_cast<uint8_t>(std::min(255u, (src.blue() * 255u) / src.alpha())) : 0;

    uint32_t s_lin_r = ColorSpaceLut::to_linear_u12(sc_unpremul_r);
    uint32_t s_lin_g = ColorSpaceLut::to_linear_u12(sc_unpremul_g);
    uint32_t s_lin_b = ColorSpaceLut::to_linear_u12(sc_unpremul_b);
    uint32_t inv_eff_a = 255 - eff_a;

    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 dp = dst[i];
        if (dp.alpha() == 255) {
            uint8_t dr = ColorSpaceLut::blend_channel_linear(sc_unpremul_r, dp.red(), eff_a);
            uint8_t dg = ColorSpaceLut::blend_channel_linear(sc_unpremul_g, dp.green(), eff_a);
            uint8_t db = ColorSpaceLut::blend_channel_linear(sc_unpremul_b, dp.blue(), eff_a);
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(dr, dg, db, 255);
        } else if (dp.alpha() == 0) {
            uint8_t dr = static_cast<uint8_t>((sc_unpremul_r * eff_a + 127) / 255);
            uint8_t dg = static_cast<uint8_t>((sc_unpremul_g * eff_a + 127) / 255);
            uint8_t db = static_cast<uint8_t>((sc_unpremul_b * eff_a + 127) / 255);
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(dr, dg, db, static_cast<uint8_t>(eff_a));
        } else {
            uint32_t da_out = eff_a + ((static_cast<uint32_t>(dp.alpha()) * inv_eff_a + 127) / 255);
            da_out = std::min(da_out, 255u);

            uint8_t dp_unpremul_r = static_cast<uint8_t>(std::min(255u, (dp.red() * 255u) / dp.alpha()));
            uint8_t dp_unpremul_g = static_cast<uint8_t>(std::min(255u, (dp.green() * 255u) / dp.alpha()));
            uint8_t dp_unpremul_b = static_cast<uint8_t>(std::min(255u, (dp.blue() * 255u) / dp.alpha()));

            uint32_t d_lin_r = ColorSpaceLut::to_linear_u12(dp_unpremul_r);
            uint32_t d_lin_g = ColorSpaceLut::to_linear_u12(dp_unpremul_g);
            uint32_t d_lin_b = ColorSpaceLut::to_linear_u12(dp_unpremul_b);

            uint32_t dst_factor = (static_cast<uint32_t>(dp.alpha()) * inv_eff_a + 127) / 255;
            uint32_t out_lin_r = (s_lin_r * eff_a + d_lin_r * dst_factor + da_out / 2) / da_out;
            uint32_t out_lin_g = (s_lin_g * eff_a + d_lin_g * dst_factor + da_out / 2) / da_out;
            uint32_t out_lin_b = (s_lin_b * eff_a + d_lin_b * dst_factor + da_out / 2) / da_out;

            uint8_t out_srgb_r = ColorSpaceLut::to_srgb(out_lin_r);
            uint8_t out_srgb_g = ColorSpaceLut::to_srgb(out_lin_g);
            uint8_t out_srgb_b = ColorSpaceLut::to_srgb(out_lin_b);

            uint8_t out_pm_r = static_cast<uint8_t>((out_srgb_r * da_out + 127) / 255);
            uint8_t out_pm_g = static_cast<uint8_t>((out_srgb_g * da_out + 127) / 255);
            uint8_t out_pm_b = static_cast<uint8_t>((out_srgb_b * da_out + 127) / 255);
            dst[i] = PremultipliedColorU8::from_rgba_unchecked(out_pm_r, out_pm_g, out_pm_b, static_cast<uint8_t>(da_out));
        }
    }
}

/// Blends a solid source color over RGB565 destination span in linear sRGB space.
inline void blend_solid_source_over_linear_span_rgb565(
    uint16_t* dst,
    uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
    uint8_t coverage,
    size_t count
) noexcept {
    if (sa == 0 || coverage == 0 || count == 0) return;
    uint32_t eff_a = (static_cast<uint32_t>(sa) * (static_cast<uint32_t>(coverage) + 1)) >> 8;
    if (eff_a == 0) return;
    eff_a = std::min(eff_a, 255u);

    for (size_t i = 0; i < count; ++i) {
        uint16_t p = dst[i];
        uint8_t dr = ((p >> 11) & 0x1F) * 255 / 31;
        uint8_t dg = ((p >> 5) & 0x3F) * 255 / 63;
        uint8_t db = (p & 0x1F) * 255 / 31;

        uint8_t out_r = ColorSpaceLut::blend_channel_linear(sr, dr, eff_a);
        uint8_t out_g = ColorSpaceLut::blend_channel_linear(sg, dg, eff_a);
        uint8_t out_b = ColorSpaceLut::blend_channel_linear(sb, db, eff_a);

        dst[i] = ((static_cast<uint16_t>(out_r >> 3) << 11) |
                  (static_cast<uint16_t>(out_g >> 2) << 5)  |
                  static_cast<uint16_t>(out_b >> 3));
    }
}

/// Vectorized mask intersection: dst[i] = premultiply_u8(dst[i], src[i])
inline void multiply_mask_spans(uint8_t* dst, const uint8_t* src, size_t count) noexcept {
    if (count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    for (; i + 32 <= count; i += 32) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));

        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);

        __m256i prod_lo = _mm256_add_epi16(_mm256_mullo_epi16(d_lo, s_lo), c_128);
        __m256i prod_hi = _mm256_add_epi16(_mm256_mullo_epi16(d_hi, s_hi), c_128);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_lo, _mm256_srli_epi16(prod_lo, 8)), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_hi, _mm256_srli_epi16(prod_hi, 8)), 8);

        // unpacklo/unpackhi + packus_epi16 already preserves exact [0..15 | 16..31] byte order in-lane.
        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    for (; i + 16 <= count; i += 16) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));

        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);
        __m128i s_lo = _mm_unpacklo_epi8(s, zero);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero);

        __m128i prod_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, s_lo), c_128);
        __m128i prod_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, s_hi), c_128);

        __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(prod_lo, _mm_srli_epi16(prod_lo, 8)), 8);
        __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(prod_hi, _mm_srli_epi16(prod_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(res_lo, res_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = premultiply_u8(dst[i], src[i]);
    }
}

/// Vectorized 3-way mask intersection: dst[i] = premultiply_u8(cov[i], mask[i])
inline void multiply_mask_spans_2(
    uint8_t* dst,
    const uint8_t* cov,
    const uint8_t* mask,
    size_t count
) noexcept {
    if (count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i ones = _mm256_set1_epi8(static_cast<char>(0xFF));
    __m256i c_128 = _mm256_set1_epi16(128);
    for (; i + 32 <= count; i += 32) {
        __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + i));
        if (_mm256_testz_si256(s, s)) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), zero);
            continue;
        }
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cov + i));
        if (_mm256_testz_si256(d, d)) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), zero);
            continue;
        }
        __m256i cmp_full = _mm256_cmpeq_epi8(s, ones);
        if (_mm256_movemask_epi8(cmp_full) == -1) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), d);
            continue;
        }

        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);

        __m256i prod_lo = _mm256_add_epi16(_mm256_mullo_epi16(d_lo, s_lo), c_128);
        __m256i prod_hi = _mm256_add_epi16(_mm256_mullo_epi16(d_hi, s_hi), c_128);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_lo, _mm256_srli_epi16(prod_lo, 8)), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_hi, _mm256_srli_epi16(prod_hi, 8)), 8);

        // unpacklo/unpackhi + packus_epi16 already preserves exact [0..15 | 16..31] byte order in-lane.
        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i ones = _mm_set1_epi8(static_cast<char>(0xFF));
    __m128i c_128 = _mm_set1_epi16(128);
    for (; i + 16 <= count; i += 16) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(s, zero)) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), zero);
            continue;
        }
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cov + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(d, zero)) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), zero);
            continue;
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(s, ones)) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), d);
            continue;
        }

        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);
        __m128i s_lo = _mm_unpacklo_epi8(s, zero);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero);

        __m128i prod_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, s_lo), c_128);
        __m128i prod_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, s_hi), c_128);

        __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(prod_lo, _mm_srli_epi16(prod_lo, 8)), 8);
        __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(prod_hi, _mm_srli_epi16(prod_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(res_lo, res_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        uint8_t m = mask[i];
        if (m == 0) {
            dst[i] = 0;
        } else if (m == 255) {
            dst[i] = cov[i];
        } else {
            dst[i] = premultiply_u8(cov[i], m);
        }
    }
}

inline PremultipliedColorU8 lerp_pixel(PremultipliedColorU8 d, PremultipliedColorU8 target, uint8_t cov) noexcept {
    if (cov == 255) return target;
    if (cov == 0) return d;
    uint32_t c = cov + 1;
    uint32_t inv_c = 256 - cov;
    uint8_t r = static_cast<uint8_t>((static_cast<uint32_t>(d.red()) * inv_c + static_cast<uint32_t>(target.red()) * c) >> 8);
    uint8_t g = static_cast<uint8_t>((static_cast<uint32_t>(d.green()) * inv_c + static_cast<uint32_t>(target.green()) * c) >> 8);
    uint8_t b = static_cast<uint8_t>((static_cast<uint32_t>(d.blue()) * inv_c + static_cast<uint32_t>(target.blue()) * c) >> 8);
    uint8_t a = static_cast<uint8_t>((static_cast<uint32_t>(d.alpha()) * inv_c + static_cast<uint32_t>(target.alpha()) * c) >> 8);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline PremultipliedColorU8 blend_screen_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint32_t r = static_cast<uint32_t>(s.red()) + d.red() - ((static_cast<uint32_t>(s.red()) * d.red() + 128) * 257 >> 16);
    uint32_t g = static_cast<uint32_t>(s.green()) + d.green() - ((static_cast<uint32_t>(s.green()) * d.green() + 128) * 257 >> 16);
    uint32_t b = static_cast<uint32_t>(s.blue()) + d.blue() - ((static_cast<uint32_t>(s.blue()) * d.blue() + 128) * 257 >> 16);
    uint32_t a = static_cast<uint32_t>(s.alpha()) + d.alpha() - ((static_cast<uint32_t>(s.alpha()) * d.alpha() + 128) * 257 >> 16);
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(r, 255u)),
        static_cast<uint8_t>(std::min(g, 255u)),
        static_cast<uint8_t>(std::min(b, 255u)),
        static_cast<uint8_t>(std::min(a, 255u))
    );
}

inline void blend_solid_screen(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (src.alpha() == 0 || count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i prod_lo = _mm256_mullo_epi16(v_src_16_lo, d_lo);
        __m256i prod_hi = _mm256_mullo_epi16(v_src_16_hi, d_hi);

        __m256i p_add_lo = _mm256_add_epi16(prod_lo, c_128);
        __m256i p_add_hi = _mm256_add_epi16(prod_hi, c_128);
        __m256i div_lo = _mm256_srli_epi16(_mm256_add_epi16(p_add_lo, _mm256_srli_epi16(p_add_lo, 8)), 8);
        __m256i div_hi = _mm256_srli_epi16(_mm256_add_epi16(p_add_hi, _mm256_srli_epi16(p_add_hi, 8)), 8);

        __m256i out_lo = _mm256_sub_epi16(_mm256_add_epi16(v_src_16_lo, d_lo), div_lo);
        __m256i out_hi = _mm256_sub_epi16(_mm256_add_epi16(v_src_16_hi, d_hi), div_hi);

        __m256i res = _mm256_packus_epi16(out_lo, out_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i prod_lo = _mm_mullo_epi16(v_src_16_lo, d_lo);
        __m128i prod_hi = _mm_mullo_epi16(v_src_16_hi, d_hi);

        __m128i p_add_lo = _mm_add_epi16(prod_lo, c_128);
        __m128i p_add_hi = _mm_add_epi16(prod_hi, c_128);
        __m128i div_lo = _mm_srli_epi16(_mm_add_epi16(p_add_lo, _mm_srli_epi16(p_add_lo, 8)), 8);
        __m128i div_hi = _mm_srli_epi16(_mm_add_epi16(p_add_hi, _mm_srli_epi16(p_add_hi, 8)), 8);

        __m128i out_lo = _mm_sub_epi16(_mm_add_epi16(v_src_16_lo, d_lo), div_lo);
        __m128i out_hi = _mm_sub_epi16(_mm_add_epi16(v_src_16_hi, d_hi), div_hi);

        __m128i res = _mm_packus_epi16(out_lo, out_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_screen_pixel(dst[i], src);
    }
}

inline void blend_solid_screen_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_screen(dst, src, count);
        return;
    }
    if (cov == 0 || src.alpha() == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_screen_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_multiply_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint32_t inv_da = 255 - d.alpha();
    uint32_t inv_sa = 255 - s.alpha();
    uint32_t r = (static_cast<uint32_t>(s.red()) * inv_da + static_cast<uint32_t>(d.red()) * inv_sa + static_cast<uint32_t>(s.red()) * d.red() + 128) * 257 >> 16;
    uint32_t g = (static_cast<uint32_t>(s.green()) * inv_da + static_cast<uint32_t>(d.green()) * inv_sa + static_cast<uint32_t>(s.green()) * d.green() + 128) * 257 >> 16;
    uint32_t b = (static_cast<uint32_t>(s.blue()) * inv_da + static_cast<uint32_t>(d.blue()) * inv_sa + static_cast<uint32_t>(s.blue()) * d.blue() + 128) * 257 >> 16;
    uint32_t a = static_cast<uint32_t>(s.alpha()) + static_cast<uint32_t>(d.alpha()) - ((static_cast<uint32_t>(s.alpha()) * d.alpha() + 128) * 257 >> 16);
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(r, 255u)),
        static_cast<uint8_t>(std::min(g, 255u)),
        static_cast<uint8_t>(std::min(b, 255u)),
        static_cast<uint8_t>(std::min(a, 255u))
    );
}

inline void blend_solid_multiply(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (src.alpha() == 0 || count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128_32 = _mm256_set1_epi32(128);
    __m256i c_257_32 = _mm256_set1_epi32(257);
    __m256i c_255_16 = _mm256_set1_epi16(255);

    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);
    __m256i v_inv_sa = _mm256_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255_16, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255_16, da_hi);

        __m256i t1_lo_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(v_src_16_lo, zero), _mm256_unpacklo_epi16(inv_da_lo, zero));
        __m256i t1_lo_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(v_src_16_lo, zero), _mm256_unpackhi_epi16(inv_da_lo, zero));
        __m256i t2_lo_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(d_lo, zero), _mm256_unpacklo_epi16(v_inv_sa, zero));
        __m256i t2_lo_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(d_lo, zero), _mm256_unpackhi_epi16(v_inv_sa, zero));
        __m256i t3_lo_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(v_src_16_lo, zero), _mm256_unpacklo_epi16(d_lo, zero));
        __m256i t3_lo_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(v_src_16_lo, zero), _mm256_unpackhi_epi16(d_lo, zero));

        __m256i sum_lo_0 = _mm256_add_epi32(_mm256_add_epi32(t1_lo_0, t2_lo_0), t3_lo_0);
        __m256i sum_lo_1 = _mm256_add_epi32(_mm256_add_epi32(t1_lo_1, t2_lo_1), t3_lo_1);

        __m256i out32_lo_0 = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_add_epi32(sum_lo_0, c_128_32), c_257_32), 16);
        __m256i out32_lo_1 = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_add_epi32(sum_lo_1, c_128_32), c_257_32), 16);
        __m256i out_lo = _mm256_packs_epi32(out32_lo_0, out32_lo_1);

        __m256i t1_hi_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(v_src_16_hi, zero), _mm256_unpacklo_epi16(inv_da_hi, zero));
        __m256i t1_hi_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(v_src_16_hi, zero), _mm256_unpackhi_epi16(inv_da_hi, zero));
        __m256i t2_hi_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(d_hi, zero), _mm256_unpacklo_epi16(v_inv_sa, zero));
        __m256i t2_hi_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(d_hi, zero), _mm256_unpackhi_epi16(v_inv_sa, zero));
        __m256i t3_hi_0 = _mm256_mullo_epi32(_mm256_unpacklo_epi16(v_src_16_hi, zero), _mm256_unpacklo_epi16(d_hi, zero));
        __m256i t3_hi_1 = _mm256_mullo_epi32(_mm256_unpackhi_epi16(v_src_16_hi, zero), _mm256_unpackhi_epi16(d_hi, zero));

        __m256i sum_hi_0 = _mm256_add_epi32(_mm256_add_epi32(t1_hi_0, t2_hi_0), t3_hi_0);
        __m256i sum_hi_1 = _mm256_add_epi32(_mm256_add_epi32(t1_hi_1, t2_hi_1), t3_hi_1);

        __m256i out32_hi_0 = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_add_epi32(sum_hi_0, c_128_32), c_257_32), 16);
        __m256i out32_hi_1 = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_add_epi32(sum_hi_1, c_128_32), c_257_32), 16);
        __m256i out_hi = _mm256_packs_epi32(out32_hi_0, out32_hi_1);

        __m256i res = _mm256_packus_epi16(out_lo, out_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_multiply_pixel(dst[i], src);
    }
}

inline void blend_solid_multiply_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_multiply(dst, src, count);
        return;
    }
    if (cov == 0 || src.alpha() == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_multiply_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_modulate_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    if (s.alpha() == 0 || d.alpha() == 0) return PremultipliedColorU8();
    return PremultipliedColorU8::from_rgba_unchecked(
        premultiply_u8(s.red(), d.red()),
        premultiply_u8(s.green(), d.green()),
        premultiply_u8(s.blue(), d.blue()),
        premultiply_u8(s.alpha(), d.alpha())
    );
}

inline void blend_solid_modulate(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    if (src.alpha() == 0) {
        std::fill_n(dst, count, PremultipliedColorU8{});
        return;
    }
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i prod_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, d_lo), c_128);
        __m256i prod_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, d_hi), c_128);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_lo, _mm256_srli_epi16(prod_lo, 8)), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_hi, _mm256_srli_epi16(prod_hi, 8)), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i prod_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, d_lo), c_128);
        __m128i prod_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, d_hi), c_128);

        __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(prod_lo, _mm_srli_epi16(prod_lo, 8)), 8);
        __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(prod_hi, _mm_srli_epi16(prod_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(res_lo, res_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_modulate_pixel(dst[i], src);
    }
}

inline PremultipliedColorU8 blend_source_in_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t da = d.alpha();
    if (da == 0) return PremultipliedColorU8();
    if (da == 255) return s;
    return PremultipliedColorU8::from_rgba_unchecked(
        premultiply_u8(s.red(), da),
        premultiply_u8(s.green(), da),
        premultiply_u8(s.blue(), da),
        premultiply_u8(s.alpha(), da)
    );
}

inline void blend_solid_source_in(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    if (src.alpha() == 0) {
        std::fill_n(dst, count, PremultipliedColorU8{});
        return;
    }
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i prod_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, da_lo), c_128);
        __m256i prod_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, da_hi), c_128);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_lo, _mm256_srli_epi16(prod_lo, 8)), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_hi, _mm256_srli_epi16(prod_hi, 8)), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i prod_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, da_lo), c_128);
        __m128i prod_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, da_hi), c_128);

        __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(prod_lo, _mm_srli_epi16(prod_lo, 8)), 8);
        __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(prod_hi, _mm_srli_epi16(prod_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(res_lo, res_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_source_in_pixel(dst[i], src);
    }
}

inline void blend_solid_source_in_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_source_in(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_source_in_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_source_out_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t inv_da = 255 - d.alpha();
    if (inv_da == 0) return PremultipliedColorU8();
    if (inv_da == 255) return s;
    return PremultipliedColorU8::from_rgba_unchecked(
        premultiply_u8(s.red(), inv_da),
        premultiply_u8(s.green(), inv_da),
        premultiply_u8(s.blue(), inv_da),
        premultiply_u8(s.alpha(), inv_da)
    );
}

inline void blend_solid_source_out(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    if (src.alpha() == 0) {
        std::fill_n(dst, count, PremultipliedColorU8{});
        return;
    }
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    __m256i c_255 = _mm256_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255, da_hi);

        __m256i prod_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m256i prod_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_lo, _mm256_srli_epi16(prod_lo, 8)), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_hi, _mm256_srli_epi16(prod_hi, 8)), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    __m128i c_255 = _mm_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i inv_da_lo = _mm_sub_epi16(c_255, da_lo);
        __m128i inv_da_hi = _mm_sub_epi16(c_255, da_hi);

        __m128i prod_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m128i prod_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);

        __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(prod_lo, _mm_srli_epi16(prod_lo, 8)), 8);
        __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(prod_hi, _mm_srli_epi16(prod_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(res_lo, res_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_source_out_pixel(dst[i], src);
    }
}

inline void blend_solid_source_out_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_source_out(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_source_out_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_xor_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t inv_da = 255 - d.alpha();
    uint8_t inv_sa = 255 - s.alpha();
    uint32_t r = premultiply_u8(s.red(), inv_da) + premultiply_u8(d.red(), inv_sa);
    uint32_t g = premultiply_u8(s.green(), inv_da) + premultiply_u8(d.green(), inv_sa);
    uint32_t b = premultiply_u8(s.blue(), inv_da) + premultiply_u8(d.blue(), inv_sa);
    uint32_t a = premultiply_u8(s.alpha(), inv_da) + premultiply_u8(d.alpha(), inv_sa);
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(r, 255u)),
        static_cast<uint8_t>(std::min(g, 255u)),
        static_cast<uint8_t>(std::min(b, 255u)),
        static_cast<uint8_t>(std::min(a, 255u))
    );
}

inline void blend_solid_xor(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    if (src.alpha() == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    __m256i c_255 = _mm256_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);
    __m256i v_inv_sa = _mm256_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255, da_hi);

        __m256i p1_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m256i p1_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m256i r1_lo = _mm256_srli_epi16(_mm256_add_epi16(p1_lo, _mm256_srli_epi16(p1_lo, 8)), 8);
        __m256i r1_hi = _mm256_srli_epi16(_mm256_add_epi16(p1_hi, _mm256_srli_epi16(p1_hi, 8)), 8);

        __m256i p2_lo = _mm256_add_epi16(_mm256_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m256i p2_hi = _mm256_add_epi16(_mm256_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m256i r2_lo = _mm256_srli_epi16(_mm256_add_epi16(p2_lo, _mm256_srli_epi16(p2_lo, 8)), 8);
        __m256i r2_hi = _mm256_srli_epi16(_mm256_add_epi16(p2_hi, _mm256_srli_epi16(p2_hi, 8)), 8);

        __m256i sum_lo = _mm256_add_epi16(r1_lo, r2_lo);
        __m256i sum_hi = _mm256_add_epi16(r1_hi, r2_hi);
        __m256i res = _mm256_packus_epi16(sum_lo, sum_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    __m128i c_255 = _mm_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);
    __m128i v_inv_sa = _mm_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i inv_da_lo = _mm_sub_epi16(c_255, da_lo);
        __m128i inv_da_hi = _mm_sub_epi16(c_255, da_hi);

        __m128i p1_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m128i p1_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m128i r1_lo = _mm_srli_epi16(_mm_add_epi16(p1_lo, _mm_srli_epi16(p1_lo, 8)), 8);
        __m128i r1_hi = _mm_srli_epi16(_mm_add_epi16(p1_hi, _mm_srli_epi16(p1_hi, 8)), 8);

        __m128i p2_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m128i p2_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m128i r2_lo = _mm_srli_epi16(_mm_add_epi16(p2_lo, _mm_srli_epi16(p2_lo, 8)), 8);
        __m128i r2_hi = _mm_srli_epi16(_mm_add_epi16(p2_hi, _mm_srli_epi16(p2_hi, 8)), 8);

        __m128i sum_lo = _mm_add_epi16(r1_lo, r2_lo);
        __m128i sum_hi = _mm_add_epi16(r1_hi, r2_hi);
        __m128i res = _mm_packus_epi16(sum_lo, sum_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_xor_pixel(dst[i], src);
    }
}

inline void blend_solid_xor_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_xor(dst, src, count);
        return;
    }
    if (cov == 0 || src.alpha() == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_xor_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_plus_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(static_cast<uint32_t>(d.red()) + s.red(), 255u)),
        static_cast<uint8_t>(std::min(static_cast<uint32_t>(d.green()) + s.green(), 255u)),
        static_cast<uint8_t>(std::min(static_cast<uint32_t>(d.blue()) + s.blue(), 255u)),
        static_cast<uint8_t>(std::min(static_cast<uint32_t>(d.alpha()) + s.alpha(), 255u))
    );
}

inline void blend_solid_plus(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (src.alpha() == 0 || count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src = _mm256_set1_epi32(static_cast<int>(src_raw));
    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i res = _mm256_adds_epu8(d, v_src);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src = _mm_set1_epi32(static_cast<int>(src_raw));
    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i res = _mm_adds_epu8(d, v_src);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_plus_pixel(dst[i], src);
    }
}

inline void blend_solid_clear(PremultipliedColorU8* dst, size_t count) noexcept {
    if (count == 0) return;
    std::memset(reinterpret_cast<void*>(dst), 0, count * sizeof(PremultipliedColorU8));
}

inline PremultipliedColorU8 blend_dest_out_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    if (sa == 255) return PremultipliedColorU8();
    if (sa == 0) return d;
    uint8_t inv_sa = 255 - sa;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>((static_cast<uint16_t>(d.red()) * inv_sa + 127) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(d.green()) * inv_sa + 127) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(d.blue()) * inv_sa + 127) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(d.alpha()) * inv_sa + 127) / 255)
    );
}

inline void blend_solid_dest_out(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    uint8_t sa = src.alpha();
    if (sa == 255) {
        std::memset(reinterpret_cast<void*>(dst), 0, count * sizeof(PremultipliedColorU8));
        return;
    }
    if (sa == 0) return;
    uint8_t inv_sa = 255 - sa;
    for (size_t i = 0; i < count; ++i) {
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(
            static_cast<uint8_t>((static_cast<uint16_t>(dst[i].red()) * inv_sa + 127) / 255),
            static_cast<uint8_t>((static_cast<uint16_t>(dst[i].green()) * inv_sa + 127) / 255),
            static_cast<uint8_t>((static_cast<uint16_t>(dst[i].blue()) * inv_sa + 127) / 255),
            static_cast<uint8_t>((static_cast<uint16_t>(dst[i].alpha()) * inv_sa + 127) / 255)
        );
    }
}

inline void blend_solid_dest_out_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_dest_out(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_dest_out_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_dest_over_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t da = d.alpha();
    if (da == 255) return d;
    uint8_t inv_da = 255 - da;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min<uint16_t>(255, d.red() + (static_cast<uint16_t>(s.red()) * inv_da + 127) / 255)),
        static_cast<uint8_t>(std::min<uint16_t>(255, d.green() + (static_cast<uint16_t>(s.green()) * inv_da + 127) / 255)),
        static_cast<uint8_t>(std::min<uint16_t>(255, d.blue() + (static_cast<uint16_t>(s.blue()) * inv_da + 127) / 255)),
        static_cast<uint8_t>(std::min<uint16_t>(255, da + (static_cast<uint16_t>(s.alpha()) * inv_da + 127) / 255))
    );
}

inline void blend_solid_dest_over(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    for (size_t i = 0; i < count; ++i) {
        uint8_t da = dst[i].alpha();
        if (da == 255) continue;
        uint8_t inv_da = 255 - da;
        uint16_t out_r = dst[i].red() + (static_cast<uint16_t>(src.red()) * inv_da + 127) / 255;
        uint16_t out_g = dst[i].green() + (static_cast<uint16_t>(src.green()) * inv_da + 127) / 255;
        uint16_t out_b = dst[i].blue() + (static_cast<uint16_t>(src.blue()) * inv_da + 127) / 255;
        uint16_t out_a = da + (static_cast<uint16_t>(src.alpha()) * inv_da + 127) / 255;
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(
            static_cast<uint8_t>(std::min(out_r, static_cast<uint16_t>(255))),
            static_cast<uint8_t>(std::min(out_g, static_cast<uint16_t>(255))),
            static_cast<uint8_t>(std::min(out_b, static_cast<uint16_t>(255))),
            static_cast<uint8_t>(std::min(out_a, static_cast<uint16_t>(255)))
        );
    }
}

inline void blend_solid_dest_over_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_dest_over(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_dest_over_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// Porter-Duff: SourceAtop & DestinationAtop
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_source_atop_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t da = d.alpha();
    if (da == 0) return PremultipliedColorU8();
    uint8_t sa = s.alpha();
    uint8_t inv_sa = 255 - sa;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(s.red()) * da + static_cast<uint32_t>(d.red()) * inv_sa + 127) / 255)),
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(s.green()) * da + static_cast<uint32_t>(d.green()) * inv_sa + 127) / 255)),
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(s.blue()) * da + static_cast<uint32_t>(d.blue()) * inv_sa + 127) / 255)),
        da
    );
}

inline void blend_solid_source_atop(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    uint32_t s_rgb0 = (static_cast<uint32_t>(src.red())) | (static_cast<uint32_t>(src.green()) << 8) | (static_cast<uint32_t>(src.blue()) << 16);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(s_rgb0));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);
    short inv_sa = static_cast<short>(255 - src.alpha());
    __m256i v_inv_sa = _mm256_setr_epi16(inv_sa, inv_sa, inv_sa, 255, inv_sa, inv_sa, inv_sa, 255,
                                         inv_sa, inv_sa, inv_sa, 255, inv_sa, inv_sa, inv_sa, 255);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i p1_lo = _mm256_mullo_epi16(v_src_16_lo, da_lo);
        __m256i p1_hi = _mm256_mullo_epi16(v_src_16_hi, da_hi);

        __m256i p2_lo = _mm256_mullo_epi16(d_lo, v_inv_sa);
        __m256i p2_hi = _mm256_mullo_epi16(d_hi, v_inv_sa);

        __m256i sum_lo = _mm256_add_epi16(_mm256_add_epi16(p1_lo, p2_lo), c_128);
        __m256i sum_hi = _mm256_add_epi16(_mm256_add_epi16(p1_hi, p2_hi), c_128);

        __m256i r_lo = _mm256_srli_epi16(_mm256_add_epi16(sum_lo, _mm256_srli_epi16(sum_lo, 8)), 8);
        __m256i r_hi = _mm256_srli_epi16(_mm256_add_epi16(sum_hi, _mm256_srli_epi16(sum_hi, 8)), 8);

        __m256i res = _mm256_packus_epi16(r_lo, r_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    uint32_t s_rgb0 = (static_cast<uint32_t>(src.red())) | (static_cast<uint32_t>(src.green()) << 8) | (static_cast<uint32_t>(src.blue()) << 16);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(s_rgb0));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);
    short inv_sa = static_cast<short>(255 - src.alpha());
    __m128i v_inv_sa = _mm_setr_epi16(inv_sa, inv_sa, inv_sa, 255, inv_sa, inv_sa, inv_sa, 255);

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i p1_lo = _mm_mullo_epi16(v_src_16_lo, da_lo);
        __m128i p1_hi = _mm_mullo_epi16(v_src_16_hi, da_hi);

        __m128i p2_lo = _mm_mullo_epi16(d_lo, v_inv_sa);
        __m128i p2_hi = _mm_mullo_epi16(d_hi, v_inv_sa);

        __m128i sum_lo = _mm_add_epi16(_mm_add_epi16(p1_lo, p2_lo), c_128);
        __m128i sum_hi = _mm_add_epi16(_mm_add_epi16(p1_hi, p2_hi), c_128);

        __m128i r_lo = _mm_srli_epi16(_mm_add_epi16(sum_lo, _mm_srli_epi16(sum_lo, 8)), 8);
        __m128i r_hi = _mm_srli_epi16(_mm_add_epi16(sum_hi, _mm_srli_epi16(sum_hi, 8)), 8);

        __m128i res = _mm_packus_epi16(r_lo, r_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_source_atop_pixel(dst[i], src);
    }
}

inline void blend_solid_source_atop_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_source_atop(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_source_atop_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_dest_atop_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    if (sa == 0) return PremultipliedColorU8();
    uint8_t da = d.alpha();
    uint8_t inv_da = 255 - da;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(d.red()) * sa + static_cast<uint32_t>(s.red()) * inv_da + 127) / 255)),
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(d.green()) * sa + static_cast<uint32_t>(s.green()) * inv_da + 127) / 255)),
        static_cast<uint8_t>(std::min(255u, (static_cast<uint32_t>(d.blue()) * sa + static_cast<uint32_t>(s.blue()) * inv_da + 127) / 255)),
        sa
    );
}

inline void blend_solid_dest_atop(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    __m256i c_255 = _mm256_set1_epi16(255);
    short sa = static_cast<short>(src.alpha());
    __m256i v_sa = _mm256_setr_epi16(sa, sa, sa, 0, sa, sa, sa, 0,
                                     sa, sa, sa, 0, sa, sa, sa, 0);
    uint32_t s_rgb_a255 = (static_cast<uint32_t>(src.red())) | (static_cast<uint32_t>(src.green()) << 8) | (static_cast<uint32_t>(src.blue()) << 16) | (255u << 24);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(s_rgb_a255));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255, da_hi);
        __m256i v_mult_s_lo = _mm256_blend_epi16(inv_da_lo, _mm256_set1_epi16(sa), 0x88);
        __m256i v_mult_s_hi = _mm256_blend_epi16(inv_da_hi, _mm256_set1_epi16(sa), 0x88);

        __m256i p1_lo = _mm256_mullo_epi16(d_lo, v_sa);
        __m256i p1_hi = _mm256_mullo_epi16(d_hi, v_sa);

        __m256i p2_lo = _mm256_mullo_epi16(v_src_16_lo, v_mult_s_lo);
        __m256i p2_hi = _mm256_mullo_epi16(v_src_16_hi, v_mult_s_hi);

        __m256i sum_lo = _mm256_add_epi16(_mm256_add_epi16(p1_lo, p2_lo), c_128);
        __m256i sum_hi = _mm256_add_epi16(_mm256_add_epi16(p1_hi, p2_hi), c_128);

        __m256i r_lo = _mm256_srli_epi16(_mm256_add_epi16(sum_lo, _mm256_srli_epi16(sum_lo, 8)), 8);
        __m256i r_hi = _mm256_srli_epi16(_mm256_add_epi16(sum_hi, _mm256_srli_epi16(sum_hi, 8)), 8);

        __m256i res = _mm256_packus_epi16(r_lo, r_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_dest_atop_pixel(dst[i], src);
    }
}

inline void blend_solid_dest_atop_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_dest_atop(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_dest_atop_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// Darken & Lighten
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_darken_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    uint16_t s_r_term = s.red() + (static_cast<uint16_t>(d.red()) * inv_sa + 127) / 255;
    uint16_t d_r_term = d.red() + (static_cast<uint16_t>(s.red()) * inv_da + 127) / 255;
    uint8_t r = static_cast<uint8_t>(std::min(s_r_term, d_r_term));

    uint16_t s_g_term = s.green() + (static_cast<uint16_t>(d.green()) * inv_sa + 127) / 255;
    uint16_t d_g_term = d.green() + (static_cast<uint16_t>(s.green()) * inv_da + 127) / 255;
    uint8_t g = static_cast<uint8_t>(std::min(s_g_term, d_g_term));

    uint16_t s_b_term = s.blue() + (static_cast<uint16_t>(d.blue()) * inv_sa + 127) / 255;
    uint16_t d_b_term = d.blue() + (static_cast<uint16_t>(s.blue()) * inv_da + 127) / 255;
    uint8_t b = static_cast<uint8_t>(std::min(s_b_term, d_b_term));

    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_darken(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    __m256i c_255 = _mm256_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);
    __m256i v_inv_sa = _mm256_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255, da_hi);

        __m256i prod_d_inv_sa_lo = _mm256_add_epi16(_mm256_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m256i prod_d_inv_sa_hi = _mm256_add_epi16(_mm256_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m256i d_inv_sa_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_d_inv_sa_lo, _mm256_srli_epi16(prod_d_inv_sa_lo, 8)), 8);
        __m256i d_inv_sa_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_d_inv_sa_hi, _mm256_srli_epi16(prod_d_inv_sa_hi, 8)), 8);
        __m256i s_term_lo = _mm256_add_epi16(v_src_16_lo, d_inv_sa_lo);
        __m256i s_term_hi = _mm256_add_epi16(v_src_16_hi, d_inv_sa_hi);

        __m256i prod_s_inv_da_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m256i prod_s_inv_da_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m256i s_inv_da_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_s_inv_da_lo, _mm256_srli_epi16(prod_s_inv_da_lo, 8)), 8);
        __m256i s_inv_da_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_s_inv_da_hi, _mm256_srli_epi16(prod_s_inv_da_hi, 8)), 8);
        __m256i d_term_lo = _mm256_add_epi16(d_lo, s_inv_da_lo);
        __m256i d_term_hi = _mm256_add_epi16(d_hi, s_inv_da_hi);

        __m256i min_lo = _mm256_min_epu16(s_term_lo, d_term_lo);
        __m256i min_hi = _mm256_min_epu16(s_term_hi, d_term_hi);

        __m256i res = _mm256_packus_epi16(min_lo, min_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    __m128i c_255 = _mm_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);
    __m128i v_inv_sa = _mm_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i inv_da_lo = _mm_sub_epi16(c_255, da_lo);
        __m128i inv_da_hi = _mm_sub_epi16(c_255, da_hi);

        __m128i prod_d_inv_sa_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m128i prod_d_inv_sa_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m128i d_inv_sa_lo = _mm_srli_epi16(_mm_add_epi16(prod_d_inv_sa_lo, _mm_srli_epi16(prod_d_inv_sa_lo, 8)), 8);
        __m128i d_inv_sa_hi = _mm_srli_epi16(_mm_add_epi16(prod_d_inv_sa_hi, _mm_srli_epi16(prod_d_inv_sa_hi, 8)), 8);
        __m128i s_term_lo = _mm_add_epi16(v_src_16_lo, d_inv_sa_lo);
        __m128i s_term_hi = _mm_add_epi16(v_src_16_hi, d_inv_sa_hi);

        __m128i prod_s_inv_da_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m128i prod_s_inv_da_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m128i s_inv_da_lo = _mm_srli_epi16(_mm_add_epi16(prod_s_inv_da_lo, _mm_srli_epi16(prod_s_inv_da_lo, 8)), 8);
        __m128i s_inv_da_hi = _mm_srli_epi16(_mm_add_epi16(prod_s_inv_da_hi, _mm_srli_epi16(prod_s_inv_da_hi, 8)), 8);
        __m128i d_term_lo = _mm_add_epi16(d_lo, s_inv_da_lo);
        __m128i d_term_hi = _mm_add_epi16(d_hi, s_inv_da_hi);

        __m128i min_lo = _mm_min_epu16(s_term_lo, d_term_lo);
        __m128i min_hi = _mm_min_epu16(s_term_hi, d_term_hi);

        __m128i res = _mm_packus_epi16(min_lo, min_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_darken_pixel(dst[i], src);
    }
}

inline void blend_solid_darken_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_darken(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_darken_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_lighten_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    uint16_t s_r_term = s.red() + (static_cast<uint16_t>(d.red()) * inv_sa + 127) / 255;
    uint16_t d_r_term = d.red() + (static_cast<uint16_t>(s.red()) * inv_da + 127) / 255;
    uint8_t r = static_cast<uint8_t>(std::min<uint16_t>(255, std::max(s_r_term, d_r_term)));

    uint16_t s_g_term = s.green() + (static_cast<uint16_t>(d.green()) * inv_sa + 127) / 255;
    uint16_t d_g_term = d.green() + (static_cast<uint16_t>(s.green()) * inv_da + 127) / 255;
    uint8_t g = static_cast<uint8_t>(std::min<uint16_t>(255, std::max(s_g_term, d_g_term)));

    uint16_t s_b_term = s.blue() + (static_cast<uint16_t>(d.blue()) * inv_sa + 127) / 255;
    uint16_t d_b_term = d.blue() + (static_cast<uint16_t>(s.blue()) * inv_da + 127) / 255;
    uint8_t b = static_cast<uint8_t>(std::min<uint16_t>(255, std::max(s_b_term, d_b_term)));

    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_lighten(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    size_t i = 0;
#if defined(NISABA_HAS_AVX2)
    __m256i zero = _mm256_setzero_si256();
    __m256i c_128 = _mm256_set1_epi16(128);
    __m256i c_255 = _mm256_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);
    __m256i v_inv_sa = _mm256_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 8 <= count; i += 8) {
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i da_lo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        __m256i da_hi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

        __m256i inv_da_lo = _mm256_sub_epi16(c_255, da_lo);
        __m256i inv_da_hi = _mm256_sub_epi16(c_255, da_hi);

        __m256i prod_d_inv_sa_lo = _mm256_add_epi16(_mm256_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m256i prod_d_inv_sa_hi = _mm256_add_epi16(_mm256_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m256i d_inv_sa_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_d_inv_sa_lo, _mm256_srli_epi16(prod_d_inv_sa_lo, 8)), 8);
        __m256i d_inv_sa_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_d_inv_sa_hi, _mm256_srli_epi16(prod_d_inv_sa_hi, 8)), 8);
        __m256i s_term_lo = _mm256_add_epi16(v_src_16_lo, d_inv_sa_lo);
        __m256i s_term_hi = _mm256_add_epi16(v_src_16_hi, d_inv_sa_hi);

        __m256i prod_s_inv_da_lo = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m256i prod_s_inv_da_hi = _mm256_add_epi16(_mm256_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m256i s_inv_da_lo = _mm256_srli_epi16(_mm256_add_epi16(prod_s_inv_da_lo, _mm256_srli_epi16(prod_s_inv_da_lo, 8)), 8);
        __m256i s_inv_da_hi = _mm256_srli_epi16(_mm256_add_epi16(prod_s_inv_da_hi, _mm256_srli_epi16(prod_s_inv_da_hi, 8)), 8);
        __m256i d_term_lo = _mm256_add_epi16(d_lo, s_inv_da_lo);
        __m256i d_term_hi = _mm256_add_epi16(d_hi, s_inv_da_hi);

        __m256i max_lo = _mm256_max_epu16(s_term_lo, d_term_lo);
        __m256i max_hi = _mm256_max_epu16(s_term_hi, d_term_hi);

        __m256i res = _mm256_packus_epi16(max_lo, max_hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), res);
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i zero = _mm_setzero_si128();
    __m128i c_128 = _mm_set1_epi16(128);
    __m128i c_255 = _mm_set1_epi16(255);
    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);
    __m128i v_inv_sa = _mm_set1_epi16(static_cast<short>(255 - src.alpha()));

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        __m128i da_lo = _mm_shufflelo_epi16(d_lo, _MM_SHUFFLE(3, 3, 3, 3));
        da_lo = _mm_shufflehi_epi16(da_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i da_hi = _mm_shufflelo_epi16(d_hi, _MM_SHUFFLE(3, 3, 3, 3));
        da_hi = _mm_shufflehi_epi16(da_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m128i inv_da_lo = _mm_sub_epi16(c_255, da_lo);
        __m128i inv_da_hi = _mm_sub_epi16(c_255, da_hi);

        __m128i prod_d_inv_sa_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, v_inv_sa), c_128);
        __m128i prod_d_inv_sa_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, v_inv_sa), c_128);
        __m128i d_inv_sa_lo = _mm_srli_epi16(_mm_add_epi16(prod_d_inv_sa_lo, _mm_srli_epi16(prod_d_inv_sa_lo, 8)), 8);
        __m128i d_inv_sa_hi = _mm_srli_epi16(_mm_add_epi16(prod_d_inv_sa_hi, _mm_srli_epi16(prod_d_inv_sa_hi, 8)), 8);
        __m128i s_term_lo = _mm_add_epi16(v_src_16_lo, d_inv_sa_lo);
        __m128i s_term_hi = _mm_add_epi16(v_src_16_hi, d_inv_sa_hi);

        __m128i prod_s_inv_da_lo = _mm_add_epi16(_mm_mullo_epi16(v_src_16_lo, inv_da_lo), c_128);
        __m128i prod_s_inv_da_hi = _mm_add_epi16(_mm_mullo_epi16(v_src_16_hi, inv_da_hi), c_128);
        __m128i s_inv_da_lo = _mm_srli_epi16(_mm_add_epi16(prod_s_inv_da_lo, _mm_srli_epi16(prod_s_inv_da_lo, 8)), 8);
        __m128i s_inv_da_hi = _mm_srli_epi16(_mm_add_epi16(prod_s_inv_da_hi, _mm_srli_epi16(prod_s_inv_da_hi, 8)), 8);
        __m128i d_term_lo = _mm_add_epi16(d_lo, s_inv_da_lo);
        __m128i d_term_hi = _mm_add_epi16(d_hi, s_inv_da_hi);

        __m128i max_lo = _mm_max_epu16(s_term_lo, d_term_lo);
        __m128i max_hi = _mm_max_epu16(s_term_hi, d_term_hi);

        __m128i res = _mm_packus_epi16(max_lo, max_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), res);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = blend_lighten_pixel(dst[i], src);
    }
}

inline void blend_solid_lighten_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_lighten(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_lighten_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// Difference & Exclusion
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_difference_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    auto diff_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        int32_t s_da = static_cast<int32_t>(sc) * da;
        int32_t d_sa = static_cast<int32_t>(dc) * sa;
        int32_t delta = std::abs(s_da - d_sa);
        int32_t term = static_cast<int32_t>(sc) * inv_da + static_cast<int32_t>(dc) * inv_sa;
        return static_cast<uint8_t>(std::min(255, (term + delta + 127) / 255));
    };

    uint8_t r = diff_ch(s.red(), d.red());
    uint8_t g = diff_ch(s.green(), d.green());
    uint8_t b = diff_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_difference(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_difference_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_difference_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_difference(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_difference_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_exclusion_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    auto excl_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        int32_t term = static_cast<int32_t>(sc) * inv_da + static_cast<int32_t>(dc) * inv_sa;
        int32_t ch = static_cast<int32_t>(sc) * da + static_cast<int32_t>(dc) * sa - 2 * static_cast<int32_t>(sc) * dc;
        int32_t res = (term + ch + 127) / 255;
        return static_cast<uint8_t>(std::clamp(res, 0, 255));
    };

    uint8_t r = excl_ch(s.red(), d.red());
    uint8_t g = excl_ch(s.green(), d.green());
    uint8_t b = excl_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_exclusion(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_exclusion_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_exclusion_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_exclusion(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_exclusion_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// Overlay & HardLight
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_overlay_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    auto overlay_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        int32_t ch;
        if (2 * dc <= da) {
            ch = (2 * static_cast<int32_t>(sc) * dc + 127) / 255;
        } else {
            ch = static_cast<int32_t>(sa) * da - 2 * (static_cast<int32_t>(da) - dc) * (static_cast<int32_t>(sa) - sc);
            ch = (ch + 127) / 255;
        }
        int32_t res = (static_cast<int32_t>(sc) * inv_da + static_cast<int32_t>(dc) * inv_sa + 127) / 255 + ch;
        return static_cast<uint8_t>(std::clamp(res, 0, 255));
    };

    uint8_t r = overlay_ch(s.red(), d.red());
    uint8_t g = overlay_ch(s.green(), d.green());
    uint8_t b = overlay_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_overlay(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_overlay_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_overlay_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_overlay(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_overlay_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_hard_light_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    uint8_t inv_sa = 255 - sa;
    uint8_t inv_da = 255 - da;

    auto hard_light_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        int32_t ch;
        if (2 * sc <= sa) {
            ch = (2 * static_cast<int32_t>(sc) * dc + 127) / 255;
        } else {
            ch = static_cast<int32_t>(sa) * da - 2 * (static_cast<int32_t>(da) - dc) * (static_cast<int32_t>(sa) - sc);
            ch = (ch + 127) / 255;
        }
        int32_t res = (static_cast<int32_t>(sc) * inv_da + static_cast<int32_t>(dc) * inv_sa + 127) / 255 + ch;
        return static_cast<uint8_t>(std::clamp(res, 0, 255));
    };

    uint8_t r = hard_light_ch(s.red(), d.red());
    uint8_t g = hard_light_ch(s.green(), d.green());
    uint8_t b = hard_light_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint16_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_hard_light(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_hard_light_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_hard_light_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_hard_light(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_hard_light_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// SoftLight, ColorDodge & ColorBurn
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_soft_light_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    float sa = static_cast<float>(s.alpha()) / 255.0f;
    float da = static_cast<float>(d.alpha()) / 255.0f;
    if (sa <= 0.0f) return d;
    if (da <= 0.0f) return s;

    float sr = static_cast<float>(s.red()) / 255.0f;
    float sg = static_cast<float>(s.green()) / 255.0f;
    float sb = static_cast<float>(s.blue()) / 255.0f;
    float dr = static_cast<float>(d.red()) / 255.0f;
    float dg = static_cast<float>(d.green()) / 255.0f;
    float db = static_cast<float>(d.blue()) / 255.0f;

    auto soft_light_ch = [](float sc, float dc, float s_a, float d_a) noexcept -> float {
        float d_norm = dc / d_a;
        float d_func = (d_norm <= 0.25f)
            ? (((16.0f * d_norm - 12.0f) * d_norm + 4.0f) * d_norm)
            : std::sqrt(d_norm);
        float ch = (2.0f * sc <= s_a)
            ? (dc * (s_a + (2.0f * sc - s_a) * (1.0f - d_norm)))
            : (dc * s_a + (2.0f * sc - s_a) * (d_func * d_a - dc));
        return sc * (1.0f - d_a) + dc * (1.0f - s_a) + ch;
    };

    float r = soft_light_ch(sr, dr, sa, da);
    float g = soft_light_ch(sg, dg, sa, da);
    float b = soft_light_ch(sb, db, sa, da);
    float a = sa + da - sa * da;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f))
    );
}

inline void blend_solid_soft_light(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_soft_light_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_soft_light_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_soft_light(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_soft_light_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_color_dodge_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    if (sa == 0) return d;
    if (da == 0) return s;
    uint32_t inv_sa = 255 - sa;
    uint32_t inv_da = 255 - da;
    uint32_t sada = static_cast<uint32_t>(sa) * da;

    auto dodge_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        if (dc == 0) return static_cast<uint8_t>((static_cast<uint32_t>(sc) * inv_da + 127) / 255);
        uint32_t ch;
        if (sc >= sa) {
            ch = sada;
        } else {
            uint32_t den = static_cast<uint32_t>(sa - sc);
            uint32_t term = (static_cast<uint32_t>(dc) * sa * sa + (den >> 1)) / den;
            ch = std::min(term, sada);
        }
        uint32_t res = (static_cast<uint32_t>(sc) * inv_da + static_cast<uint32_t>(dc) * inv_sa + ch + 127) / 255;
        return static_cast<uint8_t>(std::min(res, 255u));
    };

    uint8_t r = dodge_ch(s.red(), d.red());
    uint8_t g = dodge_ch(s.green(), d.green());
    uint8_t b = dodge_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint32_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_color_dodge(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_color_dodge_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_color_dodge_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_color_dodge(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_color_dodge_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

inline PremultipliedColorU8 blend_color_burn_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s) noexcept {
    uint8_t sa = s.alpha();
    uint8_t da = d.alpha();
    if (sa == 0) return d;
    if (da == 0) return s;
    uint32_t inv_sa = 255 - sa;
    uint32_t inv_da = 255 - da;
    uint32_t sada = static_cast<uint32_t>(sa) * da;

    auto burn_ch = [&](uint8_t sc, uint8_t dc) noexcept -> uint8_t {
        if (dc >= da) {
            uint32_t res = (sada + static_cast<uint32_t>(sc) * inv_da + static_cast<uint32_t>(dc) * inv_sa + 127) / 255;
            return static_cast<uint8_t>(std::min(res, 255u));
        }
        if (sc == 0) return static_cast<uint8_t>((static_cast<uint32_t>(dc) * inv_sa + 127) / 255);
        uint32_t sub = (static_cast<uint32_t>(da - dc) * sa * sa + (sc >> 1)) / sc;
        uint32_t ch = (sub >= sada) ? 0u : (sada - sub);
        uint32_t res = (static_cast<uint32_t>(sc) * inv_da + static_cast<uint32_t>(dc) * inv_sa + ch + 127) / 255;
        return static_cast<uint8_t>(std::min(res, 255u));
    };

    uint8_t r = burn_ch(s.red(), d.red());
    uint8_t g = burn_ch(s.green(), d.green());
    uint8_t b = burn_ch(s.blue(), d.blue());
    uint8_t a = static_cast<uint8_t>(sa + (static_cast<uint32_t>(da) * inv_sa + 127) / 255);
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

inline void blend_solid_color_burn(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_color_burn_pixel(dst[i], src);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_color_burn_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_color_burn(dst, src, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_color_burn_pixel(dst[i], src);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// HSL Non-Separable Blend Modes: Hue, Saturation, Color, Luminosity
// -----------------------------------------------------------------------------

namespace detail {

inline float rgb_lum(float r, float g, float b) noexcept {
    return 0.3f * r + 0.59f * g + 0.11f * b;
}

inline void rgb_set_lum(float& r, float& g, float& b, float l) noexcept {
    float d = l - rgb_lum(r, g, b);
    r += d; g += d; b += d;
    float l_val = rgb_lum(r, g, b);
    float n = std::min({r, g, b});
    float m = std::max({r, g, b});
    if (n < 0.0f) {
        float f = l_val - n;
        if (f > 1e-5f) {
            r = l_val + (r - l_val) * l_val / f;
            g = l_val + (g - l_val) * l_val / f;
            b = l_val + (b - l_val) * l_val / f;
        } else {
            r = g = b = l_val;
        }
    }
    if (m > 1.0f) {
        float f = m - l_val;
        if (f > 1e-5f) {
            r = l_val + (r - l_val) * (1.0f - l_val) / f;
            g = l_val + (g - l_val) * (1.0f - l_val) / f;
            b = l_val + (b - l_val) * (1.0f - l_val) / f;
        } else {
            r = g = b = l_val;
        }
    }
}

inline float rgb_sat(float r, float g, float b) noexcept {
    return std::max({r, g, b}) - std::min({r, g, b});
}

inline void rgb_set_sat(float& r, float& g, float& b, float s) noexcept {
    float* c[3] = {&r, &g, &b};
    if (*c[0] > *c[1]) std::swap(c[0], c[1]);
    if (*c[1] > *c[2]) std::swap(c[1], c[2]);
    if (*c[0] > *c[1]) std::swap(c[0], c[1]);
    if (*c[2] > *c[0]) {
        *c[1] = ((*c[1] - *c[0]) * s) / (*c[2] - *c[0]);
        *c[2] = s;
    } else {
        *c[1] = *c[2] = 0.0f;
    }
    *c[0] = 0.0f;
}

} // namespace detail

inline PremultipliedColorU8 blend_hsl_pixel(PremultipliedColorU8 d, PremultipliedColorU8 s, BlendMode mode) noexcept {
    float sa = static_cast<float>(s.alpha()) / 255.0f;
    float da = static_cast<float>(d.alpha()) / 255.0f;
    if (sa <= 0.0f) return d;
    if (da <= 0.0f) return s;

    float sr = static_cast<float>(s.red()) / (255.0f * sa);
    float sg = static_cast<float>(s.green()) / (255.0f * sa);
    float sb = static_cast<float>(s.blue()) / (255.0f * sa);

    float dr = static_cast<float>(d.red()) / (255.0f * da);
    float dg = static_cast<float>(d.green()) / (255.0f * da);
    float db = static_cast<float>(d.blue()) / (255.0f * da);

    float cr = dr, cg = dg, cb = db;
    if (mode == BlendMode::Hue) {
        float r = sr, g = sg, b = sb;
        detail::rgb_set_sat(r, g, b, detail::rgb_sat(dr, dg, db));
        detail::rgb_set_lum(r, g, b, detail::rgb_lum(dr, dg, db));
        cr = r; cg = g; cb = b;
    } else if (mode == BlendMode::Saturation) {
        float r = dr, g = dg, b = db;
        detail::rgb_set_sat(r, g, b, detail::rgb_sat(sr, sg, sb));
        detail::rgb_set_lum(r, g, b, detail::rgb_lum(dr, dg, db));
        cr = r; cg = g; cb = b;
    } else if (mode == BlendMode::Color) {
        float r = sr, g = sg, b = sb;
        detail::rgb_set_lum(r, g, b, detail::rgb_lum(dr, dg, db));
        cr = r; cg = g; cb = b;
    } else if (mode == BlendMode::Luminosity) {
        float r = dr, g = dg, b = db;
        detail::rgb_set_lum(r, g, b, detail::rgb_lum(sr, sg, sb));
        cr = r; cg = g; cb = b;
    }

    float out_r = sr * sa * (1.0f - da) + dr * da * (1.0f - sa) + sa * da * cr;
    float out_g = sg * sa * (1.0f - da) + dg * da * (1.0f - sa) + sa * da * cg;
    float out_b = sb * sa * (1.0f - da) + db * da * (1.0f - sa) + sa * da * cb;
    float out_a = sa + da - sa * da;

    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::clamp(out_r * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(out_g * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(out_b * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(out_a * 255.0f + 0.5f, 0.0f, 255.0f))
    );
}

inline void blend_solid_hsl(PremultipliedColorU8* dst, PremultipliedColorU8 src, BlendMode mode, size_t count) noexcept {
    if (count == 0 || src.alpha() == 0) return;
    uint32_t last_d = 0;
    bool has_last = false;
    PremultipliedColorU8 last_res{};
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur_d = *reinterpret_cast<const uint32_t*>(dst + i);
        if (has_last && cur_d == last_d) {
            dst[i] = last_res;
        } else {
            last_d = cur_d;
            has_last = true;
            last_res = blend_hsl_pixel(dst[i], src, mode);
            dst[i] = last_res;
        }
    }
}

inline void blend_solid_hsl_coverage(PremultipliedColorU8* dst, PremultipliedColorU8 src, BlendMode mode, uint8_t cov, size_t count) noexcept {
    if (cov == 255) {
        blend_solid_hsl(dst, src, mode, count);
        return;
    }
    if (cov == 0 || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        PremultipliedColorU8 target = blend_hsl_pixel(dst[i], src, mode);
        dst[i] = lerp_pixel(dst[i], target, cov);
    }
}

// -----------------------------------------------------------------------------
// Unified Fast Dispatcher for all 29 Blend Modes
// -----------------------------------------------------------------------------

inline PremultipliedColorU8 blend_pixel_by_mode(PremultipliedColorU8 d, PremultipliedColorU8 s, BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Clear: return PremultipliedColorU8();
        case BlendMode::Source: return s;
        case BlendMode::Destination: return d;
        case BlendMode::SourceOver: {
            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
            return PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8)),
                static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8)),
                static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8)),
                static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8))
            );
        }
        case BlendMode::DestinationOver: return blend_dest_over_pixel(d, s);
        case BlendMode::SourceIn: return blend_source_in_pixel(d, s);
        case BlendMode::DestinationIn: return blend_source_in_pixel(s, d);
        case BlendMode::SourceOut: return blend_source_out_pixel(d, s);
        case BlendMode::DestinationOut: return blend_dest_out_pixel(d, s);
        case BlendMode::SourceAtop: return blend_source_atop_pixel(d, s);
        case BlendMode::DestinationAtop: return blend_dest_atop_pixel(d, s);
        case BlendMode::Xor: return blend_xor_pixel(d, s);
        case BlendMode::Plus: return blend_plus_pixel(d, s);
        case BlendMode::Modulate: return blend_modulate_pixel(d, s);
        case BlendMode::Screen: return blend_screen_pixel(d, s);
        case BlendMode::Multiply: return blend_multiply_pixel(d, s);
        case BlendMode::Darken: return blend_darken_pixel(d, s);
        case BlendMode::Lighten: return blend_lighten_pixel(d, s);
        case BlendMode::Difference: return blend_difference_pixel(d, s);
        case BlendMode::Exclusion: return blend_exclusion_pixel(d, s);
        case BlendMode::Overlay: return blend_overlay_pixel(d, s);
        case BlendMode::HardLight: return blend_hard_light_pixel(d, s);
        case BlendMode::SoftLight: return blend_soft_light_pixel(d, s);
        case BlendMode::ColorDodge: return blend_color_dodge_pixel(d, s);
        case BlendMode::ColorBurn: return blend_color_burn_pixel(d, s);
        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity: return blend_hsl_pixel(d, s, mode);
        default: return blend_dest_over_pixel(s, d);
    }
}

inline void blend_solid_rect_source_over(
    PremultipliedColorU8* dst,
    size_t stride,
    PremultipliedColorU8 src,
    uint32_t w,
    uint32_t h
) noexcept {
    if (src.alpha() == 255) {
        for (uint32_t y = 0; y < h; ++y) {
            fill_solid_span(dst + y * stride, src, w);
        }
        return;
    }
    if (src.alpha() == 0 || w == 0 || h == 0) return;

    uint32_t inv_a = 256 - static_cast<uint32_t>(src.alpha());

#if defined(NISABA_HAS_AVX2)
    __m256i v_inv_a = _mm256_set1_epi16(static_cast<short>(inv_a));
    __m256i zero = _mm256_setzero_si256();

    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m256i v_src_32 = _mm256_set1_epi32(static_cast<int>(src_raw));
    __m256i v_src_16_lo = _mm256_unpacklo_epi8(v_src_32, zero);
    __m256i v_src_16_hi = _mm256_unpackhi_epi8(v_src_32, zero);

    PremultipliedColorU8* row = dst;
    for (uint32_t y = 0; y < h; ++y, row += stride) {
        size_t i = 0;
        for (; i + 32 <= w; i += 32) {
            __m256i d0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
            __m256i d1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i + 8));
            __m256i d2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i + 16));
            __m256i d3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i + 24));

            __m256i d0_lo = _mm256_unpacklo_epi8(d0, zero);
            __m256i d0_hi = _mm256_unpackhi_epi8(d0, zero);
            __m256i d1_lo = _mm256_unpacklo_epi8(d1, zero);
            __m256i d1_hi = _mm256_unpackhi_epi8(d1, zero);
            __m256i d2_lo = _mm256_unpacklo_epi8(d2, zero);
            __m256i d2_hi = _mm256_unpackhi_epi8(d2, zero);
            __m256i d3_lo = _mm256_unpacklo_epi8(d3, zero);
            __m256i d3_hi = _mm256_unpackhi_epi8(d3, zero);

            __m256i m0_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d0_lo, v_inv_a), 8);
            __m256i m0_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d0_hi, v_inv_a), 8);
            __m256i m1_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d1_lo, v_inv_a), 8);
            __m256i m1_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d1_hi, v_inv_a), 8);
            __m256i m2_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d2_lo, v_inv_a), 8);
            __m256i m2_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d2_hi, v_inv_a), 8);
            __m256i m3_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d3_lo, v_inv_a), 8);
            __m256i m3_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d3_hi, v_inv_a), 8);

            __m256i out0 = _mm256_packus_epi16(_mm256_add_epi16(v_src_16_lo, m0_lo), _mm256_add_epi16(v_src_16_hi, m0_hi));
            __m256i out1 = _mm256_packus_epi16(_mm256_add_epi16(v_src_16_lo, m1_lo), _mm256_add_epi16(v_src_16_hi, m1_hi));
            __m256i out2 = _mm256_packus_epi16(_mm256_add_epi16(v_src_16_lo, m2_lo), _mm256_add_epi16(v_src_16_hi, m2_hi));
            __m256i out3 = _mm256_packus_epi16(_mm256_add_epi16(v_src_16_lo, m3_lo), _mm256_add_epi16(v_src_16_hi, m3_hi));

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + i), out0);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + i + 8), out1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + i + 16), out2);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + i + 24), out3);
        }
        for (; i + 8 <= w; i += 8) {
            __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
            __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
            __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

            __m256i d_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, v_inv_a), 8);
            __m256i d_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, v_inv_a), 8);

            __m256i out_lo = _mm256_add_epi16(v_src_16_lo, d_mul_lo);
            __m256i out_hi = _mm256_add_epi16(v_src_16_hi, d_mul_hi);

            __m256i res = _mm256_packus_epi16(out_lo, out_hi);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + i), res);
        }
        for (; i < w; ++i) {
            uint32_t d = *reinterpret_cast<const uint32_t*>(row + i);
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            *reinterpret_cast<uint32_t*>(row + i) = src_raw + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        }
    }
#elif defined(NISABA_HAS_SSE2)
    __m128i v_inv_a = _mm_set1_epi16(static_cast<short>(inv_a));
    __m128i zero = _mm_setzero_si128();

    uint32_t src_raw = *reinterpret_cast<const uint32_t*>(&src);
    __m128i v_src_32 = _mm_set1_epi32(static_cast<int>(src_raw));
    __m128i v_src_16_lo = _mm_unpacklo_epi8(v_src_32, zero);
    __m128i v_src_16_hi = _mm_unpackhi_epi8(v_src_32, zero);

    for (uint32_t y = 0; y < h; ++y) {
        PremultipliedColorU8* row = dst + y * stride;
        size_t i = 0;
        for (; i + 4 <= w; i += 4) {
            __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
            __m128i d_lo = _mm_unpacklo_epi8(d, zero);
            __m128i d_hi = _mm_unpackhi_epi8(d, zero);

            __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, v_inv_a), 8);
            __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, v_inv_a), 8);

            __m128i out_lo = _mm_add_epi16(v_src_16_lo, d_mul_lo);
            __m128i out_hi = _mm_add_epi16(v_src_16_hi, d_mul_hi);

            __m128i res = _mm_packus_epi16(out_lo, out_hi);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + i), res);
        }
        for (; i < w; ++i) {
            PremultipliedColorU8 d = row[i];
            uint8_t r = static_cast<uint8_t>(src.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
            uint8_t g = static_cast<uint8_t>(src.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
            uint8_t b = static_cast<uint8_t>(src.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
            uint8_t a = static_cast<uint8_t>(src.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
            row[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
        }
    }
#else
    for (uint32_t y = 0; y < h; ++y) {
        PremultipliedColorU8* row = dst + y * stride;
        for (size_t i = 0; i < w; ++i) {
            PremultipliedColorU8 d = row[i];
            uint8_t r = static_cast<uint8_t>(src.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
            uint8_t g = static_cast<uint8_t>(src.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
            uint8_t b = static_cast<uint8_t>(src.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
            uint8_t a = static_cast<uint8_t>(src.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
            row[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
        }
    }
#endif
}

inline void blend_solid_dest_in(PremultipliedColorU8* dst, PremultipliedColorU8 src, size_t count) noexcept {
    if (count == 0) return;
    uint8_t sa = src.alpha();
    if (sa == 255) return;
    if (sa == 0) {
        blend_solid_clear(dst, count);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        dst[i] = blend_source_in_pixel(src, dst[i]);
    }
}

inline void blend_solid_span_by_mode(PremultipliedColorU8* dst, PremultipliedColorU8 src, BlendMode mode, size_t count) noexcept {
    if (count == 0) return;
    switch (mode) {
        case BlendMode::Clear:
            blend_solid_clear(dst, count);
            break;
        case BlendMode::Source:
            fill_solid_span(dst, src, count);
            break;
        case BlendMode::Destination:
            break;
        case BlendMode::SourceOver:
            blend_solid_source_over(dst, src, count);
            break;
        case BlendMode::DestinationOver:
            blend_solid_dest_over(dst, src, count);
            break;
        case BlendMode::SourceIn:
            blend_solid_source_in(dst, src, count);
            break;
        case BlendMode::DestinationIn:
            blend_solid_dest_in(dst, src, count);
            break;
        case BlendMode::SourceOut:
            blend_solid_source_out(dst, src, count);
            break;
        case BlendMode::DestinationOut:
            blend_solid_dest_out(dst, src, count);
            break;
        case BlendMode::SourceAtop:
            blend_solid_source_atop(dst, src, count);
            break;
        case BlendMode::DestinationAtop:
            blend_solid_dest_atop(dst, src, count);
            break;
        case BlendMode::Xor:
            blend_solid_xor(dst, src, count);
            break;
        case BlendMode::Plus:
            blend_solid_plus(dst, src, count);
            break;
        case BlendMode::Modulate:
            blend_solid_modulate(dst, src, count);
            break;
        case BlendMode::Multiply:
            blend_solid_multiply(dst, src, count);
            break;
        case BlendMode::Screen:
            blend_solid_screen(dst, src, count);
            break;
        case BlendMode::Overlay:
            blend_solid_overlay(dst, src, count);
            break;
        case BlendMode::Darken:
            blend_solid_darken(dst, src, count);
            break;
        case BlendMode::Lighten:
            blend_solid_lighten(dst, src, count);
            break;
        case BlendMode::ColorDodge:
            blend_solid_color_dodge(dst, src, count);
            break;
        case BlendMode::ColorBurn:
            blend_solid_color_burn(dst, src, count);
            break;
        case BlendMode::HardLight:
            blend_solid_hard_light(dst, src, count);
            break;
        case BlendMode::SoftLight:
            blend_solid_soft_light(dst, src, count);
            break;
        case BlendMode::Difference:
            blend_solid_difference(dst, src, count);
            break;
        case BlendMode::Exclusion:
            blend_solid_exclusion(dst, src, count);
            break;
        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity:
            blend_solid_hsl(dst, src, mode, count);
            break;
        default:
            break;
    }
}

} // namespace nisaba::simd

