// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.b — Reference predicates for the four FastPass rule types.
// See Predicates.h for the dual-role contract.

#include "Predicates.h"

#include <algorithm>

namespace drt::redesign::legality {

namespace {

// Shapes are on the same net iff both have non-zero net_id and they match.
// net_id == 0 is a "fixed" geometry (blockage / pin metal), treated as
// belonging to no specific net so it always conflicts with same-layer
// candidate metal of any net.
bool SameNet(const Shape& a, const Shape& b)
{
  return a.net_id != 0 && a.net_id == b.net_id;
}

// Inclusive AABB overlap.
bool Overlap(const Shape& a, const Shape& b)
{
  return !(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

}  // namespace

bool MetalShortReference(const Shape& cand, const Shape& ctx, void* /*opaque*/)
{
  if (cand.layer != ctx.layer) {
    return false;
  }
  if (SameNet(cand, ctx)) {
    return false;
  }
  return Overlap(cand, ctx);
}

bool PrlSpacingReference(const Shape& cand, const Shape& ctx, void* opaque)
{
  if (cand.layer != ctx.layer) {
    return false;
  }
  if (SameNet(cand, ctx)) {
    return false;
  }
  const auto* cfg = static_cast<const PrlSpacingConfig*>(opaque);

  // X-spacing with parallel run in y.
  const std::int32_t y_overlap
      = std::min(cand.y2, ctx.y2) - std::max(cand.y1, ctx.y1);
  if (y_overlap > 0 && y_overlap >= cfg->prl_threshold) {
    const std::int32_t x_dist
        = std::max(cand.x1 - ctx.x2, ctx.x1 - cand.x2);
    if (x_dist >= 0 && x_dist < cfg->min_spacing) {
      return true;
    }
  }
  // Y-spacing with parallel run in x.
  const std::int32_t x_overlap
      = std::min(cand.x2, ctx.x2) - std::max(cand.x1, ctx.x1);
  if (x_overlap > 0 && x_overlap >= cfg->prl_threshold) {
    const std::int32_t y_dist
        = std::max(cand.y1 - ctx.y2, ctx.y1 - cand.y2);
    if (y_dist >= 0 && y_dist < cfg->min_spacing) {
      return true;
    }
  }
  return false;
}

bool EolSpacingReference(const Shape& cand, const Shape& ctx, void* opaque)
{
  if (cand.layer != ctx.layer) {
    return false;
  }
  if (SameNet(cand, ctx)) {
    return false;
  }
  const auto* cfg = static_cast<const EolSpacingConfig*>(opaque);

  // Vertical EOL edges (left at x=cand.x1, right at x=cand.x2): triggered
  // when the edge length cand.y2-cand.y1 < eol_width_threshold.
  if ((cand.y2 - cand.y1) < cfg->eol_width_threshold) {
    const std::int32_t y_lo = cand.y1 - cfg->eol_within;
    const std::int32_t y_hi = cand.y2 + cfg->eol_within;
    const bool y_overlaps_extended = !(ctx.y2 < y_lo || ctx.y1 > y_hi);
    if (y_overlaps_extended) {
      const std::int32_t x_dist_right = ctx.x1 - cand.x2;  // ctx to right
      if (x_dist_right >= 0 && x_dist_right < cfg->eol_spacing) {
        return true;
      }
      const std::int32_t x_dist_left = cand.x1 - ctx.x2;  // ctx to left
      if (x_dist_left >= 0 && x_dist_left < cfg->eol_spacing) {
        return true;
      }
    }
  }

  // Horizontal EOL edges (top/bottom): triggered when cand.x2-cand.x1
  // < eol_width_threshold.
  if ((cand.x2 - cand.x1) < cfg->eol_width_threshold) {
    const std::int32_t x_lo = cand.x1 - cfg->eol_within;
    const std::int32_t x_hi = cand.x2 + cfg->eol_within;
    const bool x_overlaps_extended = !(ctx.x2 < x_lo || ctx.x1 > x_hi);
    if (x_overlaps_extended) {
      const std::int32_t y_dist_above = ctx.y1 - cand.y2;
      if (y_dist_above >= 0 && y_dist_above < cfg->eol_spacing) {
        return true;
      }
      const std::int32_t y_dist_below = cand.y1 - ctx.y2;
      if (y_dist_below >= 0 && y_dist_below < cfg->eol_spacing) {
        return true;
      }
    }
  }

  return false;
}

bool CutSpacingReference(const Shape& cand, const Shape& ctx, void* opaque)
{
  if (cand.layer != ctx.layer) {
    return false;
  }
  if (SameNet(cand, ctx)) {
    return false;
  }
  const auto* cfg = static_cast<const CutSpacingConfig*>(opaque);
  // L-infinity edge-to-edge distance. Negative gaps clamped to 0 so
  // touching/overlapping cuts (gap == 0) trigger when min_spacing > 0.
  const std::int32_t x_gap
      = std::max(0, std::max(cand.x1 - ctx.x2, ctx.x1 - cand.x2));
  const std::int32_t y_gap
      = std::max(0, std::max(cand.y1 - ctx.y2, ctx.y1 - cand.y2));
  const std::int32_t edge_dist = std::max(x_gap, y_gap);
  return edge_dist < cfg->min_spacing;
}

void EvaluateReference(const Shape* candidates,
                       std::size_t candidates_count,
                       const Shape* context,
                       std::size_t context_count,
                       const RuleEntry* rules,
                       std::size_t rule_count,
                       Verdict* verdicts)
{
  for (std::size_t c = 0; c < candidates_count; ++c) {
    verdicts[c] = Verdict{};
  }
  for (std::size_t c = 0; c < candidates_count; ++c) {
    for (std::size_t k = 0; k < context_count; ++k) {
      for (std::size_t r = 0; r < rule_count; ++r) {
        if (rules[r].predicate(candidates[c], context[k], rules[r].opaque)) {
          verdicts[c].legal = false;
          verdicts[c].triggered_rules |= static_cast<std::uint16_t>(
              1u << static_cast<std::uint8_t>(rules[r].type));
        }
      }
    }
  }
}

}  // namespace drt::redesign::legality
