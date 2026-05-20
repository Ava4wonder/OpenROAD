// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4.3 — ProjectionIndex implementation.

#include "dr/ProjectionIndex.h"

#include <algorithm>

namespace drt {

ProjectionIndex::ProjectionIndex() = default;
ProjectionIndex::~ProjectionIndex() = default;

void ProjectionIndex::initialize(const odb::Rect& region, int num_layers)
{
  region_ = region;
  num_layers_ = std::max(1, num_layers);
  by_layer_.assign(static_cast<std::size_t>(num_layers_), {});
  total_features_ = 0;
  initialized_ = true;
}

void ProjectionIndex::addFeature(const GeoFeature& feature)
{
  if (!initialized_) {
    return;
  }
  if (feature.layer < 0 || feature.layer >= num_layers_) {
    return;
  }
  by_layer_[static_cast<std::size_t>(feature.layer)].push_back(feature);
  ++total_features_;
}

void ProjectionIndex::queryByLayer(
    frLayerNum layer,
    const odb::Rect& roi,
    std::vector<const GeoFeature*>& out) const
{
  if (!initialized_) {
    return;
  }
  if (layer < 0 || layer >= num_layers_) {
    return;
  }
  const auto& vec = by_layer_[static_cast<std::size_t>(layer)];
  for (const auto& f : vec) {
    // Bbox intersection — Manhattan, half-open by convention.
    if (f.bbox.xMax() < roi.xMin() || f.bbox.xMin() > roi.xMax()) {
      continue;
    }
    if (f.bbox.yMax() < roi.yMin() || f.bbox.yMin() > roi.yMax()) {
      continue;
    }
    out.push_back(&f);
  }
}

int ProjectionIndex::featuresOnLayer(frLayerNum layer) const
{
  if (!initialized_ || layer < 0 || layer >= num_layers_) {
    return 0;
  }
  return static_cast<int>(by_layer_[static_cast<std::size_t>(layer)].size());
}

int ProjectionIndex::peakLayerSize() const
{
  int peak = 0;
  for (const auto& v : by_layer_) {
    if (static_cast<int>(v.size()) > peak) {
      peak = static_cast<int>(v.size());
    }
  }
  return peak;
}

}  // namespace drt
