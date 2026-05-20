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
#include "dr/ProjectionIndex.h"
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

  // Patch 4.3 — marker-conditioned, projection-indexed splat source.
  //
  // P4.2 splatted every committed via every iter on every worker.
  // P4.3 first walks the worker's drNets ONCE to populate a
  // ProjectionIndex of cut-layer features (drVias), then for each
  // marker queries the index in the bloated marker ROI and emits
  // splats only for hits. This makes the field semantically
  // marker-conditioned and gives the projection-index instrumentation
  // counters real signal.
  if (design != nullptr && design->getTech() != nullptr) {
    auto* tech = design->getTech();
    ProjectionIndex pi;
    pi.initialize(drc_box, num_layers);

    // First pass: populate the projection index with every drVia
    // visible to the worker.
    for (auto& net_uptr : worker->getNets()) {
      drNet* net = net_uptr.get();
      if (net == nullptr) {
        continue;
      }
      auto index_handle
          = [&](const std::vector<std::unique_ptr<drConnFig>>& figs) {
              for (auto& fig_uptr : figs) {
                if (fig_uptr == nullptr || fig_uptr->typeId() != drcVia) {
                  continue;
                }
                auto* via = static_cast<drVia*>(fig_uptr.get());
                const auto* vd = via->getViaDef();
                if (vd == nullptr) {
                  continue;
                }
                ++stats.num_vias_seen;
                GeoFeature f;
                f.layer = vd->getCutLayerNum();
                f.kind = FeatureKind::Cut;
                const odb::Point origin = via->getOrigin();
                // Point-sized bbox at the via origin; the marker-ROI
                // query uses bbox intersection so we just need any
                // bbox containing the via origin.
                f.bbox.init(origin.x(), origin.y(), origin.x(), origin.y());
                f.owner = net;
                f.is_fixed = false;
                pi.addFeature(f);
              }
            };
      index_handle(net->getRouteConnFigs());
      index_handle(net->getExtConnFigs());
    }
    stats.proj_index_features = pi.totalFeatures();
    stats.proj_index_peak_layer = pi.peakLayerSize();

    // Second pass: marker-conditioned splat. For each marker, bloat
    // its bbox by marker_bloat_dbu and query the projection index
    // for every cut layer (only cut-layer hits are relevant for
    // via/cut spacing risk). Non-marker-conditioned mode falls back
    // to splatting every via — preserves the P4.2 baseline for
    // ablation A1.
    auto splat_via = [&](const GeoFeature& feature) {
      const frLayerNum cut_layer = feature.layer;
      if (cut_layer < 0 || cut_layer >= num_layers) {
        return;
      }
      const auto* layer = tech->getLayer(cut_layer);
      int cut_spacing_dbu = (layer != nullptr) ? layer->getCutSpacingValue() : 0;
      int kernel_tiles
          = (cut_spacing_dbu + kTilePitchDbu - 1) / kTilePitchDbu;
      if (kernel_tiles < 1) {
        kernel_tiles = 1;
      }
      if (kernel_tiles > 6) {
        kernel_tiles = 6;
      }
      const int cx_tile = (feature.bbox.xMin() - drc_box.xMin()) / kTilePitchDbu;
      const int cy_tile = (feature.bbox.yMin() - drc_box.yMin()) / kTilePitchDbu;
      for (int dy = -kernel_tiles; dy <= kernel_tiles; ++dy) {
        for (int dx = -kernel_tiles; dx <= kernel_tiles; ++dx) {
          const int dist = std::abs(dx) + std::abs(dy);
          const int risk = std::max(1, kernel_tiles + 1 - dist);
          field.addViaRisk(static_cast<int>(cut_layer),
                           cx_tile + dx,
                           cy_tile + dy,
                           risk * 10,
                           feature.owner);
        }
      }
    };

    if (policy_.marker_conditioned) {
      const auto& markers = worker->getMarkers();
      stats.num_markers_used = static_cast<int>(markers.size());
      std::vector<const GeoFeature*> hits;
      for (const auto& m : markers) {
        odb::Rect roi = m.getBBox();
        const int bloat = policy_.marker_bloat_dbu;
        roi.set_xlo(roi.xMin() - bloat);
        roi.set_ylo(roi.yMin() - bloat);
        roi.set_xhi(roi.xMax() + bloat);
        roi.set_yhi(roi.yMax() + bloat);
        // Query every cut layer at this ROI. (Layer of the marker
        // itself could be routing OR cut; cut-spacing markers carry
        // the cut layer, but other rule classes wouldn't. Querying
        // all cut layers is correct + conservative for V1.)
        for (frLayerNum L = 0; L < num_layers; ++L) {
          hits.clear();
          pi.queryByLayer(L, roi, hits);
          for (const auto* f : hits) {
            splat_via(*f);
          }
        }
      }
    } else {
      // Legacy P4.2 mode: splat every feature. Reachable via env
      // OPENROAD_DRT_CF_MARKER_CONDITIONED=0 for ablation.
      stats.num_markers_used = 0;
      for (frLayerNum L = 0; L < num_layers; ++L) {
        std::vector<const GeoFeature*> hits;
        pi.queryByLayer(L, drc_box, hits);
        for (const auto* f : hits) {
          splat_via(*f);
        }
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
