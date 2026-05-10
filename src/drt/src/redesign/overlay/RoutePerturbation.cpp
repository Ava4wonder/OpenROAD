// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.d.a — RoutePerturbation implementation. See header.

#include "RoutePerturbation.h"

namespace drt::redesign::overlay {

std::vector<CapturedConnFig> ApplyPerturbation(
    const std::vector<CapturedConnFig>& original,
    const PerturbationParams& params)
{
  std::vector<CapturedConnFig> out;
  out.reserve(original.size());
  for (const auto& cap : original) {
    CapturedConnFig perturbed = cap;
    perturbed.bbox.ll.x += params.shift_x_dbu;
    perturbed.bbox.ll.y += params.shift_y_dbu;
    perturbed.bbox.ur.x += params.shift_x_dbu;
    perturbed.bbox.ur.y += params.shift_y_dbu;
    perturbed.via_origin.x += params.shift_x_dbu;
    perturbed.via_origin.y += params.shift_y_dbu;
    out.push_back(perturbed);
  }
  return out;
}

namespace {

// Deterministic schedule of (shift_x, shift_y) pairs for K-1
// non-identity perturbations. All shifts are multiples of T (the
// caller's track_pitch_hint).
PerturbationParams ShiftScheduleAt(std::size_t variant_index,
                                   std::int32_t T)
{
  // variant_index is 1..K-1 (0 is identity, handled elsewhere).
  // Schedule of 8 directions, then 2T radius, etc.
  static const int dx[] = {+1, -1,  0,  0, +1, -1, +1, -1};
  static const int dy[] = { 0,  0, +1, -1, +1, +1, -1, -1};
  const std::size_t cycle_len = sizeof(dx) / sizeof(dx[0]);
  const std::size_t cycle = (variant_index - 1) / cycle_len;
  const std::size_t step = (variant_index - 1) % cycle_len;
  const std::int32_t radius
      = T * static_cast<std::int32_t>(cycle + 1);

  PerturbationParams p;
  p.shift_x_dbu = dx[step] * radius;
  p.shift_y_dbu = dy[step] * radius;
  return p;
}

}  // namespace

std::vector<std::vector<CapturedConnFig>> GenerateKPerturbations(
    const std::vector<CapturedConnFig>& original,
    std::size_t k,
    std::int32_t track_pitch_hint_dbu)
{
  std::vector<std::vector<CapturedConnFig>> out;
  out.reserve(k);
  if (k == 0) {
    return out;
  }
  // Variant 0: identity — guarantees upstream's route is in the
  // candidate set so SelectMis never produces a worse outcome
  // than upstream baseline (worst case: all alternatives illegal,
  // identity wins).
  out.push_back(original);
  for (std::size_t i = 1; i < k; ++i) {
    const auto params = ShiftScheduleAt(i, track_pitch_hint_dbu);
    out.push_back(ApplyPerturbation(original, params));
  }
  return out;
}

}  // namespace drt::redesign::overlay
