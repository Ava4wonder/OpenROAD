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
#include "../LeanMode.h"
#include "db/obj/frBlockObject.h"
#include "db/obj/frBlockage.h"
#include "db/obj/frGuide.h"
#include "db/obj/frInst.h"
#include "db/obj/frInstBlockage.h"
#include "db/obj/frMarker.h"
#include "db/obj/frNet.h"
#include "db/obj/frShape.h"
#include "db/obj/frVia.h"
#include "db/tech/frConstraint.h"
#include "db/tech/frViaDef.h"
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
std::optional<ShapeRef> ProjectRouteShape(
    const ::drt::frBlockObject& obj,
    const ::odb::Rect& bbox)
{
  ShapeRef out;
  out.bbox = ToOverlayRect(bbox);
  switch (obj.typeId()) {
    case ::drt::frcPathSeg: {
      const auto& seg = static_cast<const ::drt::frPathSeg&>(obj);
      out.layer = static_cast<LayerNum>(seg.getLayerNum());
      if (const ::drt::frNet* net = seg.getNet()) {
        out.net_id = static_cast<NetId>(net->getId());
      }
      out.shape_kind = static_cast<std::uint8_t>(::drt::frcPathSeg);
      return out;
    }
    case ::drt::frcPatchWire: {
      const auto& pw = static_cast<const ::drt::frPatchWire&>(obj);
      out.layer = static_cast<LayerNum>(pw.getLayerNum());
      if (const ::drt::frNet* net = pw.getNet()) {
        out.net_id = static_cast<NetId>(net->getId());
      }
      out.shape_kind = static_cast<std::uint8_t>(::drt::frcPatchWire);
      return out;
    }
    case ::drt::frcVia: {
      // V2.2.a.1 ShapeRef for vias is a query-equivalence projection,
      // NOT a complete via legality representation. A via has a cut
      // shape, top/bottom enclosure shapes, viaDef identity, origin,
      // and possibly multiple rectangles per cut. We compress all of
      // that to (rq_box, cut_layer, net) — sufficient for the V2.2.a.1
      // hash-equivalence check on the same query, but insufficient
      // for legality evaluation. Full via representation lands with
      // OverlayGeometryView's Delta/legality path, likely as a
      // separate ViaRef entity or a ShapeKind::Via{Cut,Enclosure}
      // distinction. Do not infer via-level correctness from the
      // ShapeRef alone.
      const auto& v = static_cast<const ::drt::frVia&>(obj);
      if (const ::drt::frViaDef* vd = v.getViaDef()) {
        out.layer = static_cast<LayerNum>(vd->getCutLayerNum());
      }
      if (const ::drt::frNet* net = v.getNet()) {
        out.net_id = static_cast<NetId>(net->getId());
      }
      out.shape_kind = static_cast<std::uint8_t>(::drt::frcVia);
      return out;
    }
    default:
      // Non-route-shape kinds (blockages, terms, etc.) are caller's
      // problem. V2.2.a.3 (BlockageRef) handles blockages with its
      // own projection.
      return std::nullopt;
  }
}

std::optional<BlockageRef> ProjectBlockage(
    const ::drt::frBlockObject& obj,
    const ::odb::Rect& bbox,
    LayerNum layer)
{
  switch (obj.typeId()) {
    case ::drt::frcBlockage: {
      BlockageRef out;
      out.bbox = ToOverlayRect(bbox);
      out.layer = layer;
      // PDK / block-level blockage — no source_inst_id.
      return out;
    }
    case ::drt::frcInstBlockage: {
      const auto& ib
          = static_cast<const ::drt::frInstBlockage&>(obj);
      BlockageRef out;
      out.bbox = ToOverlayRect(bbox);
      out.layer = layer;
      if (const ::drt::frInst* inst = ib.getInst()) {
        out.source_inst_id = static_cast<std::uint64_t>(inst->getId());
      }
      return out;
    }
    default:
      // Not a blockage — caller filters.
      return std::nullopt;
  }
}

GuideRef ProjectGuide(const ::drt::frGuide& g)
{
  GuideRef out;
  out.bbox = ToOverlayRect(g.getBBox());
  out.begin_layer = static_cast<LayerNum>(g.getBeginLayerNum());
  out.end_layer = static_cast<LayerNum>(g.getEndLayerNum());
  if (const ::drt::frNet* net = g.getNet()) {
    out.net_id = static_cast<NetId>(net->getId());
  }
  return out;
}

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
  if (design_ == nullptr) {
    return {};
  }
  const ::drt::frRegionQuery* rq = design_->getRegionQuery();
  if (rq == nullptr) {
    return {};
  }
  ::drt::frRegionQuery::Objects<::drt::frBlockObject> raw;
  rq->query(FromOverlayRect(box),
            static_cast<::drt::frLayerNum>(layer),
            raw);

  ShapeQueryResult out;
  out.reserve(raw.size());
  for (const auto& entry : raw) {
    const ::drt::frBlockObject* obj = entry.second;
    if (obj == nullptr) {
      continue;
    }
    auto projected = ProjectRouteShape(*obj, entry.first);
    if (projected.has_value()) {
      out.push_back(*projected);
    }
  }
  return out;
}

GuideQueryResult RegionQueryGeometryView::QueryGuides(
    const Rect& box) const
{
  if (design_ == nullptr) {
    return {};
  }
  const ::drt::frRegionQuery* rq = design_->getRegionQuery();
  if (rq == nullptr) {
    return {};
  }
  std::vector<::drt::frGuide*> raw;
  rq->queryGuide(FromOverlayRect(box), raw);

  GuideQueryResult out;
  out.reserve(raw.size());
  for (const ::drt::frGuide* g : raw) {
    if (g == nullptr) {
      continue;
    }
    out.push_back(ProjectGuide(*g));
  }
  return out;
}

BlockageQueryResult RegionQueryGeometryView::QueryBlockages(
    const Rect& box,
    LayerNum layer) const
{
  if (design_ == nullptr) {
    return {};
  }
  const ::drt::frRegionQuery* rq = design_->getRegionQuery();
  if (rq == nullptr) {
    return {};
  }
  ::drt::frRegionQuery::Objects<::drt::frBlockObject> raw;
  // Same backend call as QueryRouteShapes; filter is by typeId.
  // Backend coalescing deferred per V2.2.a.3 design note.
  rq->query(FromOverlayRect(box),
            static_cast<::drt::frLayerNum>(layer),
            raw);

  BlockageQueryResult out;
  out.reserve(raw.size());
  for (const auto& entry : raw) {
    const ::drt::frBlockObject* obj = entry.second;
    if (obj == nullptr) {
      continue;
    }
    auto projected = ProjectBlockage(*obj, entry.first, layer);
    if (projected.has_value()) {
      out.push_back(*projected);
    }
  }
  return out;
}

PinAccessQueryResult RegionQueryGeometryView::QueryPinAccess(
    const Rect& box) const
{
  (void) box;
  throw std::logic_error(
      "GeometryView: QueryPinAccess not supported in V2.1");
}

void ShadowCompareRouteShapes(
    const ::drt::frDesign* design,
    const ::odb::Rect& box,
    int layer,
    const std::vector<std::pair<::odb::Rect, ::drt::frBlockObject*>>&
        legacy_result)
{
  if (::drt::redesign::DrtRedesignLeanMode()) return;
  if (design == nullptr) {
    return;
  }

  // Project the legacy raw vector with the same ProjectRouteShape
  // function QueryRouteShapes uses. Single source of truth.
  ShapeQueryResult legacy_proj;
  legacy_proj.reserve(legacy_result.size());
  for (const auto& entry : legacy_result) {
    if (entry.second == nullptr) {
      continue;
    }
    auto projected = ProjectRouteShape(*entry.second, entry.first);
    if (projected.has_value()) {
      legacy_proj.push_back(*projected);
    }
  }

  // Run the GeometryView path on the same (design, box, layer).
  RegionQueryGeometryView view(design);
  const Rect overlay_box = ToOverlayRect(box);
  const ShapeQueryResult overlay
      = view.QueryRouteShapes(overlay_box, static_cast<LayerNum>(layer));

  const uint64_t legacy_hash = HashCanonicalRange(legacy_proj);
  const uint64_t overlay_hash = HashCanonicalRange(overlay);

  // Record into the unified V2.2.a.0 ShadowDump pipeline. Counters
  // update regardless of dump-enable; CSV row emitted iff env var
  // active.
  ShadowDump::RecordRouteShapeComparison(legacy_hash,
                                         overlay_hash,
                                         legacy_proj.size(),
                                         overlay.size(),
                                         overlay_box,
                                         layer);

  if (legacy_hash != overlay_hash) {
    // Diagnostic only — `legacy_result` is unmodified, the legacy
    // FlexDR path still drives behaviour. Mismatch indicates a
    // projection / filter / box-conversion drift in either side.
    std::cerr << "[drt-redesign-overlay] V2.2.a.1 shadow route-shape "
              << "hash mismatch: legacy=" << std::hex << legacy_hash
              << " overlay=" << overlay_hash << std::dec
              << " layer=" << layer
              << " legacy_count=" << legacy_proj.size()
              << " overlay_count=" << overlay.size() << "\n";
  }
}

void ShadowCompareBlockages(
    const ::drt::frDesign* design,
    const ::odb::Rect& box,
    int layer,
    const std::vector<std::pair<::odb::Rect, ::drt::frBlockObject*>>&
        legacy_result)
{
  if (::drt::redesign::DrtRedesignLeanMode()) return;
  if (design == nullptr) {
    return;
  }

  BlockageQueryResult legacy_proj;
  legacy_proj.reserve(legacy_result.size());
  for (const auto& entry : legacy_result) {
    if (entry.second == nullptr) {
      continue;
    }
    auto projected = ProjectBlockage(
        *entry.second, entry.first, static_cast<LayerNum>(layer));
    if (projected.has_value()) {
      legacy_proj.push_back(*projected);
    }
  }

  RegionQueryGeometryView view(design);
  const Rect overlay_box = ToOverlayRect(box);
  const BlockageQueryResult overlay
      = view.QueryBlockages(overlay_box, static_cast<LayerNum>(layer));

  const uint64_t legacy_hash = HashCanonicalRange(legacy_proj);
  const uint64_t overlay_hash = HashCanonicalRange(overlay);

  ShadowDump::ComparisonRecord rec;
  rec.entity = ShadowDump::Entity::Blockage;
  rec.query_kind = "query";
  rec.box = overlay_box;
  rec.layer = layer;
  rec.legacy_hash = legacy_hash;
  rec.overlay_hash = overlay_hash;
  rec.legacy_count = legacy_proj.size();
  rec.overlay_count = overlay.size();
  ShadowDump::Record(rec);

  if (legacy_hash != overlay_hash) {
    std::cerr << "[drt-redesign-overlay] V2.2.a.3 shadow blockage hash "
              << "mismatch: legacy=" << std::hex << legacy_hash
              << " overlay=" << overlay_hash << std::dec
              << " layer=" << layer
              << " legacy_count=" << legacy_proj.size()
              << " overlay_count=" << overlay.size() << "\n";
  }
}

void ShadowCompareGuides(const ::drt::frDesign* design,
                         const ::odb::Rect& box,
                         const std::vector<::drt::frGuide*>& legacy_result)
{
  if (::drt::redesign::DrtRedesignLeanMode()) return;
  if (design == nullptr) {
    return;
  }

  GuideQueryResult legacy_proj;
  legacy_proj.reserve(legacy_result.size());
  for (const ::drt::frGuide* g : legacy_result) {
    if (g != nullptr) {
      legacy_proj.push_back(ProjectGuide(*g));
    }
  }

  RegionQueryGeometryView view(design);
  const Rect overlay_box = ToOverlayRect(box);
  const GuideQueryResult overlay = view.QueryGuides(overlay_box);

  const uint64_t legacy_hash = HashCanonicalRange(legacy_proj);
  const uint64_t overlay_hash = HashCanonicalRange(overlay);

  ShadowDump::ComparisonRecord rec;
  rec.entity = ShadowDump::Entity::Guide;
  rec.query_kind = "queryGuide";
  rec.box = overlay_box;
  // Layer-less query — leave rec.layer absent.
  rec.legacy_hash = legacy_hash;
  rec.overlay_hash = overlay_hash;
  rec.legacy_count = legacy_proj.size();
  rec.overlay_count = overlay.size();
  ShadowDump::Record(rec);

  if (legacy_hash != overlay_hash) {
    std::cerr << "[drt-redesign-overlay] V2.2.a.2 shadow guide hash "
              << "mismatch: legacy=" << std::hex << legacy_hash
              << " overlay=" << overlay_hash << std::dec
              << " legacy_count=" << legacy_proj.size()
              << " overlay_count=" << overlay.size() << "\n";
  }
}

void ShadowCompareMarkers(
    const ::drt::frDesign* design,
    const ::odb::Rect& box,
    const std::vector<::drt::frMarker*>& legacy_result)
{
  if (::drt::redesign::DrtRedesignLeanMode()) return;
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
