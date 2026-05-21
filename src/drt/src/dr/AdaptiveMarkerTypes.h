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
#include <string>
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

  // Patch 7 — per-worker init-marker scaling. When normalizer > 0,
  // the worker (inside main(), after init()) recomputes its own
  // drc_cost_mul by taking max with a per-worker mul derived from
  // its init_num_markers + the normalizer + the severe mul carried
  // along here. Read-only after policy snapshot is set.
  int per_worker_normalizer = 0;
  float per_worker_severe_mul = 1.0f;
  // marker_decay_override < 0 means "do not override"; the worker keeps the
  // upstream-scheduled decay value.
  float marker_decay_override = -1.0f;

  bool relax_guide = false;
  float guide_cost_mul = 1.0f;

  // Activated in Patches 5/6. Reserved here so future commits don't churn
  // the struct layout that workers consume.
  bool use_rule_aware_marker_increment = false;
  bool use_layer_aware_cost = false;
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

// Patch 3.5 — runtime profiling. Two CSV-row structs, populated only when
// OPENROAD_DRT_ADAPTIVE_PROFILE=1 is set. Both default-constructable so the
// non-profile build path costs nothing.

struct AdaptiveWorkerProfile
{
  int iter = 0;
  int batch_id = 0;
  int worker_id = 0;
  odb::Rect route_box;
  odb::Rect drc_box;

  // Encoded policy classification: "identity" / "hot" / "severe" / "off".
  // Determined from drc_cost_mul applied to the worker (severe > hot > 1.0).
  std::string policy_class;

  float drc_mul = 1.0f;
  float marker_mul = 1.0f;
  float fixed_mul = 1.0f;

  // -1 means "not captured in this version". Free to populate in a follow-up
  // patch without changing the CSV schema.
  int input_markers = -1;
  int output_markers = -1;
  int input_weighted_score = -1;
  int output_weighted_score = -1;
  int heat_score = -1;
  int current_marker_overlap = -1;
  int queue_pops = -1;
  int rerouted_nets = -1;
  int failed_routes = -1;

  bool congested = false;

  double worker_wall_ms = 0.0;
};

struct AdaptiveIterProfile
{
  int iter = 0;
  std::string variant_name;
  std::string flow_state;
  std::string ripup_mode;
  int clip_size = -1;

  int markers_start = -1;
  int markers_end = -1;
  int weighted_start = -1;
  int weighted_end = -1;

  // Encoded mul tier picked at iter start: "H" / "G" / "D" / "identity" /
  // "off" (model disabled).
  std::string policy_phase;
  float hot_drc = 1.0f;
  float severe_drc = 1.0f;
  float marker_mul = 1.0f;
  float fixed_mul = 1.0f;

  int num_worker_calls = 0;
  int num_active_workers = 0;
  int num_hot_workers = 0;
  int num_severe_workers = 0;
  int num_identity_workers = 0;

  double search_repair_wall_ms = 0.0;
  double worker_wall_sum_ms = 0.0;
  double worker_wall_max_ms = 0.0;
  double worker_wall_p50_ms = 0.0;
  double worker_wall_p95_ms = 0.0;
  double worker_wall_p99_ms = 0.0;
  double connectivity_wall_ms = 0.0;
  double writeback_wall_ms = -1.0;  // not separable yet
  double gc_wall_ms = -1.0;         // not separable yet

  int total_queue_pops = -1;
  int total_rerouted_nets = -1;
  int total_failed_routes = -1;
};

}  // namespace drt
