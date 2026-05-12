// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Lean V2.6.h mode — runtime gate to skip V2.1.e through V2.4.f
// shadow paths (observation-only diagnostic code that adds wall +
// may perturb scheduling) while keeping V2.6.f.5.c K-bias driver +
// V2.6.h inner-loop coordinate descent active.
//
// Gated by env: OPENROAD_DRT_REDESIGN_LEAN=1. Read once at first
// call, cached for the process lifetime.
//
// When LEAN mode is on:
//   - V2.1.e Snapshot shadow comparisons are skipped (the costly
//     QueryRouteShapes / Blockages / Guides / Markers overlay path
//     is short-circuited before any FNV hashing or projection).
//   - V2.2.f synthetic single-candidate dry run is skipped.
//   - V2.3.b/c synthetic K=4 perturbation generation + scoring is
//     skipped (the FlexDR_maze.cpp top-of-routeNet V2.3.c block).
//   - V2.4.f cross-worker MIS resolve is skipped (FlexDR.cpp
//     endWorkersBatch block).
//
// What's PRESERVED in lean mode:
//   - V2.6.f.5.c K-bias driver (re-runs upstream gridGraph_.search()
//     with K=2 cost-bias variants on admit-eligible nets)
//   - V2.6.f.8 argmin-DRV scoring
//   - V2.6.f.9 iter-aware bias amplifier
//   - V2.6.f.10 per-net cross-net conflict check
//   - V2.6.h inner-loop coordinate descent over marker-touching nets

#pragma once

#include <cstdlib>

namespace drt::redesign {

// Inline so the env-var check folds away when LEAN is off (the
// caller-side branch becomes a single load of a static atomic that
// the compiler can hoist). One-time init per process.
inline bool DrtRedesignLeanMode()
{
  static const bool enabled = [] {
    const char* v = std::getenv("OPENROAD_DRT_REDESIGN_LEAN");
    return v != nullptr && v[0] == '1';
  }();
  return enabled;
}

}  // namespace drt::redesign
