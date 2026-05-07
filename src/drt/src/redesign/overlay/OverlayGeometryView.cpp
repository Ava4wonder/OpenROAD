// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.b — OverlayGeometryView implementation. See header for scope
// and the V2.2.b add-only / V2.2.b.del removal-semantics split.

#include "OverlayGeometryView.h"

#include <type_traits>
#include <variant>

namespace drt::redesign::overlay {

namespace {

bool BboxOverlap(const Rect& a, const Rect& b) noexcept
{
  return !(a.ur.x < b.ll.x || b.ur.x < a.ll.x || a.ur.y < b.ll.y
           || b.ur.y < a.ll.y);
}

// V2.2.b stub via-enclosure halo (matches the V2.1.c default in
// WriteFootprint::Of(AddVia)). V2.2.c will resolve from the actual
// via_def.
constexpr int32_t kDefaultViaEnclosureDbu = 100;

}  // namespace

MarkerQueryResult OverlayGeometryView::QueryMarkers(
    const Rect& box) const
{
  // V2.2.b: no AddMarker / DeleteMarker delta kinds — markers are
  // legality verdicts, produced by GC, never directly added/deleted
  // by a Delta. Pass-through.
  return base_->QueryMarkers(box);
}

ShapeQueryResult OverlayGeometryView::QueryRouteShapes(
    const Rect& box,
    LayerNum layer) const
{
  ShapeQueryResult out = base_->QueryRouteShapes(box, layer);
  for (const Delta& d : deltas_) {
    std::visit(
        [&](const auto& kind) {
          using T = std::decay_t<decltype(kind)>;
          if constexpr (std::is_same_v<T, AddWire>) {
            if (kind.layer != layer) {
              return;
            }
            if (!BboxOverlap(box, kind.bbox)) {
              return;
            }
            ShapeRef s;
            s.bbox = kind.bbox;
            s.layer = kind.layer;
            // net_id intentionally absent in V2.2.b (see header NET-ID
            // LIMITATION). V2.2.b.netid populates it from a pre-
            // resolved field once proposers wire it.
            out.push_back(s);
          } else if constexpr (std::is_same_v<T, AddVia>) {
            // V2.2.b stub: cut layer placeholder 0, location-derived
            // bbox with default enclosure halo (mirrors
            // WriteFootprint::Of(AddVia)). V2.2.c.via resolves real
            // layer from via_def.
            if (layer != 0) {
              return;
            }
            Rect r;
            r.ll.x = kind.location.x - kDefaultViaEnclosureDbu;
            r.ll.y = kind.location.y - kDefaultViaEnclosureDbu;
            r.ur.x = kind.location.x + kDefaultViaEnclosureDbu;
            r.ur.y = kind.location.y + kDefaultViaEnclosureDbu;
            if (!BboxOverlap(box, r)) {
              return;
            }
            ShapeRef s;
            s.bbox = r;
            s.layer = 0;
            out.push_back(s);
          } else if constexpr (std::is_same_v<T, InsertShield>) {
            if (kind.layer != layer) {
              return;
            }
            if (!BboxOverlap(box, kind.coverage)) {
              return;
            }
            ShapeRef s;
            s.bbox = kind.coverage;
            s.layer = kind.layer;
            out.push_back(s);
          }
          // DeleteWire / DeleteVia / MoveCell / ChangePinAccess /
          // ChangeLayerAssignment / ResizeCell: V2.2.b skips these.
          // V2.2.b.del adds DeleteX with resolved bbox+layer; the
          // others are not relevant to QueryRouteShapes anyway
          // (they affect Blockages / PinAccess / different paths).
        },
        d);
  }
  return out;
}

GuideQueryResult OverlayGeometryView::QueryGuides(const Rect& box) const
{
  // V2.2.b: no AddGuide / DeleteGuide delta kinds — guides are
  // GR-produced corridors and not currently mutated by Delta.
  return base_->QueryGuides(box);
}

BlockageQueryResult OverlayGeometryView::QueryBlockages(
    const Rect& box,
    LayerNum layer) const
{
  // V2.2.b: MoveCell / ResizeCell would in principle move
  // frInstBlockage shapes, but resolving the new blockage bbox
  // requires the design-side cell-master geometry. Deferred to
  // V2.2.b.del / V2.2.c. Pass-through for now.
  return base_->QueryBlockages(box, layer);
}

PinAccessQueryResult OverlayGeometryView::QueryPinAccess(
    const Rect& box) const
{
  // V2.2.b: ChangePinAccess delta would alter a PinAccessRef, but
  // pin-access has no live RegionQuery backend yet (V2.2.a.4 B+C
  // hybrid). Composition over a synthetic MemoryBacked base is
  // possible but not exercised in V2.2.b's scope. Pass-through.
  return base_->QueryPinAccess(box);
}

}  // namespace drt::redesign::overlay
