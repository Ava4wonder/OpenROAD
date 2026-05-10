// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.5.c — abstract source of per-net congestion + timing deltas
// for the eval scoring path. Pluggable so that:
//   (a) tests can attach a synthetic stub
//   (b) production can attach a real provider backed by FlexGR
//       congestion estimates + an STA-style criticality view
//       (deferred — no concrete real impl in V2.5.c)
//
// The provider is a query: given a candidate AddWire / AddVia /
// other Delta and the geometry view it was proposed against,
// return (delta_congestion, delta_timing) — the local SCORE
// contribution from these two domains. PhysicalState::eval calls
// the provider once per candidate (when one is attached) and the
// returned values land in Score::delta_congestion +
// Score::delta_timing, which then flow into aggregate via
// CostWeights::congestion + CostWeights::timing.
//
// SCOPE — interface only. The V2.5.c minimum ships an abstract
// base + a TestStubProvider for unit tests; a real provider is
// V2.5.c.real / V2.6+. The seam exists so V2.6 drive-mode can
// score with real congestion/timing without a further refactor.

#pragma once

#include "EvalOutcome.h"
#include "Footprint.h"

namespace drt::redesign::overlay {
class GeometryView;
}

namespace drt::redesign {

// Per-candidate result. Both terms are in score-space units (no
// pre-weighting); CostWeights are applied later in
// PhysicalState::eval's aggregate computation.
struct CongestionTimingDelta
{
  double delta_congestion = 0.0;
  double delta_timing = 0.0;
};

class CongestionTimingProvider
{
 public:
  virtual ~CongestionTimingProvider() = default;

  // Returns the (delta_congestion, delta_timing) contribution for
  // committing `delta` against `base`. May query `base` for
  // contextual signals (route shapes around the candidate, guides,
  // etc.). MUST be re-entrant — V2.5.c's call site is inside
  // eval(), which V2.4.f calls from inside an OMP-parallel region.
  virtual CongestionTimingDelta Query(
      const overlay::GeometryView& base,
      const ProposedDelta& delta) const = 0;
};

}  // namespace drt::redesign
