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

    // P4.A — size BBH heat array to same per-(layer, ty, tx) shape as
    // layer_heat_ would have, only allocated when BBH is enabled to
    // keep the disabled path zero-overhead.
    if (options_.bbh_enabled) {
      const std::size_t bbh_cells
          = static_cast<std::size_t>(num_layers_)
            * static_cast<std::size_t>(num_tile_y_)
            * static_cast<std::size_t>(num_tile_x_);
      bbh_heat_.assign(bbh_cells, 0);
    }
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
  // Patch 3.2 — tail dump after observation. Done here (not in
  // observeOneMarker) because we need the total marker count up
  // front to decide whether to dump.
  dumpTailMarkersIfEnabled(markers);
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::vector<std::unique_ptr<frMarker>>& markers)
{
  for (const auto& m : markers) {
    if (m != nullptr) {
      observeOneMarker(*m);
    }
  }
  dumpTailMarkersIfEnabled(markers);
}

void AdaptiveMarkerModel::dumpTailMarkersIfEnabled(
    const std::list<std::unique_ptr<frMarker>>& markers)
{
  if (!options_.log_tail_csv || options_.tail_log_path.empty()) {
    return;
  }
  if (static_cast<int>(markers.size()) > options_.tail_dump_threshold) {
    return;
  }
  for (const auto& m : markers) {
    if (m != nullptr) {
      writeTailRow(*m);
    }
  }
}

void AdaptiveMarkerModel::dumpTailMarkersIfEnabled(
    const std::vector<std::unique_ptr<frMarker>>& markers)
{
  if (!options_.log_tail_csv || options_.tail_log_path.empty()) {
    return;
  }
  if (static_cast<int>(markers.size()) > options_.tail_dump_threshold) {
    return;
  }
  for (const auto& m : markers) {
    if (m != nullptr) {
      writeTailRow(*m);
    }
  }
}

void AdaptiveMarkerModel::writeTailRow(const frMarker& marker)
{
  const bool need_header = !tail_csv_header_written_;
  std::ofstream os(options_.tail_log_path,
                   need_header ? std::ios::trunc : std::ios::app);
  if (!os.is_open()) {
    return;
  }
  if (need_header) {
    os << "iter,rule,constraint_typeid,layer,"
          "bbox_xMin,bbox_yMin,bbox_xMax,bbox_yMax,"
          "center_x,center_y,num_srcs,num_aggressors,net_ids\n";
    tail_csv_header_written_ = true;
  }
  const AdaptiveRuleClass rule = classifyConstraint(marker.getConstraint());
  const auto* c = marker.getConstraint();
  const int ctype = c ? static_cast<int>(c->typeId()) : -1;
  const odb::Rect bb = marker.getBBox();
  const int cx = (bb.xMin() + bb.xMax()) / 2;
  const int cy = (bb.yMin() + bb.yMax()) / 2;
  os << iter_ << ',' << static_cast<int>(rule) << ',' << ctype << ','
     << marker.getLayerNum() << ',' << bb.xMin() << ',' << bb.yMin()
     << ',' << bb.xMax() << ',' << bb.yMax() << ',' << cx << ',' << cy;
  // num_srcs only — getAggressors() is non-const on frMarker so it
  // can't be called from this const-reference path. Leaving a
  // placeholder column for schema stability; not needed for H1 test.
  os << ',' << marker.getSrcs().size() << ',' << -1;
  // net_ids — semicolon-separated list of frcNet owner IDs
  os << ',';
  bool first = true;
  for (frBlockObject* obj : marker.getSrcs()) {
    if (obj == nullptr || obj->typeId() != frcNet) {
      continue;
    }
    auto* net = static_cast<frNet*>(obj);
    if (!first) {
      os << ';';
    }
    os << net->getId();
    first = false;
  }
  os << '\n';
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
  // Patch 3.1b — iter gate lowered from `iter < 2` to `iter < 1`.
  //
  // The Patch 3.1a instrumentation showed: with the original gate,
  // iters 0 and 1 received ZERO policy calls. Those two iters
  // accounted for 99.6% of all markers on ISPD-18 test9 (92,709 +
  // 1,618 of 94,717 total). By iter 2 only 386 markers remained;
  // by iter 3 only 4 — too few for policy multipliers to influence
  // outer-iter count.
  //
  // Iter 0 still gets identity policy (heat array is empty on the
  // first beginOuterIter; observeGlobalMarkers runs AFTER iter 0's
  // searchRepair). Iter 1 is the first iter where the heat map
  // reflects real DRC data, so it's the earliest meaningful
  // activation point.
  if (iter < 1) {
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

  // Threshold ladder. Patch 3.1c — effective threshold is the max of
  // the static floor (options_.*_threshold) and the dynamic percentile
  // (dynamic_*_threshold_, computed at the previous endOuterIter).
  // Patch 3.1d — multipliers come from Options (overridable via env).
  const int eff_severe = std::max(options_.severe_hotspot_threshold,
                                  dynamic_severe_threshold_);
  const int eff_hot
      = std::max(options_.hotspot_threshold, dynamic_hot_threshold_);
  bool is_severe = false;
  bool is_hot = false;
  if (static_cast<int>(total_heat) >= eff_severe) {
    policy.marker_cost_mul = options_.severe_marker_mul;
    policy.drc_cost_mul = options_.severe_drc_mul;
    if (options_.severe_decay_override > 0.0f) {
      policy.marker_decay_override
          = std::max(base_decay, options_.severe_decay_override);
    }
    is_severe = true;
  } else if (static_cast<int>(total_heat) >= eff_hot) {
    policy.marker_cost_mul = options_.hot_marker_mul;
    policy.drc_cost_mul = options_.hot_drc_mul;
    is_hot = true;
  }

  // P4.A — additionally compute the BBH-tier mul from the per-(layer,
  // tile) bbh_heat_ array summed over drc_box tiles. Combined with the
  // H-tier drc_cost_mul via max() rather than multiplication: a tile
  // that is H-hot OR BBH-severe gets the WORSE single penalty, not a
  // compound penalty. This is the deliberate fix vs. the failed P4.1
  // H+Stitch trial that multiplied the two muls.
  float bbh_mul = 1.0f;
  bool bbh_is_hot = false;
  bool bbh_is_severe = false;
  if (options_.bbh_enabled && !bbh_heat_.empty()) {
    std::uint64_t bbh_total = 0;
    for (int l = 0; l < num_layers_; ++l) {
      for (int ty = ty_lo; ty <= ty_hi; ++ty) {
        for (int tx = tx_lo; tx <= tx_hi; ++tx) {
          bbh_total += bbh_heat_[bbhHeatIdx(l, ty, tx)];
        }
      }
    }
    if (dynamic_bbh_severe_threshold_ > 0
        && static_cast<int>(bbh_total) >= dynamic_bbh_severe_threshold_) {
      bbh_mul = options_.bbh_severe_drc_mul;
      bbh_is_severe = true;
    } else if (dynamic_bbh_hot_threshold_ > 0
               && static_cast<int>(bbh_total) >= dynamic_bbh_hot_threshold_) {
      bbh_mul = options_.bbh_hot_drc_mul;
      bbh_is_hot = true;
    }
    if (bbh_is_severe) {
      ++iter_bbh_severe_workers_;
    } else if (bbh_is_hot) {
      ++iter_bbh_hot_workers_;
    }
    // Combine ADDITIVELY (via max, not multiplication) with the H-tier
    // drc_cost_mul. This is the load-bearing P4.A vs P4.1 distinction.
    policy.drc_cost_mul = std::max(policy.drc_cost_mul, bbh_mul);
    policy.bbh_mul = bbh_mul;
  }

  // Cap mults at 2.0 (defensive — current values never exceed 1.5).
  policy.drc_cost_mul = std::min(policy.drc_cost_mul, 2.0f);
  policy.marker_cost_mul = std::min(policy.marker_cost_mul, 2.0f);
  policy.fixed_shape_cost_mul = std::min(policy.fixed_shape_cost_mul, 2.0f);

  // Patch 3.1a — diagnostics: count calls, hot/severe workers, and
  // (optionally) emit a per-call CSV row for offline coverage analysis.
  ++iter_policy_calls_;
  if (is_severe) {
    ++iter_severe_workers_;
  } else if (is_hot) {
    ++iter_hot_workers_;
  }
  if (options_.log_policy_csv && !options_.policy_log_path.empty()) {
    const bool need_header = !policy_csv_header_written_;
    std::ofstream os(options_.policy_log_path,
                     need_header ? std::ios::trunc : std::ios::app);
    if (os.is_open()) {
      if (need_header) {
        os << "iter,drc_x1,drc_y1,drc_x2,drc_y2,total_heat,"
              "is_hot,is_severe,drc_mul,marker_mul,fixed_mul,"
              "marker_decay_override,bbh_mul\n";
        policy_csv_header_written_ = true;
      }
      os << iter << ',' << drc_box.xMin() << ',' << drc_box.yMin()
         << ',' << drc_box.xMax() << ',' << drc_box.yMax() << ','
         << total_heat << ',' << (is_hot ? 1 : 0) << ','
         << (is_severe ? 1 : 0) << ',' << policy.drc_cost_mul << ','
         << policy.marker_cost_mul << ',' << policy.fixed_shape_cost_mul
         << ',' << policy.marker_decay_override << ',' << policy.bbh_mul
         << '\n';
    }
  }

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
  // P4.A — decay BBH heat with the same factor so stale stitch heat
  // fades at the same rate as the per-rule heat.
  for (auto& h : bbh_heat_) {
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
  dynamic_hot_threshold_ = 0;
  dynamic_severe_threshold_ = 0;
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

  // Patch 3.1c — compute percentile-based dynamic thresholds. Sort a
  // copy of the tile-total array, pick the (1 - hot_percentile) and
  // (1 - severe_percentile) quantiles. Tiles with heat = 0 dominate
  // most ISPD designs (routing is sparse), so excluding zeros before
  // computing percentiles gives a more useful threshold on the
  // actually-hot subset.
  std::vector<std::uint32_t> nonzero;
  nonzero.reserve(tile_total.size());
  for (auto h : tile_total) {
    if (h > 0) {
      nonzero.push_back(h);
    }
  }
  if (!nonzero.empty() && options_.hot_percentile > 0.0f) {
    std::sort(nonzero.begin(), nonzero.end());
    const std::size_t n = nonzero.size();
    const auto hot_idx = static_cast<std::size_t>(
        n * (1.0f - std::clamp(options_.hot_percentile, 0.0f, 1.0f)));
    const auto sev_idx = static_cast<std::size_t>(
        n
        * (1.0f - std::clamp(options_.severe_percentile, 0.0f, 1.0f)));
    dynamic_hot_threshold_
        = static_cast<int>(nonzero[std::min(hot_idx, n - 1)]);
    dynamic_severe_threshold_
        = static_cast<int>(nonzero[std::min(sev_idx, n - 1)]);
  }

  // Hotspot rect list — uses effective severe threshold (max of static
  // floor + dynamic percentile).
  const int eff_severe = std::max(options_.severe_hotspot_threshold,
                                  dynamic_severe_threshold_);
  for (int ty = 0; ty < num_tile_y_; ++ty) {
    for (int tx = 0; tx < num_tile_x_; ++tx) {
      const std::uint32_t h = tile_total[ty * num_tile_x_ + tx];
      if (static_cast<int>(h) >= eff_severe) {
        odb::Rect r;
        r.init(die_ll_x_ + tx * tile_pitch_dbu_,
               die_ll_y_ + ty * tile_pitch_dbu_,
               die_ll_x_ + (tx + 1) * tile_pitch_dbu_,
               die_ll_y_ + (ty + 1) * tile_pitch_dbu_);
        hotspots_.push_back(r);
      }
    }
  }

  // P4.A — compute per-iter dynamic BBH thresholds from the bbh_heat_
  // distribution. Same nonzero-only percentile method used for
  // layer_heat_ above. Reset to 0 so an iter with no observed stitches
  // (or BBH disabled) leaves the threshold inactive.
  dynamic_bbh_hot_threshold_ = 0;
  dynamic_bbh_severe_threshold_ = 0;
  if (options_.bbh_enabled && !bbh_heat_.empty()) {
    std::vector<std::uint32_t> bbh_nonzero;
    bbh_nonzero.reserve(bbh_heat_.size());
    for (auto h : bbh_heat_) {
      if (h > 0) {
        bbh_nonzero.push_back(h);
      }
    }
    if (!bbh_nonzero.empty() && options_.bbh_hot_percentile > 0.0f) {
      std::sort(bbh_nonzero.begin(), bbh_nonzero.end());
      const std::size_t n = bbh_nonzero.size();
      const auto hot_idx = static_cast<std::size_t>(
          n * (1.0f - std::clamp(options_.bbh_hot_percentile, 0.0f, 1.0f)));
      const auto sev_idx = static_cast<std::size_t>(
          n * (1.0f - std::clamp(options_.bbh_severe_percentile, 0.0f, 1.0f)));
      dynamic_bbh_hot_threshold_
          = static_cast<int>(bbh_nonzero[std::min(hot_idx, n - 1)]);
      dynamic_bbh_severe_threshold_
          = static_cast<int>(bbh_nonzero[std::min(sev_idx, n - 1)]);
    }
  }
}

void AdaptiveMarkerModel::writeCsvRowIfEnabled()
{
  if (!options_.log_csv || options_.log_path.empty()) {
    iter_rule_counts_.fill(0);
    iter_total_markers_ = 0;
    iter_weighted_score_ = 0;
    // P4.A — keep per-iter BBH counters in sync with the rest of the
    // per-iter accumulators even when CSV is disabled.
    iter_bbh_hot_workers_ = 0;
    iter_bbh_severe_workers_ = 0;
    total_stitch_markers_observed_ = 0;
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
    // Other. Patch 3.1a appended policy_calls / hot_workers /
    // severe_workers.
    os << "iter,total_markers,weighted_score,num_hotspots,"
          "short_count,cut_short_count,metal_spacing_count,cut_spacing_count,"
          "eol_count,min_area_count,ns_metal_count,min_step_count,other_count,"
          "num_tile_x,num_tile_y,tile_pitch_dbu,"
          "policy_calls,hot_workers,severe_workers,"
          // P4.A — BBH diagnostic columns.
          "bbh_hot_workers,bbh_severe_workers,"
          "dynamic_bbh_hot_threshold,dynamic_bbh_severe_threshold,"
          "total_stitch_markers_observed\n";
    csv_header_written_ = true;
  }
  os << iter_ << ',' << iter_total_markers_ << ','
     << iter_weighted_score_ << ',' << hotspots_.size();
  for (int r = 0; r < static_cast<int>(kNumAdaptiveRuleClasses); ++r) {
    os << ',' << iter_rule_counts_[r];
  }
  os << ',' << num_tile_x_ << ',' << num_tile_y_ << ','
     << tile_pitch_dbu_ << ',' << iter_policy_calls_ << ','
     << iter_hot_workers_ << ',' << iter_severe_workers_ << ','
     << iter_bbh_hot_workers_ << ',' << iter_bbh_severe_workers_ << ','
     << dynamic_bbh_hot_threshold_ << ',' << dynamic_bbh_severe_threshold_
     << ',' << total_stitch_markers_observed_ << '\n';
  os.flush();

  iter_rule_counts_.fill(0);
  iter_total_markers_ = 0;
  iter_weighted_score_ = 0;
  iter_policy_calls_ = 0;
  iter_hot_workers_ = 0;
  iter_severe_workers_ = 0;
  // P4.A — reset BBH per-iter counters.
  iter_bbh_hot_workers_ = 0;
  iter_bbh_severe_workers_ = 0;
  total_stitch_markers_observed_ = 0;
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

std::size_t AdaptiveMarkerModel::bbhHeatIdx(int layer, int ty, int tx) const
{
  return (static_cast<std::size_t>(layer) * num_tile_y_ + ty) * num_tile_x_
         + tx;
}

// P4.A — accumulate stitch markers into bbh_heat_ at the (layer, tile)
// containing the marker's bbox center. Mirrors observeOneMarker but
// uses a single per-marker tile (the center) rather than the inflated
// bbox span, since stitch heat is meant to fire on the discrete grid
// line the marker straddles.
void AdaptiveMarkerModel::observeStitchMarker(const frMarker& marker)
{
  if (!options_.bbh_enabled || bbh_heat_.empty()) {
    return;
  }
  const frLayerNum layer = marker.getLayerNum();
  const int layer_idx = std::clamp<int>(layer, 0, num_layers_ - 1);

  const odb::Rect bb = marker.getBBox();
  const int cx = (bb.xMin() + bb.xMax()) / 2;
  const int cy = (bb.yMin() + bb.yMax()) / 2;
  const int tx = std::clamp<int>(
      (cx - die_ll_x_) / tile_pitch_dbu_, 0, num_tile_x_ - 1);
  const int ty = std::clamp<int>(
      (cy - die_ll_y_) / tile_pitch_dbu_, 0, num_tile_y_ - 1);

  const std::size_t i = bbhHeatIdx(layer_idx, ty, tx);
  const std::uint32_t cur = bbh_heat_[i];
  const std::uint32_t bumped = cur + 1u;
  bbh_heat_[i] = static_cast<std::uint16_t>(
      std::min<std::uint32_t>(bumped, options_.heat_max));
  ++total_stitch_markers_observed_;
}

}  // namespace drt
