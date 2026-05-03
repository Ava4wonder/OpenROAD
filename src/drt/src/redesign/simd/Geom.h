// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 2.1 (P2.1) — SIMD geometric primitives for the legality oracle.
// See drt_redesign_execution_plan.md.
//
// Operates on plain SoA arrays so callers can stage candidate batches
// in any layout (vector<int32_t>, raw arena allocator, GPU-staged
// pinned memory, etc.) without coupling to a richer C++ rect type.
//
// All boundary semantics are INCLUSIVE: rectangles that share an edge
// are considered overlapping. Matches OpenROAD's frBox convention.

#pragma once

#include <cstddef>
#include <cstdint>

namespace drt::redesign::simd {

// For each lhs[i], output[i] = 1 iff any rhs[j] overlaps it; 0 otherwise.
// `output` must point to at least `lhs_count` bytes.
// `lhs_count == 0` is a no-op; `rhs_count == 0` writes zeros.
void AnyOverlap(const int32_t* lhs_x1,
                const int32_t* lhs_y1,
                const int32_t* lhs_x2,
                const int32_t* lhs_y2,
                std::size_t lhs_count,
                const int32_t* rhs_x1,
                const int32_t* rhs_y1,
                const int32_t* rhs_x2,
                const int32_t* rhs_y2,
                std::size_t rhs_count,
                std::uint8_t* output);

// True iff (px, py) is within `halo` dbu of [x1, y1, x2, y2] (inclusive).
// Single-rect query — provided for API symmetry; trivially scalar.
bool ExpandedContains(std::int32_t x1,
                      std::int32_t y1,
                      std::int32_t x2,
                      std::int32_t y2,
                      std::int32_t halo,
                      std::int32_t px,
                      std::int32_t py);

// Per-element layer-equality mask. output[i] = 1 iff lhs_layers[i] == rhs_layer.
void SameLayerMask(const std::int16_t* lhs_layers,
                   std::size_t count,
                   std::int16_t rhs_layer,
                   std::uint8_t* output);

// Per-element net-id-equality mask.
void SameNetMask(const std::uint64_t* lhs_net_ids,
                 std::size_t count,
                 std::uint64_t rhs_net_id,
                 std::uint8_t* output);

// True iff the AVX2 backend is selected at runtime.
// (Always false on non-x86 builds.)
bool UsingAvx2();

// Forced-scalar variant of AnyOverlap. Same contract as AnyOverlap but
// bypasses runtime SIMD dispatch. Intended for benchmarking the SIMD/scalar
// ratio and for the correctness reference path; production code should call
// AnyOverlap() instead.
void AnyOverlapScalar(const std::int32_t* lhs_x1,
                      const std::int32_t* lhs_y1,
                      const std::int32_t* lhs_x2,
                      const std::int32_t* lhs_y2,
                      std::size_t lhs_count,
                      const std::int32_t* rhs_x1,
                      const std::int32_t* rhs_y1,
                      const std::int32_t* rhs_x2,
                      const std::int32_t* rhs_y2,
                      std::size_t rhs_count,
                      std::uint8_t* output);

}  // namespace drt::redesign::simd
