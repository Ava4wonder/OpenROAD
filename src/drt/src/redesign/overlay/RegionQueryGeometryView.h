// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.3 — first concrete GeometryView backend.
//
// V2.1 only: read-through view over live FlexDR/FlexGridGraph state.
// Provides API compatibility, not multi-version snapshot isolation.
// V2.4 replaces this backend with SnapshotGeometryView (persistent
// CoW backing) per v2 plan §2.5.
//
// V2.1.e implements **only QueryMarkers**. The other query methods
// throw std::logic_error per the GeometryView V2.1 contract; tests
// assert the throws. Per-entity rollout (route shapes, guides,
// blockages, pin access) lands in V2.2.a sub-commits.
//
// Include discipline: this header forward-declares frDesign only;
// the heavy OpenROAD includes (frDesign.h / frMarker.h /
// dbBlockObject) live in RegionQueryGeometryView.cpp. This is the
// ONLY file in redesign/overlay/ permitted to include those headers.
//
// **Lifetime contract:** RegionQueryGeometryView holds a NON-OWNING
// pointer to frDesign. The caller guarantees frDesign outlives the
// view. In V2.1, the view is constructed inside a worker's read
// path where the design's lifetime trivially dominates the view's;
// V2.4's persistent SnapshotGeometryView will switch to a different
// ownership model.

#pragma once

#include <vector>

#include "GeometryView.h"

namespace drt {
class frBlockObject;
class frDesign;
class frMarker;
}  // namespace drt
namespace odb {
class Rect;
}  // namespace odb

namespace drt::redesign::overlay {

class RegionQueryGeometryView final : public GeometryView
{
 public:
  explicit RegionQueryGeometryView(const ::drt::frDesign* design)
      : design_(design)
  {
  }

  // V2.1-supported. Projects each frMarker* the region query returns
  // into a MarkerRef value (no pointers escape). Canonical identity
  // is (bbox, layer, constraint_type_id); constraint_id and
  // source_net_id are intentionally absent in V2.1.e — see
  // implementation comment.
  MarkerQueryResult QueryMarkers(const Rect& box) const override;

  // V2.2+ — throw per GeometryView V2.1 contract.
  ShapeQueryResult QueryRouteShapes(const Rect& box,
                                    LayerNum layer) const override;
  GuideQueryResult QueryGuides(const Rect& box) const override;
  BlockageQueryResult QueryBlockages(const Rect& box,
                                     LayerNum layer) const override;
  PinAccessQueryResult QueryPinAccess(const Rect& box) const override;

 private:
  // Non-owning. frDesign outlives FlexDRWorker / SnapshotHandle in
  // V2.1; never delete through this pointer.
  const ::drt::frDesign* design_;
};

// Single source of truth for frMarker -> MarkerRef projection. Used by
// RegionQueryGeometryView::QueryMarkers and by the V2.1.e.4 shadow
// comparator. Pointer-free identity per V2.1 contract.
MarkerRef ProjectMarker(const ::drt::frMarker& m);

// V2.2.a.1 — single source of truth for frBlockObject -> ShapeRef
// projection of route shapes (frPathSeg / frVia / frPatchWire).
//
// `bbox` is taken from the rq_box_value_t pair (not getBBox()) since
// the region-query index already produced a canonical box for the
// indexed shape. Returns nullopt for non-route-shape block-object
// kinds (blockages, terms, etc.) so callers can filter; those land
// in V2.2.a.3 (BlockageRef) and elsewhere with their own projection.
std::optional<ShapeRef> ProjectRouteShape(
    const ::drt::frBlockObject& obj,
    const ::odb::Rect& bbox);

// V2.1.e.4 shadow validation. Diagnostic-only, never alters behaviour.
//
// Compare a legacy regionQuery->queryMarker result against
// RegionQueryGeometryView::QueryMarkers on the same (design, box).
// Both sides project via ProjectMarker, then HashCanonicalRange. If
// hashes differ, a single-line warning fires on stderr. Match: silent.
//
// V2.1.e.5 (separate commit) adds the OPENROAD_OVERLAY_DUMP_HASHES
// env-var path that appends every comparison's hashes to a dump file
// for cross-run A/B diff.
//
// IMPORTANT: this is NOT a substitution. The legacy frMarker* path
// remains authoritative for routing behaviour; the shadow path proves
// equivalence without owning state.
void ShadowCompareMarkers(
    const ::drt::frDesign* design,
    const ::odb::Rect& box,
    const std::vector<::drt::frMarker*>& legacy_result);

}  // namespace drt::redesign::overlay
