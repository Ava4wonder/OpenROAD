// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4 — ConstraintField implementation.
//
// Phase 4.1: addViaRisk + getViaRisk are wired but the builder doesn't
// populate any tiles yet, so all risk queries return 0 and the maze
// cost is unchanged. Stats are written for sanity checking.

#include "dr/ConstraintField.h"

#include <algorithm>
#include <limits>

namespace drt {

ConstraintField::ConstraintField() = default;
ConstraintField::~ConstraintField() = default;

void ConstraintField::initialize(const odb::Rect& region,
                                 int tile_pitch_dbu,
                                 int num_layers)
{
  region_ = region;
  tile_pitch_dbu_ = std::max(1, tile_pitch_dbu);
  die_ll_x_ = region.xMin();
  die_ll_y_ = region.yMin();
  num_tile_x_
      = std::max(1, (region.dx() + tile_pitch_dbu_ - 1) / tile_pitch_dbu_);
  num_tile_y_
      = std::max(1, (region.dy() + tile_pitch_dbu_ - 1) / tile_pitch_dbu_);
  num_layers_ = std::max(1, num_layers);
  initialized_ = true;
  via_risk_.clear();
  stats_.num_tile_x = num_tile_x_;
  stats_.num_tile_y = num_tile_y_;
  stats_.num_layers = num_layers_;
  stats_.drc_box = region;
}

std::size_t ConstraintField::cellIdx(int layer, int ty, int tx) const
{
  return (static_cast<std::size_t>(layer) * num_tile_y_
          + static_cast<std::size_t>(ty))
             * num_tile_x_
         + static_cast<std::size_t>(tx);
}

void ConstraintField::addViaRisk(int cut_layer,
                                 int tile_x,
                                 int tile_y,
                                 int delta)
{
  if (!initialized_ || delta <= 0) {
    return;
  }
  if (cut_layer < 0 || cut_layer >= num_layers_) {
    return;
  }
  if (tile_x < 0 || tile_x >= num_tile_x_) {
    return;
  }
  if (tile_y < 0 || tile_y >= num_tile_y_) {
    return;
  }
  const std::size_t key = cellIdx(cut_layer, tile_y, tile_x);
  auto& v = via_risk_[key];
  const int next = static_cast<int>(v) + delta;
  v = static_cast<std::uint16_t>(
      std::min(next, static_cast<int>(std::numeric_limits<uint16_t>::max())));
  ++stats_.cut_splats;
}

int ConstraintField::getViaRisk(int cut_layer, int dbu_x, int dbu_y) const
{
  if (!initialized_ || via_risk_.empty()) {
    return 0;
  }
  if (cut_layer < 0 || cut_layer >= num_layers_) {
    return 0;
  }
  const int tx = (dbu_x - die_ll_x_) / tile_pitch_dbu_;
  const int ty = (dbu_y - die_ll_y_) / tile_pitch_dbu_;
  if (tx < 0 || tx >= num_tile_x_ || ty < 0 || ty >= num_tile_y_) {
    return 0;
  }
  const auto it = via_risk_.find(cellIdx(cut_layer, ty, tx));
  if (it == via_risk_.end()) {
    return 0;
  }
  return static_cast<int>(it->second);
}

int ConstraintField::getPlanarRisk(int /*layer*/,
                                   int /*dbu_x*/,
                                   int /*dbu_y*/,
                                   int /*dir*/) const
{
  // Phase 4.3 — returns 0 in Phase 4.2.
  return 0;
}

}  // namespace drt
