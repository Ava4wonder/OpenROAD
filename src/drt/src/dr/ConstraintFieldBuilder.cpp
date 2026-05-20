// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4 — ConstraintFieldBuilder implementation.
//
// Phase 4.1 (this commit): record the worker's drcBox + design layer
// count into the field's region, populate stats. NO splats — fields
// remain empty so getViaRisk() returns 0 and routing is unchanged.
//
// Phase 4.2 (next commit): walk the worker's local committed vias /
// fixed cuts; for each, inflate by a conservative cut-spacing kernel
// and splat into the per-cut-layer risk tiles. The actual maze-cost
// query path lives in FlexGridGraph and is added in 4.2 too.

#include "dr/ConstraintFieldBuilder.h"

#include "db/obj/frBlock.h"
#include "db/tech/frLayer.h"
#include "db/tech/frTechObject.h"
#include "dr/FlexDR.h"
#include "frDesign.h"
#include "utl/Logger.h"

namespace drt {

ConstraintFieldBuilder::ConstraintFieldBuilder(
    const ConstraintFieldPolicy& policy,
    utl::Logger* logger)
    : policy_(policy), logger_(logger)
{
}

void ConstraintFieldBuilder::build(ConstraintField& field,
                                   FlexDRWorker* worker,
                                   frDesign* design,
                                   int iter,
                                   int worker_id,
                                   int batch_id)
{
  const auto t0 = std::chrono::steady_clock::now();
  if (!policy_.enabled || worker == nullptr) {
    return;
  }

  const odb::Rect drc_box = worker->getDrcBox();
  // Tile pitch — Phase 4.1 picks a coarse default matching the
  // AdaptiveMarkerModel heat-grid pitch (10000 dbu = 10 um at 1 dbu
  // = 1 nm). Worker-local fields can afford finer pitches later;
  // Phase 4.2 will tune this from the via-pitch distribution.
  constexpr int kTilePitchDbu = 10000;

  int num_layers = 0;
  if (design != nullptr && design->getTech() != nullptr) {
    num_layers = static_cast<int>(design->getTech()->getLayers().size());
  }

  field.initialize(drc_box, kTilePitchDbu, num_layers);

  auto& stats = field.stats();
  stats.iter = iter;
  stats.batch_id = batch_id;
  stats.worker_id = worker_id;
  // Phase 4.1: just-initialised, no splats. Counts stay at 0; the
  // CSV row documents that the field is wired but doesn't change
  // routing.
  stats.num_shapes_seen = 0;
  stats.num_vias_seen = 0;
  stats.spacing_splats = 0;
  stats.cut_splats = 0;
  stats.field_nonzero_tiles = 0;
  stats.field_max_risk = 0;
  stats.field_mean_nonzero_risk = 0.0;
  // Rough memory accounting — Phase 4.1 only holds the tile geometry,
  // no per-tile entries until Phase 4.2 starts splatting.
  stats.memory_bytes
      = static_cast<int>(sizeof(ConstraintField)
                         + sizeof(ConstraintFieldStats));

  const auto t1 = std::chrono::steady_clock::now();
  stats.build_wall_ms
      = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace drt
