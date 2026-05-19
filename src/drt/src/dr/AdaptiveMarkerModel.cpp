// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 2 — observation + CSV logging implementation.
//
// The model now:
//   - Sizes a flat per-(rule, layer, tile_y, tile_x) heat array lazily
//     on first beginOuterIter(), once design dimensions are known.
//   - beginOuterIter(): decays heat by options_.heat_decay.
//   - observeGlobalMarkers(): classifies each marker, inflates the
//     bbox rule-aware, accumulates weighted heat across overlapping
//     tiles, and bumps per-net scores.
//   - endOuterIter(): scans the heat array to update hotspot list +
//     writes a per-iter CSV row when log_csv is enabled.
//
// getWorkerPolicy() still returns an identity policy (real scalar
// multipliers land in Patch 3). The point of Patch 2 is to validate
// observation correctness with no routing behaviour change.

#include "dr/AdaptiveMarkerModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db/obj/frBlock.h"
#include "db/obj/frMarker.h"
#include "db/obj/frNet.h"
#include "db/tech/frConstraint.h"
#include "db/tech/frTechObject.h"
#include "dr/AdaptiveMarkerUtils.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "utl/Logger.h"

namespace drt {

namespace {

// Default tile pitch in DBU when options_.tile_size_dbu is 0. Chosen
// so test9's ~1820000 × 1568800 DBU die produces a ~180 × 156 grid =
// ~28K tiles per (rule, layer) slice, ~2M total entries × 2 bytes =
// ~4 MB heat array. Manageable.
constexpr int kDefaultTilePitchDbu = 10000;

// Per-marker default inflation. Picked up by getRuleAwareInflatedBox
// in AdaptiveMarkerUtils — spacing rules multiply by 1.5.
constexpr int kDefaultMarkerBloatDbu = 1000;

}  // namespace

AdaptiveMarkerModel::AdaptiveMarkerModel(const Options& options,
                                         frDesign* design,
                                         utl::Logger* logger)
    : options_(options), design_(design), logger_(logger)
{
}

AdaptiveMarkerModel::~AdaptiveMarkerModel()
{
  // Best-effort CSV flush — destructor cannot throw.
  // (writeCsvHeaderIfNeeded + endOuterIter already flush after each iter,
  // so this is normally a no-op.)
}

void AdaptiveMarkerModel::beginOuterIter(int iter)
{
  iter_ = iter;

  // Lazy-size the heat arrays on first call. We need the design's
  // top-block die box + the tech's layer count, which are stable
  // once routing starts.
  if (rule_layer_heat_.empty() && design_ != nullptr
      && design_->getTopBlock() != nullptr) {
    const odb::Rect die = design_->getTopBlock()->getDieBox();
    const int tile = options_.tile_size_dbu > 0 ? options_.tile_size_dbu
                                                : kDefaultTilePitchDbu;
    // +1 to cover the partial tile at the upper edge.
    num_tile_x_ = std::max(1, (die.dx() + tile - 1) / tile);
    num_tile_y_ = std::max(1, (die.dy() + tile - 1) / tile);
    die_ll_x_ = die.xMin();
    die_ll_y_ = die.yMin();
    tile_pitch_dbu_ = tile;
    num_layers_ = static_cast<int>(
        design_->getTech() != nullptr
            ? design_->getTech()->getLayers().size()
            : 0);
    // Cap at a sane upper bound; layers above this are exotic and the
    // tile pitch doesn't reflect them well.
    if (num_layers_ < 1) {
      num_layers_ = 1;
    }
    const std::size_t cells = static_cast<std::size_t>(kNumAdaptiveRuleClasses)
                              * static_cast<std::size_t>(num_layers_)
                              * static_cast<std::size_t>(num_tile_y_)
                              * static_cast<std::size_t>(num_tile_x_);
    rule_layer_heat_.assign(cells, 0);
  }

  decayHeat();
}

void AdaptiveMarkerModel::endOuterIter()
{
  updateHotspots();
  writeCsvRowIfEnabled();
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::list<std::unique_ptr<frMarker>>& markers)
{
  for (const auto& m : markers) {
    if (m != nullptr) {
      observeOneMarker(*m);
    }
  }
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::vector<std::unique_ptr<frMarker>>& markers)
{
  for (const auto& m : markers) {
    if (m != nullptr) {
      observeOneMarker(*m);
    }
  }
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::vector<frMarker>& markers)
{
  for (const auto& m : markers) {
    observeOneMarker(m);
  }
}

void AdaptiveMarkerModel::observeWorkerStats(
    const AdaptiveWorkerStats& /*stats*/)
{
  // Patch 2: not used yet. Patches 3+ feed congestion flags + rule
  // counts from workers back into the model.
}

AdaptiveWorkerPolicy AdaptiveMarkerModel::getWorkerPolicy(
    const odb::Rect& /*route_box*/,
    const odb::Rect& drc_box,
    int iter,
    frUInt4 /*base_drc_cost*/,
    frUInt4 /*base_marker_cost*/,
    frUInt4 /*base_fixed_shape_cost*/,
    float base_decay) const
{
  // Patch 3 — compute real scalar multipliers from per-tile heat
  // overlap with the worker's drc_box.
  //
  // Activation gates:
  //   1. Model must be enabled (env var set).
  //   2. Heat array must be sized (set on first beginOuterIter).
  //   3. Iter gate: only apply policy from iter 2 onwards. Iters 0-1
  //      have no useful heat history to draw on (iter 0 starts with
  //      empty heat; iter 1 has just one decayed iter of history).
  //      Applying policy early risks biasing the search before the
  //      hotspot signal is reliable. From iter 2 the model has 2+
  //      iterations of decay-accumulated heat per tile.
  //   4. Caps: drc_cost_mul / marker_cost_mul capped at 2.0;
  //      marker_decay_override capped at 0.99.

  AdaptiveWorkerPolicy policy;
  policy.enabled = options_.enabled;
  if (!options_.enabled) {
    return policy;
  }
  if (rule_layer_heat_.empty()) {
    return policy;
  }
  if (iter < 2) {
    return policy;
  }

  // Sum heat over tiles overlapping drc_box (across all rules + layers).
  const int tx_lo = std::clamp<int>(
      (drc_box.xMin() - die_ll_x_) / tile_pitch_dbu_, 0, num_tile_x_ - 1);
  const int tx_hi = std::clamp<int>(
      (drc_box.xMax() - die_ll_x_) / tile_pitch_dbu_, 0, num_tile_x_ - 1);
  const int ty_lo = std::clamp<int>(
      (drc_box.yMin() - die_ll_y_) / tile_pitch_dbu_, 0, num_tile_y_ - 1);
  const int ty_hi = std::clamp<int>(
      (drc_box.yMax() - die_ll_y_) / tile_pitch_dbu_, 0, num_tile_y_ - 1);

  std::uint64_t total_heat = 0;
  for (int r = 0; r < static_cast<int>(kNumAdaptiveRuleClasses); ++r) {
    for (int l = 0; l < num_layers_; ++l) {
      for (int ty = ty_lo; ty <= ty_hi; ++ty) {
        for (int tx = tx_lo; tx <= tx_hi; ++tx) {
          total_heat += rule_layer_heat_[heatIdx(r, l, ty, tx)];
        }
      }
    }
  }

  // Threshold ladder. Hotspot/severe thresholds are configurable via
  // Options. Default 200 / 800 from current_state.md's risk cap doc.
  if (static_cast<int>(total_heat) >= options_.severe_hotspot_threshold) {
    policy.marker_cost_mul = 1.50f;
    policy.drc_cost_mul = 1.25f;
    policy.marker_decay_override
        = std::min(0.99f, std::max(base_decay, 0.99f));
  } else if (static_cast<int>(total_heat) >= options_.hotspot_threshold) {
    policy.marker_cost_mul = 1.25f;
    policy.drc_cost_mul = 1.10f;
    // No decay override at the moderate level — only severe.
  }

  // Cap mults at 2.0 (defensive — current values never exceed 1.5).
  policy.drc_cost_mul = std::min(policy.drc_cost_mul, 2.0f);
  policy.marker_cost_mul = std::min(policy.marker_cost_mul, 2.0f);
  policy.fixed_shape_cost_mul = std::min(policy.fixed_shape_cost_mul, 2.0f);

  return policy;
}

int AdaptiveMarkerModel::getMarkerIncrement(const frMarker& /*marker*/,
                                            bool /*is_via*/,
                                            frLayerNum /*layer*/) const
{
  return 10;
}

int AdaptiveMarkerModel::getNetPriority(frNet* net) const
{
  const auto it = net_score_.find(net);
  return it == net_score_.end() ? 0 : it->second;
}

bool AdaptiveMarkerModel::shouldIncreaseClipSize(
    const odb::Rect& /*region*/) const
{
  return false;
}

// --- private ---

void AdaptiveMarkerModel::decayHeat()
{
  if (options_.heat_decay >= 1.0f) {
    return;
  }
  const float k = std::max(0.0f, options_.heat_decay);
  for (auto& h : rule_layer_heat_) {
    h = static_cast<std::uint16_t>(std::lround(h * k));
  }
}

void AdaptiveMarkerModel::observeOneMarker(const frMarker& marker)
{
  if (rule_layer_heat_.empty()) {
    return;
  }
  const AdaptiveRuleClass rule = classifyConstraint(marker.getConstraint());
  const int rule_idx = static_cast<int>(rule);
  const frLayerNum layer = marker.getLayerNum();
  // Clamp layer into the allocated range; markers on out-of-range
  // layers (which shouldn't happen) go into layer 0 rather than
  // silently mis-index.
  const int layer_idx = std::clamp<int>(layer, 0, num_layers_ - 1);

  const int weight = getRuleWeight(rule);

  // Net-score accumulation (independent of tile geometry).
  for (frNet* net : extractMarkerNets(marker)) {
    net_score_[net] += weight;
  }

  // Tile range covered by the inflated marker bbox.
  const odb::Rect bb
      = getRuleAwareInflatedBox(marker, rule, kDefaultMarkerBloatDbu);
  const int tx_lo = std::clamp<int>(
      (bb.xMin() - die_ll_x_) / tile_pitch_dbu_, 0, num_tile_x_ - 1);
  const int tx_hi = std::clamp<int>(
      (bb.xMax() - die_ll_x_) / tile_pitch_dbu_, 0, num_tile_x_ - 1);
  const int ty_lo = std::clamp<int>(
      (bb.yMin() - die_ll_y_) / tile_pitch_dbu_, 0, num_tile_y_ - 1);
  const int ty_hi = std::clamp<int>(
      (bb.yMax() - die_ll_y_) / tile_pitch_dbu_, 0, num_tile_y_ - 1);

  for (int ty = ty_lo; ty <= ty_hi; ++ty) {
    for (int tx = tx_lo; tx <= tx_hi; ++tx) {
      const std::size_t i = heatIdx(rule_idx, layer_idx, ty, tx);
      const std::uint32_t cur = rule_layer_heat_[i];
      const std::uint32_t bumped = cur + static_cast<std::uint32_t>(weight);
      rule_layer_heat_[i] = static_cast<std::uint16_t>(
          std::min<std::uint32_t>(bumped, options_.heat_max));
    }
  }

  // Per-iter rule counter for the CSV log.
  if (rule_idx >= 0 && rule_idx < static_cast<int>(kNumAdaptiveRuleClasses)) {
    iter_rule_counts_[rule_idx]++;
  }
  iter_total_markers_++;
  iter_weighted_score_ += weight;
}

void AdaptiveMarkerModel::updateHotspots()
{
  hotspots_.clear();
  if (rule_layer_heat_.empty()) {
    return;
  }
  // Sum heat per tile across all rules + layers; flag tiles whose
  // total heat exceeds severe threshold.
  std::vector<std::uint32_t> tile_total(
      static_cast<std::size_t>(num_tile_x_)
          * static_cast<std::size_t>(num_tile_y_),
      0);
  for (int r = 0; r < static_cast<int>(kNumAdaptiveRuleClasses); ++r) {
    for (int l = 0; l < num_layers_; ++l) {
      for (int ty = 0; ty < num_tile_y_; ++ty) {
        for (int tx = 0; tx < num_tile_x_; ++tx) {
          tile_total[ty * num_tile_x_ + tx]
              += rule_layer_heat_[heatIdx(r, l, ty, tx)];
        }
      }
    }
  }
  for (int ty = 0; ty < num_tile_y_; ++ty) {
    for (int tx = 0; tx < num_tile_x_; ++tx) {
      const std::uint32_t h = tile_total[ty * num_tile_x_ + tx];
      if (static_cast<int>(h) >= options_.severe_hotspot_threshold) {
        odb::Rect r;
        r.init(die_ll_x_ + tx * tile_pitch_dbu_,
               die_ll_y_ + ty * tile_pitch_dbu_,
               die_ll_x_ + (tx + 1) * tile_pitch_dbu_,
               die_ll_y_ + (ty + 1) * tile_pitch_dbu_);
        hotspots_.push_back(r);
      }
    }
  }
}

void AdaptiveMarkerModel::writeCsvRowIfEnabled()
{
  if (!options_.log_csv || options_.log_path.empty()) {
    iter_rule_counts_.fill(0);
    iter_total_markers_ = 0;
    iter_weighted_score_ = 0;
    return;
  }

  const bool need_header = !csv_header_written_;
  std::ofstream os(options_.log_path, need_header ? std::ios::trunc
                                                  : std::ios::app);
  if (!os.is_open()) {
    if (logger_ != nullptr) {
      logger_->warn(utl::DRT, 990,
                    "AdaptiveMarkerModel: cannot open CSV log '{}'",
                    options_.log_path);
    }
    return;
  }
  if (need_header) {
    // Column order matches AdaptiveRuleClass enum values 0..8
    // (Short, CutShort, MetalSpacing, CutSpacing, Eol, MinArea,
    // NsMetal, MinStep, Other). Patch 2.1 inserted MinStep before
    // Other.
    os << "iter,total_markers,weighted_score,num_hotspots,"
          "short_count,cut_short_count,metal_spacing_count,cut_spacing_count,"
          "eol_count,min_area_count,ns_metal_count,min_step_count,other_count,"
          "num_tile_x,num_tile_y,tile_pitch_dbu\n";
    csv_header_written_ = true;
  }
  os << iter_ << ',' << iter_total_markers_ << ','
     << iter_weighted_score_ << ',' << hotspots_.size();
  for (int r = 0; r < static_cast<int>(kNumAdaptiveRuleClasses); ++r) {
    os << ',' << iter_rule_counts_[r];
  }
  os << ',' << num_tile_x_ << ',' << num_tile_y_ << ','
     << tile_pitch_dbu_ << '\n';
  os.flush();

  iter_rule_counts_.fill(0);
  iter_total_markers_ = 0;
  iter_weighted_score_ = 0;
}

void AdaptiveMarkerModel::addMarkerObservation(
    const AdaptiveMarkerObs& /*obs*/)
{
  // Patch 2: unused — observeOneMarker handles the work directly.
}

std::size_t AdaptiveMarkerModel::heatIdx(int rule, int layer, int ty,
                                         int tx) const
{
  return ((static_cast<std::size_t>(rule) * num_layers_ + layer)
              * num_tile_y_
          + ty)
             * num_tile_x_
         + tx;
}

}  // namespace drt
