// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.c.legality.synthetic — SyntheticOracle implementation.
// See header for scope and the not-a-physical-DRC-model disclaimer.

#include "SyntheticOracle.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>

#include "Hashing.h"

namespace drt::redesign::overlay {

namespace {

bool BboxOverlap(const Rect& a, const Rect& b) noexcept
{
  return !(a.ur.x < b.ll.x || b.ur.x < a.ll.x || a.ur.y < b.ll.y
           || b.ur.y < a.ll.y);
}

// Chebyshev (L∞) edge-to-edge distance between two non-overlapping
// rectangles. 0 if they overlap. Used for the synthetic spacing
// rule: a candidate within kSyntheticSpacingThresholdDbu of any
// same-layer context shape is flagged.
std::int32_t MinEdgeDistance(const Rect& a, const Rect& b) noexcept
{
  std::int32_t dx = 0;
  if (a.ur.x < b.ll.x) {
    dx = b.ll.x - a.ur.x;
  } else if (b.ur.x < a.ll.x) {
    dx = a.ll.x - b.ur.x;
  }
  std::int32_t dy = 0;
  if (a.ur.y < b.ll.y) {
    dy = b.ll.y - a.ur.y;
  } else if (b.ur.y < a.ll.y) {
    dy = a.ll.y - b.ur.y;
  }
  return std::max(dx, dy);
}

// Inflate a rect by `halo` on each side. Used to size the base
// query so we capture every shape that might be within
// kSyntheticSpacingThresholdDbu of the candidate.
Rect ExpandRect(const Rect& r, std::int32_t halo) noexcept
{
  Rect out = r;
  out.ll.x -= halo;
  out.ll.y -= halo;
  out.ur.x += halo;
  out.ur.y += halo;
  return out;
}

// Build a multiset key set from deleted_context for fast lookup
// during the "skip if also-deleted" check on each base neighbour.
std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t,
                    std::int32_t, std::int32_t>>
BuildDeletedKeySet(const OracleCandidateBatch& batch)
{
  std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t,
                      std::int32_t, std::int32_t>>
      keys;
  for (const auto& d : batch.deleted_context) {
    keys.emplace(d.bbox.ll.x, d.bbox.ll.y, d.bbox.ur.x, d.bbox.ur.y,
                 static_cast<std::int32_t>(d.layer));
  }
  return keys;
}

bool IsDeleted(
    const std::set<std::tuple<std::int32_t, std::int32_t,
                              std::int32_t, std::int32_t,
                              std::int32_t>>& deleted_keys,
    const ShapeRef& base_shape)
{
  if (!base_shape.layer.has_value()) {
    return false;  // can't match a layer-less base entry
  }
  auto key = std::make_tuple(
      base_shape.bbox.ll.x, base_shape.bbox.ll.y, base_shape.bbox.ur.x,
      base_shape.bbox.ur.y, static_cast<std::int32_t>(*base_shape.layer));
  return deleted_keys.count(key) > 0;
}

}  // namespace

LegalityVerdict SyntheticOracle::Evaluate(
    const OracleCandidateBatch& batch,
    const GeometryView& base)
{
  LegalityVerdict v;
  v.source = LegalitySource::SyntheticOracle;

  // Adapter-level non-Ok status: the bridge already determined this
  // proposal can't be validated. Surface the status through the
  // verdict so eval can mark commit_eligible=false. The synthetic
  // oracle does not "rescue" UnsupportedDelta or UnresolvedFootprint.
  if (batch.status != OracleCandidateBatch::Status::Ok) {
    v.legal = false;
    v.source = LegalitySource::UnresolvedFootprint;  // mapping below
    if (batch.status == OracleCandidateBatch::Status::UnsupportedDelta) {
      // No specific source enum for adapter-unsupported; reuse
      // UnresolvedFootprint as the catch-all non-trusted bucket.
      // V2.2.c.score may add a more specific source if needed.
    }
    return v;
  }

  const auto deleted_keys = BuildDeletedKeySet(batch);

  // Walk each candidate. Same-layer overlap or spacing-below-threshold
  // anywhere in the context flags the whole batch illegal.
  for (std::size_t i = 0; i < batch.added.size(); ++i) {
    const OracleCandidateShape& cand = batch.added[i];

    // Query base on this layer in a halo-expanded box. Any shape
    // within kSyntheticSpacingThresholdDbu could matter.
    const Rect query_box
        = ExpandRect(cand.bbox, kSyntheticSpacingThresholdDbu);
    const auto neighbours = base.QueryRouteShapes(query_box, cand.layer);

    for (const ShapeRef& n : neighbours) {
      if (IsDeleted(deleted_keys, n)) {
        continue;  // shape is being removed by this same batch
      }
      // Layer should match (we queried on cand.layer), but be
      // defensive in case the base view returns layer-less entries.
      if (n.layer.has_value() && n.layer.value() != cand.layer) {
        continue;
      }
      if (BboxOverlap(cand.bbox, n.bbox)) {
        v.legal = false;
        v.violations.push_back(MarkerKind::Short);
        v.marker_count_after += 1;
        continue;
      }
      if (MinEdgeDistance(cand.bbox, n.bbox)
          < kSyntheticSpacingThresholdDbu) {
        v.legal = false;
        v.violations.push_back(MarkerKind::PrlSpacing);
        v.marker_count_after += 1;
      }
    }

    // Inter-candidate checks (other added shapes in this batch on
    // the same layer).
    for (std::size_t j = i + 1; j < batch.added.size(); ++j) {
      const OracleCandidateShape& other = batch.added[j];
      if (other.layer != cand.layer) {
        continue;
      }
      if (BboxOverlap(cand.bbox, other.bbox)) {
        v.legal = false;
        v.violations.push_back(MarkerKind::Short);
        v.marker_count_after += 1;
        continue;
      }
      if (MinEdgeDistance(cand.bbox, other.bbox)
          < kSyntheticSpacingThresholdDbu) {
        v.legal = false;
        v.violations.push_back(MarkerKind::PrlSpacing);
        v.marker_count_after += 1;
      }
    }
  }

  return v;
}

}  // namespace drt::redesign::overlay
