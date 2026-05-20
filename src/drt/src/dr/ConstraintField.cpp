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

std::uint32_t ConstraintField::getOrAssignOwnerId(drNet* owner_net)
{
  if (owner_net == nullptr) {
    return 0;  // anonymous / fixed shape
  }
  auto it = owner_to_id_.find(owner_net);
  if (it != owner_to_id_.end()) {
    return it->second;
  }
  const std::uint32_t id = next_owner_id_++;
  owner_to_id_.emplace(owner_net, id);
  return id;
}

std::uint64_t ConstraintField::packPerNetKey(std::size_t cell_idx,
                                             std::uint32_t owner_id) const
{
  // Top 32 bits = owner id, bottom 32 = cell index. cell_idx is bounded
  // by num_layers * num_tile_x * num_tile_y of a worker drcBox — well
  // under 2^32 for any realistic worker.
  return (static_cast<std::uint64_t>(owner_id) << 32)
         | static_cast<std::uint64_t>(cell_idx & 0xFFFFFFFFULL);
}

void ConstraintField::addViaRisk(int cut_layer,
                                 int tile_x,
                                 int tile_y,
                                 int delta,
                                 drNet* owner_net)
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
  if (owner_net != nullptr) {
    const std::uint32_t oid = getOrAssignOwnerId(owner_net);
    auto& pv = via_risk_per_net_[packPerNetKey(key, oid)];
    const int next_pv = static_cast<int>(pv) + delta;
    pv = static_cast<std::uint16_t>(std::min(
        next_pv, static_cast<int>(std::numeric_limits<uint16_t>::max())));
  }
  ++stats_.cut_splats;
}

int ConstraintField::getViaRisk(int cut_layer,
                                int dbu_x,
                                int dbu_y,
                                drNet* routing_net) const
{
  ++stats_.field_query_count;  // P4.3 instrumentation
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
  const std::size_t key = cellIdx(cut_layer, ty, tx);
  const auto it = via_risk_.find(key);
  if (it == via_risk_.end()) {
    return 0;
  }
  int risk = static_cast<int>(it->second);
  if (routing_net != nullptr && !via_risk_per_net_.empty()) {
    const auto own_it = owner_to_id_.find(routing_net);
    if (own_it != owner_to_id_.end()) {
      const auto pit
          = via_risk_per_net_.find(packPerNetKey(key, own_it->second));
      if (pit != via_risk_per_net_.end()) {
        risk -= static_cast<int>(pit->second);
        if (risk < 0) {
          risk = 0;
        }
      }
    }
  }
  if (risk > 0) {
    ++stats_.field_query_hit_count;
    stats_.field_query_total_cost_added += risk;
  }
  return risk;
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
