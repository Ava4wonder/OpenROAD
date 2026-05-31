// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 1 — POD types for the AdaptiveMarkerModel framework.
//
// All structs are passive data. The model (AdaptiveMarkerModel) lives at
// FlexDR scope; workers consume AdaptiveWorkerPolicy as a read-only snapshot
// and emit AdaptiveWorkerStats after they finish. FlexDR merges stats into
// the model serially after each OMP-parallel worker batch — no atomics, no
// shared mutation from worker threads.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "frBaseTypes.h"
#include "odb/geom.h"

namespace drt {

class frNet;

// Coarse rule taxonomy for marker classification. Keep small + stable —
// per-rule arrays in AdaptiveWorkerPolicy are indexed by this enum cast to
// int. Order matches the rule_counts std::array<int, 16> in
// AdaptiveWorkerStats so adding entries requires bumping that size.
enum class AdaptiveRuleClass : std::uint8_t
{
  Short = 0,
  CutShort = 1,
  MetalSpacing = 2,
  CutSpacing = 3,
  Eol = 4,
  MinArea = 5,
  NsMetal = 6,
  MinStep = 7,  // Patch 2.1 — added to absorb min-step / minimum-cut
                // rules that dominated the "Other" bucket on ISPD-18.
  Other = 8,
};
inline constexpr std::size_t kNumAdaptiveRuleClasses = 9;

// Tile index into the model's flat heat array. (-1, -1) is the sentinel
// for "outside all tiles" used by tile-overlap iteration.
struct AdaptiveTileIdx
{
  int x = 0;
  int y = 0;
};

// One marker observation as extracted by AdaptiveMarkerUtils. Conservative
// in Patch 1: nets vector starts populated only with frcNet owners; later
// patches add inst-term / pin-fig / block-object owners.
struct AdaptiveMarkerObs
{
  AdaptiveRuleClass rule = AdaptiveRuleClass::Other;
  frLayerNum layer = 0;
  odb::Rect bbox;
  bool has_dir = false;
  bool is_horizontal = false;

  std::vector<frNet*> nets;
  int weight = 1;
};

// Per-worker read-only snapshot. Constructed by FlexDR before each worker
// batch and passed to FlexDRWorker::setAdaptivePolicy. All fields default
// to the no-op identity (all multipliers = 1.0, flags false) so a worker
// that has not been given a policy behaves identically to the disabled-
// model case.
struct AdaptiveWorkerPolicy
{
  bool enabled = false;

  float drc_cost_mul = 1.0f;
  float marker_cost_mul = 1.0f;
  float fixed_shape_cost_mul = 1.0f;
  // marker_decay_override < 0 means "do not override"; the worker keeps the
  // upstream-scheduled decay value.
  float marker_decay_override = -1.0f;

  bool relax_guide = false;
  float guide_cost_mul = 1.0f;

  // Activated in Patches 5/6. Reserved here so future commits don't churn
  // the struct layout that workers consume.
  bool use_rule_aware_marker_increment = false;
  bool use_layer_aware_cost = false;

  // P4.A — diagnostic-only: the BBH-tier multiplier that was combined
  // (via max) with the H-tier drc_cost_mul to produce the final
  // drc_cost_mul. 1.0 = no BBH bump (worker not in a hot boundary
  // band, or BBH disabled). The integration site uses
  // drc_cost_mul only; bbh_mul is kept for CSV diagnostics.
  float bbh_mul = 1.0f;
};

// Per-worker output. Returned by FlexDRWorker after main(). FlexDR ingests
// stats serially after each OMP-parallel batch.
struct AdaptiveWorkerStats
{
  odb::Rect route_box;
  odb::Rect drc_box;
  int iter = 0;

  int num_markers = 0;
  int weighted_marker_score = 0;
  bool congested = false;

  // Indexed by AdaptiveRuleClass cast to int. Size = 16 leaves headroom
  // beyond kNumAdaptiveRuleClasses for future taxonomy growth without
  // breaking serialised stats files.
  std::array<int, 16> rule_counts{};
};

}  // namespace drt
