// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.1 — SIMD geometric primitives.
// AVX2 + scalar fallback, runtime-selected via __builtin_cpu_supports.

#include "Geom.h"

#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace drt::redesign::simd {

namespace {

bool DetectAvx2()
{
#if defined(__x86_64__) || defined(_M_X64)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

const bool kUseAvx2 = DetectAvx2();

void AnyOverlapScalar(const std::int32_t* lx1,
                      const std::int32_t* ly1,
                      const std::int32_t* lx2,
                      const std::int32_t* ly2,
                      std::size_t lc,
                      const std::int32_t* rx1,
                      const std::int32_t* ry1,
                      const std::int32_t* rx2,
                      const std::int32_t* ry2,
                      std::size_t rc,
                      std::uint8_t* out)
{
  for (std::size_t i = 0; i < lc; ++i) {
    std::uint8_t any = 0;
    for (std::size_t j = 0; j < rc; ++j) {
      const bool ovl = !(lx2[i] < rx1[j] || rx2[j] < lx1[i]
                         || ly2[i] < ry1[j] || ry2[j] < ly1[i]);
      if (ovl) {
        any = 1;
        break;
      }
    }
    out[i] = any;
  }
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2"))) void AnyOverlapAvx2(
    const std::int32_t* lx1,
    const std::int32_t* ly1,
    const std::int32_t* lx2,
    const std::int32_t* ly2,
    std::size_t lc,
    const std::int32_t* rx1,
    const std::int32_t* ry1,
    const std::int32_t* rx2,
    const std::int32_t* ry2,
    std::size_t rc,
    std::uint8_t* out)
{
  const std::size_t lvec = lc & ~static_cast<std::size_t>(7);  // round down to 8
  const __m256i ones = _mm256_set1_epi32(-1);
  for (std::size_t i = 0; i < lvec; i += 8) {
    const __m256i v_lx1
        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lx1 + i));
    const __m256i v_ly1
        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ly1 + i));
    const __m256i v_lx2
        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lx2 + i));
    const __m256i v_ly2
        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ly2 + i));
    __m256i any = _mm256_setzero_si256();
    for (std::size_t j = 0; j < rc; ++j) {
      const __m256i br_x1 = _mm256_set1_epi32(rx1[j]);
      const __m256i br_y1 = _mm256_set1_epi32(ry1[j]);
      const __m256i br_x2 = _mm256_set1_epi32(rx2[j]);
      const __m256i br_y2 = _mm256_set1_epi32(ry2[j]);
      // overlap == NOT(lx2<rx1 || rx2<lx1 || ly2<ry1 || ry2<ly1)
      // (a < b) lanes evaluate to all-ones via cmpgt(b, a)
      const __m256i c1 = _mm256_cmpgt_epi32(br_x1, v_lx2);
      const __m256i c2 = _mm256_cmpgt_epi32(v_lx1, br_x2);
      const __m256i c3 = _mm256_cmpgt_epi32(br_y1, v_ly2);
      const __m256i c4 = _mm256_cmpgt_epi32(v_ly1, br_y2);
      const __m256i nonovl
          = _mm256_or_si256(_mm256_or_si256(c1, c2), _mm256_or_si256(c3, c4));
      const __m256i ovl = _mm256_xor_si256(nonovl, ones);
      any = _mm256_or_si256(any, ovl);
    }
    alignas(32) std::int32_t lanes[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), any);
    for (int k = 0; k < 8; ++k) {
      out[i + k] = lanes[k] != 0 ? 1 : 0;
    }
  }
  if (lvec < lc) {
    AnyOverlapScalar(lx1 + lvec,
                     ly1 + lvec,
                     lx2 + lvec,
                     ly2 + lvec,
                     lc - lvec,
                     rx1,
                     ry1,
                     rx2,
                     ry2,
                     rc,
                     out + lvec);
  }
}
#endif

}  // namespace

bool UsingAvx2()
{
  return kUseAvx2;
}

void AnyOverlap(const std::int32_t* lhs_x1,
                const std::int32_t* lhs_y1,
                const std::int32_t* lhs_x2,
                const std::int32_t* lhs_y2,
                std::size_t lhs_count,
                const std::int32_t* rhs_x1,
                const std::int32_t* rhs_y1,
                const std::int32_t* rhs_x2,
                const std::int32_t* rhs_y2,
                std::size_t rhs_count,
                std::uint8_t* output)
{
  if (lhs_count == 0) {
    return;
  }
  if (rhs_count == 0) {
    std::memset(output, 0, lhs_count);
    return;
  }
#if defined(__x86_64__) || defined(_M_X64)
  if (kUseAvx2) {
    AnyOverlapAvx2(lhs_x1,
                   lhs_y1,
                   lhs_x2,
                   lhs_y2,
                   lhs_count,
                   rhs_x1,
                   rhs_y1,
                   rhs_x2,
                   rhs_y2,
                   rhs_count,
                   output);
    return;
  }
#endif
  AnyOverlapScalar(lhs_x1,
                   lhs_y1,
                   lhs_x2,
                   lhs_y2,
                   lhs_count,
                   rhs_x1,
                   rhs_y1,
                   rhs_x2,
                   rhs_y2,
                   rhs_count,
                   output);
}

bool ExpandedContains(std::int32_t x1,
                      std::int32_t y1,
                      std::int32_t x2,
                      std::int32_t y2,
                      std::int32_t halo,
                      std::int32_t px,
                      std::int32_t py)
{
  return px >= x1 - halo && px <= x2 + halo && py >= y1 - halo
         && py <= y2 + halo;
}

void SameLayerMask(const std::int16_t* lhs_layers,
                   std::size_t count,
                   std::int16_t rhs_layer,
                   std::uint8_t* output)
{
  for (std::size_t i = 0; i < count; ++i) {
    output[i] = lhs_layers[i] == rhs_layer ? 1 : 0;
  }
}

void SameNetMask(const std::uint64_t* lhs_net_ids,
                 std::size_t count,
                 std::uint64_t rhs_net_id,
                 std::uint8_t* output)
{
  for (std::size_t i = 0; i < count; ++i) {
    output[i] = lhs_net_ids[i] == rhs_net_id ? 1 : 0;
  }
}

}  // namespace drt::redesign::simd
