// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.1 — Standalone unit tests + microbench for redesign/simd/Geom.
// Built only when ENABLE_DRT_REDESIGN=ON. Run via `./redesign_simd_test`.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "redesign/simd/Geom.h"

namespace dr = drt::redesign::simd;

namespace {

void RefAnyOverlap(const std::vector<std::int32_t>& lx1,
                   const std::vector<std::int32_t>& ly1,
                   const std::vector<std::int32_t>& lx2,
                   const std::vector<std::int32_t>& ly2,
                   const std::vector<std::int32_t>& rx1,
                   const std::vector<std::int32_t>& ry1,
                   const std::vector<std::int32_t>& rx2,
                   const std::vector<std::int32_t>& ry2,
                   std::vector<std::uint8_t>& out)
{
  const std::size_t lc = lx1.size();
  const std::size_t rc = rx1.size();
  out.assign(lc, 0);
  for (std::size_t i = 0; i < lc; ++i) {
    for (std::size_t j = 0; j < rc; ++j) {
      const bool ovl = !(lx2[i] < rx1[j] || rx2[j] < lx1[i]
                         || ly2[i] < ry1[j] || ry2[j] < ly1[i]);
      if (ovl) {
        out[i] = 1;
        break;
      }
    }
  }
}

bool TestAnyOverlapCorrectness()
{
  std::mt19937 rng(0xCAFE);
  std::uniform_int_distribution<std::int32_t> dist(0, 1000);
  for (int trial = 0; trial < 200; ++trial) {
    const std::size_t lc = 1 + (rng() % 257);
    const std::size_t rc = 1 + (rng() % 65);
    std::vector<std::int32_t> lx1(lc), ly1(lc), lx2(lc), ly2(lc);
    std::vector<std::int32_t> rx1(rc), ry1(rc), rx2(rc), ry2(rc);
    for (std::size_t i = 0; i < lc; ++i) {
      lx1[i] = dist(rng);
      ly1[i] = dist(rng);
      lx2[i] = lx1[i] + 1 + (rng() % 50);
      ly2[i] = ly1[i] + 1 + (rng() % 50);
    }
    for (std::size_t j = 0; j < rc; ++j) {
      rx1[j] = dist(rng);
      ry1[j] = dist(rng);
      rx2[j] = rx1[j] + 1 + (rng() % 50);
      ry2[j] = ry1[j] + 1 + (rng() % 50);
    }
    std::vector<std::uint8_t> ref;
    std::vector<std::uint8_t> got(lc, 0);
    RefAnyOverlap(lx1, ly1, lx2, ly2, rx1, ry1, rx2, ry2, ref);
    dr::AnyOverlap(lx1.data(),
                   ly1.data(),
                   lx2.data(),
                   ly2.data(),
                   lc,
                   rx1.data(),
                   ry1.data(),
                   rx2.data(),
                   ry2.data(),
                   rc,
                   got.data());
    for (std::size_t i = 0; i < lc; ++i) {
      if (ref[i] != got[i]) {
        std::fprintf(
            stderr,
            "FAIL TestAnyOverlapCorrectness trial=%d i=%zu lc=%zu rc=%zu "
            "ref=%u got=%u\n",
            trial,
            i,
            lc,
            rc,
            ref[i],
            got[i]);
        return false;
      }
    }
  }
  return true;
}

bool TestBoundaryCases()
{
  // empty inputs (defensive)
  dr::AnyOverlap(
      nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr,
      nullptr, 0, nullptr);

  // Edge-touching rects must register as overlap (inclusive boundary).
  {
    std::vector<std::int32_t> lx1{0}, ly1{0}, lx2{10}, ly2{10};
    std::vector<std::int32_t> rx1{10}, ry1{0}, rx2{20}, ry2{10};
    std::vector<std::uint8_t> out(1, 0);
    dr::AnyOverlap(lx1.data(),
                   ly1.data(),
                   lx2.data(),
                   ly2.data(),
                   1,
                   rx1.data(),
                   ry1.data(),
                   rx2.data(),
                   ry2.data(),
                   1,
                   out.data());
    if (out[0] != 1) {
      std::fprintf(stderr,
                   "FAIL TestBoundaryCases edge-touching: expected 1 got %u\n",
                   out[0]);
      return false;
    }
  }

  // Disjoint by 1 dbu must register as no overlap.
  {
    std::vector<std::int32_t> lx1{0}, ly1{0}, lx2{10}, ly2{10};
    std::vector<std::int32_t> rx1{11}, ry1{0}, rx2{20}, ry2{10};
    std::vector<std::uint8_t> out(1, 0);
    dr::AnyOverlap(lx1.data(),
                   ly1.data(),
                   lx2.data(),
                   ly2.data(),
                   1,
                   rx1.data(),
                   ry1.data(),
                   rx2.data(),
                   ry2.data(),
                   1,
                   out.data());
    if (out[0] != 0) {
      std::fprintf(stderr,
                   "FAIL TestBoundaryCases 1-dbu gap: expected 0 got %u\n",
                   out[0]);
      return false;
    }
  }

  // Tail-handling: lhs count not multiple of 8 (verifies AVX2 tail path).
  {
    const std::size_t lc = 13;
    std::vector<std::int32_t> lx1(lc, 0), ly1(lc, 0), lx2(lc, 5), ly2(lc, 5);
    std::vector<std::int32_t> rx1{3}, ry1{3}, rx2{4}, ry2{4};
    std::vector<std::uint8_t> out(lc, 0);
    dr::AnyOverlap(lx1.data(),
                   ly1.data(),
                   lx2.data(),
                   ly2.data(),
                   lc,
                   rx1.data(),
                   ry1.data(),
                   rx2.data(),
                   ry2.data(),
                   1,
                   out.data());
    for (std::size_t i = 0; i < lc; ++i) {
      if (out[i] != 1) {
        std::fprintf(stderr,
                     "FAIL TestBoundaryCases tail i=%zu got %u\n",
                     i,
                     out[i]);
        return false;
      }
    }
  }

  // SameLayerMask + SameNetMask quick smoke
  {
    const std::int16_t layers[] = {1, 2, 3, 2, 4};
    std::uint8_t mask[5];
    dr::SameLayerMask(layers, 5, 2, mask);
    if (mask[0] != 0 || mask[1] != 1 || mask[2] != 0 || mask[3] != 1
        || mask[4] != 0) {
      std::fprintf(stderr, "FAIL SameLayerMask\n");
      return false;
    }
  }
  {
    const std::uint64_t nets[] = {100, 200, 100, 300};
    std::uint8_t mask[4];
    dr::SameNetMask(nets, 4, 100, mask);
    if (mask[0] != 1 || mask[1] != 0 || mask[2] != 1 || mask[3] != 0) {
      std::fprintf(stderr, "FAIL SameNetMask\n");
      return false;
    }
  }

  // ExpandedContains
  if (!dr::ExpandedContains(0, 0, 10, 10, 2, -1, -1)
      || dr::ExpandedContains(0, 0, 10, 10, 2, -3, 5)) {
    std::fprintf(stderr, "FAIL ExpandedContains\n");
    return false;
  }

  return true;
}

struct BenchResult
{
  double dispatched_pair_per_s;
  double scalar_pair_per_s;
};

BenchResult BenchAnyOverlap(std::size_t lc, std::size_t rc, int iters)
{
  std::mt19937 rng(0xBEEF);
  std::uniform_int_distribution<std::int32_t> dist(0, 100000);
  std::vector<std::int32_t> lx1(lc), ly1(lc), lx2(lc), ly2(lc);
  std::vector<std::int32_t> rx1(rc), ry1(rc), rx2(rc), ry2(rc);
  for (std::size_t i = 0; i < lc; ++i) {
    lx1[i] = dist(rng);
    ly1[i] = dist(rng);
    lx2[i] = lx1[i] + 1 + (rng() % 1000);
    ly2[i] = ly1[i] + 1 + (rng() % 1000);
  }
  for (std::size_t j = 0; j < rc; ++j) {
    rx1[j] = dist(rng);
    ry1[j] = dist(rng);
    rx2[j] = rx1[j] + 1 + (rng() % 1000);
    ry2[j] = ry1[j] + 1 + (rng() % 1000);
  }
  std::vector<std::uint8_t> out(lc);

  const auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    dr::AnyOverlap(lx1.data(), ly1.data(), lx2.data(), ly2.data(), lc,
                   rx1.data(), ry1.data(), rx2.data(), ry2.data(), rc,
                   out.data());
  }
  const auto t1 = std::chrono::steady_clock::now();

  for (int it = 0; it < iters; ++it) {
    dr::AnyOverlapScalar(lx1.data(), ly1.data(), lx2.data(), ly2.data(), lc,
                         rx1.data(), ry1.data(), rx2.data(), ry2.data(), rc,
                         out.data());
  }
  const auto t2 = std::chrono::steady_clock::now();

  const double sec_dispatched
      = std::chrono::duration<double>(t1 - t0).count();
  const double sec_scalar
      = std::chrono::duration<double>(t2 - t1).count();
  const double total_pairs = static_cast<double>(iters) * lc * rc;
  return {total_pairs / sec_dispatched, total_pairs / sec_scalar};
}

}  // namespace

int main()
{
  std::printf("AVX2 backend selected: %s\n", dr::UsingAvx2() ? "yes" : "no");

  if (!TestAnyOverlapCorrectness()) {
    return 1;
  }
  std::printf("PASS TestAnyOverlapCorrectness (200 randomized trials)\n");

  if (!TestBoundaryCases()) {
    return 1;
  }
  std::printf(
      "PASS TestBoundaryCases (edge-touch, 1-dbu gap, tail, masks, halo)\n");

  const BenchResult b = BenchAnyOverlap(1024, 1024, 20);
  const double ratio = b.dispatched_pair_per_s / b.scalar_pair_per_s;
  std::printf("Bench 1024x1024 dispatched (%s): %.2f Mpair/s\n",
              dr::UsingAvx2() ? "AVX2" : "scalar",
              b.dispatched_pair_per_s / 1e6);
  std::printf("Bench 1024x1024 forced-scalar:   %.2f Mpair/s\n",
              b.scalar_pair_per_s / 1e6);
  std::printf("Speedup ratio (dispatched / scalar): %.2fx\n", ratio);

  // Exit criterion from drt_redesign_execution_plan.md P2.1: >= 4x speedup
  // over scalar for AnyOverlap on 1024-rect batches when AVX2 is selected.
  if (dr::UsingAvx2() && ratio < 4.0) {
    std::fprintf(stderr,
                 "FAIL P2.1 exit criterion: AVX2 speedup %.2fx < 4.0x\n",
                 ratio);
    return 1;
  }
  std::printf("PASS P2.1 exit criterion (>=4x speedup over scalar)\n");
  return 0;
}
