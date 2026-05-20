// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4 — POD types for the ConstraintField framework.
//
// The framework adds a proactive, advisory DRC-risk layer to DRT. Existing
// vias/cuts (Phase 4.2) and fixed metal (Phase 4.3) are splatted into
// per-layer uint16_t risk tiles. During FlexGridGraph maze expansion the
// router queries those tiles and adds a small risk-cost so the search
// avoids high-risk geometry BEFORE FlexGC materialises a marker.
//
// Strict correctness contract:
//   * The field is advisory. It is never authoritative — exact FlexGC +
//     frMarker + the existing repair loop remain the legality oracle.
//   * False positives are acceptable (over-penalise → small detour).
//   * False negatives are acceptable (under-penalise → exact check
//     still catches the violation).
//   * With OPENROAD_DRT_CONSTRAINT_FIELD unset, no field is built and
//     no risk cost is added — routing output MUST be bit-identical to
//     the without-field baseline.

#pragma once

#include <cstdint>
#include <string>

#include "odb/geom.h"

namespace drt {

// Patch 4 — runtime config for the constraint-field layer. Env vars:
//   OPENROAD_DRT_CONSTRAINT_FIELD=1              enable (default off)
//   OPENROAD_DRT_CONSTRAINT_FIELD_STATS_DIR=path stats CSV output dir
//   OPENROAD_DRT_CF_VIA_LAMBDA=<float>           cost weight for via
//                                                expansion (default 1.0)
//   OPENROAD_DRT_CF_SPACING_LAMBDA=<float>       planar spacing weight
//                                                (reserved for 4.3)
//   OPENROAD_DRT_CF_MAX_RISK_COST=<int>          saturating cap (default
//                                                100 * markerCost-like)
struct ConstraintFieldPolicy
{
  bool enabled = false;
  // Phase 4.2: via/cut spacing weight. cost += lambda_cut * F_cut.
  float lambda_cut = 1.0f;
  // Phase 4.3 (reserved). Planar metal-spacing weight.
  float lambda_spacing = 1.0f;
  // Saturating upper bound on per-tile risk cost added to maze edge.
  int max_risk_cost = 1000;
  // Stats CSV output directory. Empty = cwd.
  std::string stats_dir;

  // Patch 4.3 — marker-conditioned + conservative activation.
  // Default to the new semantics (marker-conditioned, repair-stage-
  // only) because P4.2's blanket via splat was a documented
  // negative result on test9. The legacy P4.2 behaviour stays
  // reachable via env override (set marker_conditioned=0).
  // Env vars:
  //   OPENROAD_DRT_CF_MARKER_CONDITIONED=0/1 (default 1)
  //   OPENROAD_DRT_CF_SKIP_ITER_ZERO=0/1     (default 1)
  //   OPENROAD_DRT_CF_SKIP_CLEAN_WORKERS=0/1 (default 1)
  //   OPENROAD_DRT_CF_MARKER_BLOAT_DBU=<int> (default 2000 = 2um)
  //   OPENROAD_DRT_CF_SAME_NET_WEIGHT=<flt>  (default 0.0 — discount)
  bool marker_conditioned = true;
  bool skip_iter_zero = true;
  bool skip_clean_workers = true;
  int marker_bloat_dbu = 2000;
  float same_net_weight = 0.0f;
};

// Patch 4 — one CSV row per worker field build. Populated by
// ConstraintFieldBuilder during FlexDRWorker init and merged
// serially after each OMP batch (same pattern as Patch 3.5
// AdaptiveWorkerProfile).
struct ConstraintFieldStats
{
  int iter = 0;
  int batch_id = 0;
  int worker_id = 0;
  // Worker drc_box dimensions (for sanity / sparseness analysis).
  odb::Rect drc_box;
  int num_tile_x = 0;
  int num_tile_y = 0;
  int num_layers = 0;

  // Counts from the build pass.
  int num_shapes_seen = 0;       // fixed metal + committed metal
  int num_vias_seen = 0;         // committed vias / cuts
  int spacing_splats = 0;        // planar tile writes (Phase 4.3)
  int cut_splats = 0;            // via/cut tile writes (Phase 4.2)
  int field_nonzero_tiles = 0;
  int field_max_risk = 0;
  double field_mean_nonzero_risk = 0.0;

  // Resource accounting.
  double build_wall_ms = 0.0;
  int memory_bytes = 0;
};

}  // namespace drt
