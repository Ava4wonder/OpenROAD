// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding. See drt_redesign_plan.md §7B.
// PhysicalState::eval(snapshot, delta) returns one of these.
// All fields are in proposal-local units; ConflictPolicy applies the
// phase-dependent weighting (early/middle/late, per plan §7D).

#pragma once

#include <cstdint>

namespace drt::redesign {

struct EvaluationResult
{
  double drc_cost = 0.0;
  double timing_cost = 0.0;
  double wirelength_cost = 0.0;
  double via_cost = 0.0;
  double congestion_cost = 0.0;
  double pin_access_cost = 0.0;
  double extractability_cost = 0.0;

  bool legal = true;

  uint64_t marker_count_after = 0;
};

}  // namespace drt::redesign
