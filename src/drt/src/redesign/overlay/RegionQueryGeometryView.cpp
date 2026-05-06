// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.3 — RegionQueryGeometryView implementation. See header for
// the V2.1 contract, lifetime rules, and the include-discipline note
// (this is the only file in redesign/overlay/ permitted to include
// frDesign / frMarker / odb internals).

#include "RegionQueryGeometryView.h"

#include <iostream>
#include <stdexcept>
#include <vector>

#include "Hashing.h"
#include "ShadowDump.h"
#include "db/obj/frMarker.h"
#include "db/tech/frConstraint.h"
#include "frDesign.h"
#include "frRegionQuery.h"
#include "odb/geom.h"

namespace drt::redesign::overlay {

namespace {

inline Rect ToOverlayRect(const odb::Rect& in) noexcept
{
  Rect r;
  r.ll.x = in.xMin();
  r.ll.y = in.yMin();
  r.ur.x = in.xMax();
  r.ur.y = in.yMax();
  return r;
}

inline odb::Rect FromOverlayRect(const Rect& in) noexcept
{
  return odb::Rect(in.ll.x, in.ll.y, in.ur.x, in.ur.y);
}

}  // namespace

// V2.1 MarkerRef identity is sufficient for query-path equivalence,
// not for unique signoff marker identity. Two distinct constraints
// of the same type can theoretically produce markers with the same
// bbox + layer + constraint_type_id; for V2.1.e's use case (compare
// direct-query path against GeometryView projection on the same
// design at the same time), both sides project identically, so the
// equivalence proof holds. Real per-marker identity (constraint_id
// + source_net_id) is V2.2+ once we know how the consumer wants it.
MarkerRef ProjectMarker(const ::drt::frMarker& m)
{
  MarkerRef out;
  out.bbox = ToOverlayRect(m.getBBox());
  // frMarker::getLayerNum returns frLayerNum (int16_t alias); always
  // present on a real marker.
  out.layer = static_cast<LayerNum>(m.getLayerNum());
  // Constraint type id: present iff the marker carries a constraint.
  if (const ::drt::frConstraint* c = m.getConstraint()) {
    out.constraint_type_id = static_cast<uint32_t>(c->typeId());
  }
  // constraint_id: intentionally absent in V2.1.e. No stable
  // per-process numeric id exists for frConstraint*; synthesising one
  // from pointer addresses or local enumeration would violate the
  // pointer-free discipline (Hashing.h §1).
  // source_net_id: intentionally absent in V2.1.e. frMarker::getSrcs
  // returns multiple source frBlockObject*; picking one as "the"
  // source net would be arbitrary. V2.2+ may add a vector<SourceRef>
  // when a real consumer needs it.
  return out;
}

MarkerQueryResult RegionQueryGeometryView::QueryMarkers(
    const Rect& box) const
{
  if (design_ == nullptr) {
    return {};
  }
  const ::drt::frRegionQuery* rq = design_->getRegionQuery();
  if (rq == nullptr) {
    return {};
  }
  std::vector<::drt::frMarker*> raw;
  rq->queryMarker(FromOverlayRect(box), raw);

  MarkerQueryResult out;
  out.reserve(raw.size());
  for (const ::drt::frMarker* m : raw) {
    if (m == nullptr) {
      continue;
    }
    out.push_back(ProjectMarker(*m));
  }
  return out;
}

ShapeQueryResult RegionQueryGeometryView::QueryRouteShapes(
    const Rect& box,
    LayerNum layer) const
{
  (void) box;
  (void) layer;
  throw std::logic_error(
      "GeometryView: QueryRouteShapes not supported in V2.1");
}

GuideQueryResult RegionQueryGeometryView::QueryGuides(
    const Rect& box) const
{
  (void) box;
  throw std::logic_error(
      "GeometryView: QueryGuides not supported in V2.1");
}

BlockageQueryResult RegionQueryGeometryView::QueryBlockages(
    const Rect& box,
    LayerNum layer) const
{
  (void) box;
  (void) layer;
  throw std::logic_error(
      "GeometryView: QueryBlockages not supported in V2.1");
}

PinAccessQueryResult RegionQueryGeometryView::QueryPinAccess(
    const Rect& box) const
{
  (void) box;
  throw std::logic_error(
      "GeometryView: QueryPinAccess not supported in V2.1");
}

void ShadowCompareMarkers(
    const ::drt::frDesign* design,
    const ::odb::Rect& box,
    const std::vector<::drt::frMarker*>& legacy_result)
{
  if (design == nullptr) {
    return;
  }

  // Project the legacy raw pointer set into MarkerRefs using the same
  // ProjectMarker function that QueryMarkers calls. Single source of
  // truth for the projection.
  MarkerQueryResult legacy_proj;
  legacy_proj.reserve(legacy_result.size());
  for (const ::drt::frMarker* m : legacy_result) {
    if (m != nullptr) {
      legacy_proj.push_back(ProjectMarker(*m));
    }
  }

  // Run the GeometryView path on the same (design, box).
  RegionQueryGeometryView view(design);
  const Rect overlay_box = ToOverlayRect(box);
  const MarkerQueryResult overlay = view.QueryMarkers(overlay_box);

  const uint64_t legacy_hash = HashCanonicalRange(legacy_proj);
  const uint64_t overlay_hash = HashCanonicalRange(overlay);

  // V2.1.e.5: best-effort CSV dump under OPENROAD_OVERLAY_DUMP_HASHES.
  // Maintains per-process counters even when dumping is disabled; the
  // atexit summary reports calls + mismatches.
  ShadowDump::RecordMarkerComparison(legacy_hash,
                                     overlay_hash,
                                     legacy_proj.size(),
                                     overlay.size(),
                                     overlay_box);

  if (legacy_hash != overlay_hash) {
    // Diagnostic only — the legacy frMarker* path remains
    // authoritative and `legacy_result` is unmodified by this
    // function. A mismatch indicates a projection or query-path
    // drift bug in RegionQueryGeometryView; investigate, do not
    // route on it.
    std::cerr << "[drt-redesign-overlay] V2.1.e shadow marker-hash "
              << "mismatch: legacy=" << std::hex << legacy_hash
              << " overlay=" << overlay_hash << std::dec
              << " legacy_count=" << legacy_proj.size()
              << " overlay_count=" << overlay.size() << "\n";
  }
}

}  // namespace drt::redesign::overlay
