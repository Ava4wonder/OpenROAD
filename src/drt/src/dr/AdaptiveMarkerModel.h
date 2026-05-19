// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 1 — AdaptiveMarkerModel class declaration.
//
// Persistent, in-memory, FlexDR-scope model that consumes frMarker
// objects and worker outcomes and emits per-worker AdaptiveWorkerPolicy
// snapshots. Patch 1 declares the full interface but only implements a
// minimal disabled-model skeleton; subsequent patches fill in observation,
// heat accumulation, hotspot detection, and policy emission.
//
// Thread-safety contract (enforced by integration, not by this class):
//   - The FlexDR thread is the SOLE mutator of AdaptiveMarkerModel.
//   - Worker threads receive read-only AdaptiveWorkerPolicy snapshots
//     produced by getWorkerPolicy(). They MUST NOT call any non-const
//     method on the model.
//   - After each OMP-parallel worker batch, the FlexDR thread serially
//     ingests AdaptiveWorkerStats via observeWorkerStats().

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dr/AdaptiveMarkerTypes.h"
#include "frBaseTypes.h"

namespace utl {
class Logger;
}

namespace drt {

class frDesign;
class frMarker;
class frNet;

class AdaptiveMarkerModel
{
 public:
  struct Options
  {
    bool enabled = false;

    // Tile size for the heat grid. 0 = derive from gcell pitch.
    int tile_size_dbu = 0;

    float heat_decay = 0.85f;
    int heat_max = 65535;

    // Hotspot detection thresholds — weighted heat per tile.
    int hotspot_threshold = 200;
    int severe_hotspot_threshold = 800;

    bool log_csv = false;
    std::string log_path;
  };

  AdaptiveMarkerModel(const Options& options,
                      frDesign* design,
                      utl::Logger* logger);
  ~AdaptiveMarkerModel();

  // Non-copyable, non-movable — instance is owned by FlexDR and never
  // duplicated. unique_ptr handles lifetime; copies would break the
  // single-mutator thread-safety contract.
  AdaptiveMarkerModel(const AdaptiveMarkerModel&) = delete;
  AdaptiveMarkerModel& operator=(const AdaptiveMarkerModel&) = delete;
  AdaptiveMarkerModel(AdaptiveMarkerModel&&) = delete;
  AdaptiveMarkerModel& operator=(AdaptiveMarkerModel&&) = delete;

  // Per-outer-iter lifecycle. FlexDR calls beginOuterIter at the start
  // (decays heat) and endOuterIter at the end (updates hotspot list,
  // writes CSV row).
  void beginOuterIter(int iter);
  void endOuterIter();

  // Mutation paths — FlexDR thread ONLY.
  void observeGlobalMarkers(
      const std::vector<std::unique_ptr<frMarker>>& markers);
  void observeGlobalMarkers(const std::vector<frMarker>& markers);
  void observeWorkerStats(const AdaptiveWorkerStats& stats);

  // Read paths — safe to call from any thread provided no concurrent
  // mutation (which is guaranteed by the integration contract: workers
  // call this BEFORE main() returns, so the FlexDR thread is blocked
  // inside the OMP parallel for).
  AdaptiveWorkerPolicy getWorkerPolicy(const odb::Rect& route_box,
                                       const odb::Rect& drc_box,
                                       int iter,
                                       frUInt4 base_drc_cost,
                                       frUInt4 base_marker_cost,
                                       frUInt4 base_fixed_shape_cost,
                                       float base_decay) const;

  int getMarkerIncrement(const frMarker& marker,
                         bool is_via,
                         frLayerNum layer) const;

  int getNetPriority(frNet* net) const;

  bool shouldIncreaseClipSize(const odb::Rect& region) const;

  // Introspection / testing.
  const Options& getOptions() const { return options_; }
  int getCurrentIter() const { return iter_; }

 private:
  void decayHeat();
  void addMarkerObservation(const AdaptiveMarkerObs& obs);
  void updateHotspots();

  Options options_;
  frDesign* design_ = nullptr;
  utl::Logger* logger_ = nullptr;

  int iter_ = 0;

  // Flat heat arrays: indexed [rule][layer][tile_y][tile_x]. Sized
  // lazily on first observation once tile dimensions are known.
  std::vector<std::uint16_t> rule_layer_heat_;
  std::vector<std::uint16_t> layer_heat_;
  std::vector<std::uint16_t> via_heat_;

  std::unordered_map<frNet*, int> net_score_;

  // Deferred — populated by Patch 12. Key = canonical pair<frNet*, frNet*>
  // packed into uint64_t.
  std::unordered_map<std::uint64_t, int> conflict_score_;

  std::vector<odb::Rect> hotspots_;
};

}  // namespace drt
