// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.d.a — pure-CPU multi-candidate generation by perturbing
// upstream's captured route in K small ways.
//
// Why perturbation: real multi-candidate ("re-run upstream's maze
// search K times with K different cost-bias vectors") is multi-week
// work because the maze search is deeply integrated with upstream's
// FlexGridGraph cost machinery. Perturbation is a much cheaper
// approximation that's enough for the runtime-comparison report:
// take upstream's single captured route as the baseline (variant 0
// = identity, always preserves upstream's behaviour), then apply
// small bbox shifts in (x, y) to produce K-1 alternatives. Many
// alternatives will be illegal under V2.5.b's CpuDrcOracle and get
// filtered out by SelectMis; the surviving best may differ from
// upstream's, demonstrating multi-candidate selection at the
// architectural level.
//
// V2.6.d.a SCOPE — pure conversion. No FlexDR_maze.cpp wiring;
// V2.6.d.b adds the call site. RoutePerturbation has no dependency
// on drNet / drConnFig (the caller has already extracted the
// CapturedConnFig vector from upstream).

#pragma once

#include <cstdint>
#include <vector>

#include "MazeSearchProposer.h"  // CapturedConnFig

namespace drt::redesign::overlay {

struct PerturbationParams
{
  // DBU shifts applied uniformly to every CapturedConnFig in the
  // route. Shifts are signed; (0, 0) is identity. Future
  // extensions (layer swap, patch drop, via choice) would add
  // fields here without breaking ApplyPerturbation's signature.
  std::int32_t shift_x_dbu = 0;
  std::int32_t shift_y_dbu = 0;
};

// V2.6.d.a — apply a shift to every shape in `original` and return
// the perturbed copy. Identity ((0, 0)) returns a faithful copy.
// PathSeg / PatchWire bboxes shift by (shift_x, shift_y); Via
// origin also shifts. Layers are unchanged.
std::vector<CapturedConnFig> ApplyPerturbation(
    const std::vector<CapturedConnFig>& original,
    const PerturbationParams& params);

// V2.6.d.a — generate K alternative routes for one net, indexed
// 0..K-1:
//   0:        identity (no shift) — guarantees upstream's route is
//             always in the candidate set
//   1..K-1:   small (x, y) shifts on a deterministic schedule so
//             SelectMis sees something other than upstream
//
// Defaults pick shifts ≈ ±track_pitch (track_pitch_hint is the
// caller's per-design hint; DBU; defaults to 100 DBU which is the
// asap7 track pitch order of magnitude). The schedule is:
//   1: shift_x=+T,  shift_y=0
//   2: shift_x=-T,  shift_y=0
//   3: shift_x=0,   shift_y=+T
//   4: shift_x=0,   shift_y=-T
//   5: shift_x=+T,  shift_y=+T
//   ... etc., diagonals of growing radius.
//
// SCOPE — synthetic shifts. V2.8+ replaces with real
// alternative-route generation (GPU multi-trajectory or
// upstream-maze re-run with K cost-bias vectors).
std::vector<std::vector<CapturedConnFig>> GenerateKPerturbations(
    const std::vector<CapturedConnFig>& original,
    std::size_t k,
    std::int32_t track_pitch_hint_dbu = 100);

}  // namespace drt::redesign::overlay
