// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4.3 — ProjectionIndex: worker-local indexed
// geometry feature store for rule-relevant candidate extraction.
//
// Purpose: given a marker ROI + rule kind, return only the geometry
// features (cuts, metal shapes) that are candidates for that rule —
// not the worker's entire geometry. This is the data-structure half
// of the Phase 4.3 "projection-indexed, marker-conditioned" design:
// without it, marker conditioning is just an O(markers × shapes)
// brute-force filter; with it, the cost is O(shapes_in_roi).
//
// Phase 4.3 V1 scope: a sparse vector of GeoFeature, layered, with a
// simple bbox-bounded query (linear scan within layer; layers are
// few — typically 6-12 — and per-layer feature counts are bounded by
// what fits in a worker drcBox). A real interval-tree / BVH can come
// later if profiling shows this is the bottleneck.

#pragma once

#include <cstdint>
#include <vector>

#include "frBaseTypes.h"
#include "odb/geom.h"

namespace drt {

class drNet;

enum class FeatureKind : std::uint8_t
{
  Cut = 0,
  Metal = 1,
  PinAccess = 2,
  Blockage = 3,
  Other = 4,
};

struct GeoFeature
{
  frLayerNum layer = 0;
  FeatureKind kind = FeatureKind::Other;
  odb::Rect bbox;
  drNet* owner = nullptr;  // nullptr = fixed / no routing owner
  bool is_fixed = false;
};

class ProjectionIndex
{
 public:
  ProjectionIndex();
  ~ProjectionIndex();

  // Populate. Worker drcBox is the spatial extent.
  void initialize(const odb::Rect& region, int num_layers);

  // Add a feature. No internal sort yet — query scans the layer's
  // vector linearly. Future versions may sort or index after build.
  void addFeature(const GeoFeature& feature);

  // Query all features on a given layer whose bbox intersects roi.
  // Pushes into `out`. Doesn't clear `out` first.
  void queryByLayer(frLayerNum layer,
                    const odb::Rect& roi,
                    std::vector<const GeoFeature*>& out) const;

  // Aggregate counts for stats CSV.
  int totalFeatures() const { return total_features_; }
  int featuresOnLayer(frLayerNum layer) const;
  int peakLayerSize() const;

 private:
  bool initialized_ = false;
  odb::Rect region_;
  int num_layers_ = 0;
  std::vector<std::vector<GeoFeature>> by_layer_;
  int total_features_ = 0;
};

}  // namespace drt
