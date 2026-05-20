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

#include "db/drObj/drNet.h"
#include "db/drObj/drVia.h"
#include "db/obj/frBlock.h"
#include "db/tech/frLayer.h"
#include "db/tech/frTechObject.h"
#include "db/tech/frViaDef.h"
#include "dr/FlexDR.h"
#include "frBaseTypes.h"
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

  // Phase 4.2 — iterate the worker's drNets; for each committed
  // drVia in route + ext connFigs, splat into the cut-layer risk
  // field, kernel sized by LEF cut spacing. Owner-net tracked so
  // the maze query can subtract same-net contribution.
  if (design != nullptr && design->getTech() != nullptr) {
    auto* tech = design->getTech();
    for (auto& net_uptr : worker->getNets()) {
      drNet* net = net_uptr.get();
      if (net == nullptr) {
        continue;
      }
      auto handle = [&](const std::vector<std::unique_ptr<drConnFig>>& figs) {
        for (auto& fig_uptr : figs) {
          if (fig_uptr == nullptr) {
            continue;
          }
          if (fig_uptr->typeId() != drcVia) {
            continue;
          }
          auto* via = static_cast<drVia*>(fig_uptr.get());
          const auto* vd = via->getViaDef();
          if (vd == nullptr) {
            continue;
          }
          ++stats.num_vias_seen;
          const frLayerNum cut_layer = vd->getCutLayerNum();
          if (cut_layer < 0 || cut_layer >= num_layers) {
            continue;
          }
          // LEF cut spacing — frLayer::getCutSpacingValue returns the
          // worst (largest) cut-to-cut spacing constraint on that
          // layer, falling back to 0 when none is defined.
          const auto* layer = tech->getLayer(cut_layer);
          int cut_spacing_dbu = 0;
          if (layer != nullptr) {
            cut_spacing_dbu = layer->getCutSpacingValue();
          }
          // Kernel radius: spacing rounded up to whole tiles, plus
          // one for the cut's own tile coverage. Bounded so a huge
          // LEF spacing doesn't cover the whole worker.
          int kernel_tiles
              = (cut_spacing_dbu + kTilePitchDbu - 1) / kTilePitchDbu;
          if (kernel_tiles < 1) {
            kernel_tiles = 1;  // at minimum splat own tile + 8-neighbour
          }
          if (kernel_tiles > 6) {
            kernel_tiles = 6;
          }
          const odb::Point origin = via->getOrigin();
          const int cx_tile = (origin.x() - drc_box.xMin()) / kTilePitchDbu;
          const int cy_tile = (origin.y() - drc_box.yMin()) / kTilePitchDbu;
          drNet* owner = net;
          for (int dy = -kernel_tiles; dy <= kernel_tiles; ++dy) {
            for (int dx = -kernel_tiles; dx <= kernel_tiles; ++dx) {
              // Risk magnitude decays with manhattan distance.
              const int dist = std::abs(dx) + std::abs(dy);
              int risk = std::max(1, kernel_tiles + 1 - dist);
              field.addViaRisk(static_cast<int>(cut_layer),
                               cx_tile + dx,
                               cy_tile + dy,
                               risk * 10,
                               owner);
            }
          }
        }
      };
      handle(net->getRouteConnFigs());
      handle(net->getExtConnFigs());
    }
  }

  // Phase 4.3 reserved — planar metal spacing splat.

  // Stats roll-up.
  stats.spacing_splats = 0;  // 4.3
  stats.field_nonzero_tiles = 0;
  stats.field_max_risk = 0;
  stats.field_mean_nonzero_risk = 0.0;
  stats.memory_bytes = static_cast<int>(
      sizeof(ConstraintField) + sizeof(ConstraintFieldStats)
      + stats.cut_splats * (sizeof(std::size_t) + sizeof(std::uint16_t))
                  * 2);  // aggregate + per-net entries

  const auto t1 = std::chrono::steady_clock::now();
  stats.build_wall_ms
      = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace drt
