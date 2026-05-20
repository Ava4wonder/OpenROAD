// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4 — ConstraintField: per-worker spatial DRC-risk
// field. Worker-local (no global state, no cross-thread mutation).
// Default-OFF behind OPENROAD_DRT_CONSTRAINT_FIELD=1.
//
// Phase 4.1 (this commit): skeleton + empty fields + stats CSV. Query
// methods return 0 risk so no routing-cost change is introduced.
//
// Phase 4.2 (next): via/cut spacing splat populates the per-cut-layer
// risk tiles; getViaRisk() begins returning non-zero. FlexGridGraph via
// expansion picks it up via lambda_cut * F_cut.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dr/ConstraintFieldTypes.h"
#include "frBaseTypes.h"
#include "odb/geom.h"

namespace drt {

class FlexDRWorker;
class frBlock;
class frNet;
class drNet;

class ConstraintField
{
 public:
  ConstraintField();
  ~ConstraintField();

  ConstraintField(const ConstraintField&) = delete;
  ConstraintField& operator=(const ConstraintField&) = delete;
  ConstraintField(ConstraintField&&) = default;
  ConstraintField& operator=(ConstraintField&&) = default;

  // Sizing — called once by the builder before splatting. tile_pitch_dbu
  // sets the spatial resolution. num_layers covers ALL routing+cut
  // layers (Phase 4.2 only writes cut-layer tiles).
  void initialize(const odb::Rect& region,
                  int tile_pitch_dbu,
                  int num_layers);

  // Sparse splat — per Phase 4.2. Risk values saturate at UINT16_MAX
  // internally; the maze-cost path applies its own max_risk_cost cap.
  // owner_net is the drNet that contributed this splat. Tracking per-
  // owner contributions allows getViaRisk to subtract the routing
  // net's own contribution so the maze isn't penalised for laying its
  // own via row (same-net filtering).
  void addViaRisk(int cut_layer,
                  int tile_x,
                  int tile_y,
                  int delta,
                  drNet* owner_net);

  // Lookup with optional same-net filtering. routing_net == nullptr
  // returns the raw aggregate (no subtraction). When routing_net is
  // supplied, the per-net contribution by that net is subtracted so
  // its own committed vias don't penalise its own search.
  int getViaRisk(int cut_layer,
                 int dbu_x,
                 int dbu_y,
                 drNet* routing_net) const;

  // Phase 4.3 placeholder. Returns 0 in Phase 4.2. dir is the routing
  // direction (-1 = unspecified, 0 = horizontal, 1 = vertical) for
  // future directional kernels.
  int getPlanarRisk(int layer, int dbu_x, int dbu_y, int dir) const;

  // Stats accessors — called by FlexDR to write the CSV row after each
  // worker batch.
  ConstraintFieldStats& stats() { return stats_; }
  const ConstraintFieldStats& stats() const { return stats_; }

 private:
  bool initialized_ = false;

  odb::Rect region_;
  int tile_pitch_dbu_ = 0;
  int num_tile_x_ = 0;
  int num_tile_y_ = 0;
  int num_layers_ = 0;
  int die_ll_x_ = 0;
  int die_ll_y_ = 0;

  // Sparse storage keyed by ((layer * num_tile_y + ty) * num_tile_x +
  // tx). uint16_t saturating. Sparse (unordered_map) for Phase 4.2
  // because typical worker drcBox has few cuts; dense vector reserved
  // for Phase 4.3 when planar metal coverage is high.
  std::unordered_map<std::size_t, std::uint16_t> via_risk_;

  // Per-owner-net contribution map for same-net filtering. Keyed by a
  // packed (cellIdx << 32) | net_id pair; net_id is a small int
  // assigned at first sighting via owner_to_id_. We hash a 64-bit
  // packed key so unordered_map can stay lightweight. The per-net
  // contribution can never exceed the aggregate by construction.
  std::unordered_map<std::uint64_t, std::uint16_t> via_risk_per_net_;
  std::unordered_map<drNet*, std::uint32_t> owner_to_id_;
  std::uint32_t next_owner_id_ = 1;  // 0 reserved for "none"

  // Stats populated by the builder; read by the FlexDR-side CSV
  // writer. Mutable because getViaRisk() is const but bumps the
  // query-side counters (P4.3 instrumentation).
  mutable ConstraintFieldStats stats_;

  std::size_t cellIdx(int layer, int ty, int tx) const;
  std::uint32_t getOrAssignOwnerId(drNet* owner_net);
  std::uint64_t packPerNetKey(std::size_t cell_idx,
                              std::uint32_t owner_id) const;
};

}  // namespace drt
