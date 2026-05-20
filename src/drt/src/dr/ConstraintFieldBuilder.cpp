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

  // Patch 4.3 — marker-conditioned splat source.
  //
  // P4.2 splatted every committed via in every worker every iter →
  // degenerated into via-density penalty (-2.7% vias but +50% wall
  // on test9). The corrected design only emits risk near markers
  // the worker is actively trying to repair, with same-net filter
  // already in place (per-net subtraction in ConstraintField).
  //
  // Two modes:
  //   marker_conditioned = true (P4.3, default): bloat each marker
  //     bbox; iterate worker drVias; splat ONLY if via origin lies
  //     inside the bloated bbox. Same-net via still goes into the
  //     per-net map for query-time subtraction.
  //   marker_conditioned = false (P4.2 legacy): splat every via
  //     regardless of marker proximity. Reachable via env var for
  //     A/B comparison.
  if (design != nullptr && design->getTech() != nullptr) {
    auto* tech = design->getTech();

    // Pre-compute bloated marker bboxes when marker-conditioned.
    std::vector<odb::Rect> marker_rois;
    if (policy_.marker_conditioned) {
      const auto& markers = worker->getMarkers();
      marker_rois.reserve(markers.size());
      for (const auto& m : markers) {
        odb::Rect r = m.getBBox();
        const int bloat = policy_.marker_bloat_dbu;
        r.set_xlo(r.xMin() - bloat);
        r.set_ylo(r.yMin() - bloat);
        r.set_xhi(r.xMax() + bloat);
        r.set_yhi(r.yMax() + bloat);
        marker_rois.push_back(r);
      }
    }

    auto inside_any_marker_roi = [&](const odb::Point& pt) {
      if (!policy_.marker_conditioned) {
        return true;  // legacy P4.2: splat everything
      }
      for (const auto& r : marker_rois) {
        if (pt.x() >= r.xMin() && pt.x() <= r.xMax()
            && pt.y() >= r.yMin() && pt.y() <= r.yMax()) {
          return true;
        }
      }
      return false;
    };

    // Early exit: marker-conditioned mode with no marker ROIs → no
    // splats possible. (Worker passed the activation gate via the
    // initNumMarkers check on the FlexDR side, but the live
    // getMarkers() could still be empty on edge cases.)
    if (!policy_.marker_conditioned || !marker_rois.empty()) {
      for (auto& net_uptr : worker->getNets()) {
        drNet* net = net_uptr.get();
        if (net == nullptr) {
          continue;
        }
        auto handle
            = [&](const std::vector<std::unique_ptr<drConnFig>>& figs) {
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
                  const odb::Point origin = via->getOrigin();
                  if (!inside_any_marker_roi(origin)) {
                    continue;  // P4.3 marker-conditioned filter
                  }
                  const frLayerNum cut_layer = vd->getCutLayerNum();
                  if (cut_layer < 0 || cut_layer >= num_layers) {
                    continue;
                  }
                  const auto* layer = tech->getLayer(cut_layer);
                  int cut_spacing_dbu = 0;
                  if (layer != nullptr) {
                    cut_spacing_dbu = layer->getCutSpacingValue();
                  }
                  int kernel_tiles = (cut_spacing_dbu + kTilePitchDbu - 1)
                                     / kTilePitchDbu;
                  if (kernel_tiles < 1) {
                    kernel_tiles = 1;
                  }
                  if (kernel_tiles > 6) {
                    kernel_tiles = 6;
                  }
                  const int cx_tile
                      = (origin.x() - drc_box.xMin()) / kTilePitchDbu;
                  const int cy_tile
                      = (origin.y() - drc_box.yMin()) / kTilePitchDbu;
                  drNet* owner = net;
                  for (int dy = -kernel_tiles; dy <= kernel_tiles; ++dy) {
                    for (int dx = -kernel_tiles; dx <= kernel_tiles; ++dx) {
                      const int dist = std::abs(dx) + std::abs(dy);
                      const int risk
                          = std::max(1, kernel_tiles + 1 - dist);
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
