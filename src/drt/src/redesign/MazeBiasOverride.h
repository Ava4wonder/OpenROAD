// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.f.1 — RAII helper for save/restore of FlexGridGraph cost
// weights (ggDRCCost, ggMarkerCost, ggFixedShapeCost).
//
// Foundation for V2.6.f's K-bias multi-candidate search: the
// driver constructs one MazeBiasOverride per candidate run with
// the desired bias, calls upstream's routeNet (or
// gridGraph_.search) inside that scope, captures the result, and
// the override falls out of scope to restore the original
// weights before the next candidate. K candidates → K
// MazeBiasOverride scopes.
//
// Templated on the gridgraph type so unit tests can use a mock
// without dragging in the full FlexGridGraph implementation. The
// gridgraph need only expose four methods:
//   frUInt4 getDRCCost() const;
//   frUInt4 getMarkerCost() const;
//   frUInt4 getFixedShapeCost() const;
//   void setCost(frUInt4, frUInt4, frUInt4);
// FlexGridGraph satisfies this surface (V2.6.f.1 added the three
// getters).
//
// Discipline: non-copyable, non-movable. Each instance owns one
// save/restore round trip. Nested scopes ARE supported (each
// outer scope sees the value the inner scope restored).

#pragma once

#include <cstdint>

namespace drt::redesign {

// Plain alias to keep V2's namespace independent of upstream's
// frUInt4 typedef (which is uint32_t in upstream global.h).
using GridGraphCost = std::uint32_t;

template <typename GridGraph>
class MazeBiasOverride
{
 public:
  MazeBiasOverride(GridGraph& gg,
                   GridGraphCost drc_bias,
                   GridGraphCost marker_bias,
                   GridGraphCost fixed_shape_bias)
      : gg_(gg),
        saved_drc_(static_cast<GridGraphCost>(gg.getDRCCost())),
        saved_marker_(static_cast<GridGraphCost>(gg.getMarkerCost())),
        saved_fixed_shape_(
            static_cast<GridGraphCost>(gg.getFixedShapeCost()))
  {
    gg.setCost(drc_bias, marker_bias, fixed_shape_bias);
  }

  ~MazeBiasOverride()
  {
    // Best-effort restore — destructor must not throw. setCost is
    // a noexcept-trivial three-field assignment on FlexGridGraph
    // so this is safe.
    gg_.setCost(saved_drc_, saved_marker_, saved_fixed_shape_);
  }

  MazeBiasOverride(const MazeBiasOverride&) = delete;
  MazeBiasOverride& operator=(const MazeBiasOverride&) = delete;
  MazeBiasOverride(MazeBiasOverride&&) = delete;
  MazeBiasOverride& operator=(MazeBiasOverride&&) = delete;

  // Test-only / introspection — what was saved.
  GridGraphCost saved_drc() const { return saved_drc_; }
  GridGraphCost saved_marker() const { return saved_marker_; }
  GridGraphCost saved_fixed_shape() const { return saved_fixed_shape_; }

 private:
  GridGraph& gg_;
  GridGraphCost saved_drc_;
  GridGraphCost saved_marker_;
  GridGraphCost saved_fixed_shape_;
};

}  // namespace drt::redesign
