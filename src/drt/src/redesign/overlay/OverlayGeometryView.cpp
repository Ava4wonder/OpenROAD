// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.b — OverlayGeometryView implementation. See header for scope
// and the V2.2.b add-only / V2.2.b.del removal-semantics split.

#include "OverlayGeometryView.h"

#include <map>
#include <type_traits>
#include <variant>

#include "Hashing.h"
// frBaseTypes.h is a pure types/enum header (not an object-internals
// header like frMarker.h or frDesign.h). Including it here is
// permitted by the redesign/overlay/ include discipline. Used for
// frcVia enum value when synthesising delete-target shape_kind.
#include "frBaseTypes.h"

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

// V2.2.b.del — synthesise a ShapeRef matching what a DeleteWire /
// DeleteVia delta is targeting for removal. Returns nullopt if the
// delta's identity is incomplete (per V2.2.b.del contract: unresolved
// net_id or zero-area bbox is non-committable; OverlayGeometryView
// silently skips such deletes — production validation happens at the
// commit layer via WriteFootprint::unknown).
std::optional<ShapeRef> DeleteTargetShape(const Delta& d)
{
  return std::visit(
      [](const auto& kind) -> std::optional<ShapeRef> {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, DeleteWire>) {
          if (!kind.resolved_net_id.has_value()
              || !kind.shape_kind.has_value()) {
            return std::nullopt;
          }
          if (kind.bbox.ll.x == kind.bbox.ur.x
              && kind.bbox.ll.y == kind.bbox.ur.y) {
            return std::nullopt;
          }
          ShapeRef s;
          s.bbox = kind.bbox;
          s.layer = kind.layer;
          s.net_id = static_cast<NetId>(kind.resolved_net_id.value());
          s.shape_kind = kind.shape_kind.value();
          return s;
        } else if constexpr (std::is_same_v<T, DeleteVia>) {
          if (!kind.resolved_net_id.has_value()) {
            return std::nullopt;
          }
          if (kind.bbox.ll.x == kind.bbox.ur.x
              && kind.bbox.ll.y == kind.bbox.ur.y) {
            return std::nullopt;
          }
          ShapeRef s;
          s.bbox = kind.bbox;
          s.layer = kind.cut_layer;
          s.net_id = static_cast<NetId>(kind.resolved_net_id.value());
          s.shape_kind = static_cast<std::uint8_t>(::drt::frcVia);
          return s;
        }
        return std::nullopt;
      },
      d);
}

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
  ShapeQueryResult base_shapes = base_->QueryRouteShapes(box, layer);

  // V2.2.b.del — multiset subtraction over canonical identity.
  // Build per-tuple delete count from deltas; filter base_shapes by
  // decrementing. Order of base shapes is preserved.
  using Tuple = decltype(CanonicalTuple(std::declval<const ShapeRef&>()));
  std::map<Tuple, int> delete_counts;
  for (const Delta& d : deltas_) {
    auto target = DeleteTargetShape(d);
    if (!target.has_value()) {
      continue;  // unresolved delete: silently skipped (V2.2.b.del
                 // contract — production validation via
                 // WriteFootprint::unknown).
    }
    if (!target->layer.has_value() || target->layer.value() != layer) {
      continue;  // layer-scope mismatch
    }
    if (!BboxOverlap(box, target->bbox)) {
      continue;  // box-scope mismatch
    }
    delete_counts[CanonicalTuple(*target)] += 1;
  }

  ShapeQueryResult out;
  out.reserve(base_shapes.size());
  for (const ShapeRef& b : base_shapes) {
    auto it = delete_counts.find(CanonicalTuple(b));
    if (it != delete_counts.end() && it->second > 0) {
      it->second -= 1;  // consume one base entry
      continue;
    }
    out.push_back(b);
  }
  // Note: any remaining positive counts in delete_counts represent
  // deletes that targeted shapes not present in the base. V2.2.b.del
  // intentionally tolerates this silently (delete=[C] absent from
  // base → no effect, no crash). Diagnostic reporting could land in
  // a follow-up if proposers misbehave often.

  // Now apply additive deltas (existing V2.2.b logic).
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
          // DeleteWire / DeleteVia: handled in V2.2.b.del above by
          // multiset subtraction over canonical ShapeRef identity.
          // MoveCell / ChangePinAccess / ChangeLayerAssignment /
          // ResizeCell: not relevant to QueryRouteShapes (they
          // affect Blockages / PinAccess / different paths).
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
