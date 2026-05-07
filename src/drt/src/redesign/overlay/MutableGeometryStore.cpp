// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.d — MutableGeometryStore implementation.

#include "MutableGeometryStore.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "frBaseTypes.h"  // frcVia enum value (used for via shape_kind)

namespace drt::redesign::overlay {

namespace {

bool BboxOverlap(const Rect& a, const Rect& b) noexcept
{
  return !(a.ur.x < b.ll.x || b.ur.x < a.ll.x || a.ur.y < b.ll.y
           || b.ur.y < a.ll.y);
}

constexpr std::int32_t kDefaultViaEnclosureDbu = 100;

// V2.2.d delete-by-identity match: same as OverlayGeometryView's
// canonical-tuple match but local to the mutable store. Returns
// the iterator to the first matching entry, or end() if none.
std::vector<ShapeRef>::iterator FindFirstMatch(
    std::vector<ShapeRef>& shapes,
    const ShapeRef& target)
{
  return std::find_if(
      shapes.begin(), shapes.end(),
      [&target](const ShapeRef& s) {
        return s.layer == target.layer && s.bbox.ll.x == target.bbox.ll.x
               && s.bbox.ll.y == target.bbox.ll.y
               && s.bbox.ur.x == target.bbox.ur.x
               && s.bbox.ur.y == target.bbox.ur.y
               && s.net_id == target.net_id
               && s.shape_kind == target.shape_kind;
      });
}

}  // namespace

bool MutableGeometryStore::Apply(const Delta& d)
{
  return std::visit(
      [&](const auto& kind) -> bool {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, AddWire>) {
          ShapeRef s;
          s.bbox = kind.bbox;
          s.layer = kind.layer;
          // V2.2.b NET-ID LIMITATION: net_id absent (frNet*->getId()
          // requires include discipline violation). V2.2.d test
          // base shapes can carry net_id directly via SeedRouteShapes.
          route_shapes_.push_back(s);
          return true;
        } else if constexpr (std::is_same_v<T, AddVia>) {
          ShapeRef s;
          s.bbox.ll.x = kind.location.x - kDefaultViaEnclosureDbu;
          s.bbox.ll.y = kind.location.y - kDefaultViaEnclosureDbu;
          s.bbox.ur.x = kind.location.x + kDefaultViaEnclosureDbu;
          s.bbox.ur.y = kind.location.y + kDefaultViaEnclosureDbu;
          s.layer = 0;  // V2.2.b stub layer; V2.2.f resolves via_def
          s.shape_kind
              = static_cast<std::uint8_t>(::drt::frcVia);
          route_shapes_.push_back(s);
          return true;
        } else if constexpr (std::is_same_v<T, InsertShield>) {
          ShapeRef s;
          s.bbox = kind.coverage;
          s.layer = kind.layer;
          route_shapes_.push_back(s);
          return true;
        } else if constexpr (std::is_same_v<T, DeleteWire>) {
          if (!kind.resolved_net_id.has_value()
              || !kind.shape_kind.has_value()) {
            return false;  // unresolved — caller should have rejected
          }
          ShapeRef target;
          target.bbox = kind.bbox;
          target.layer = kind.layer;
          target.net_id
              = static_cast<NetId>(kind.resolved_net_id.value());
          target.shape_kind = kind.shape_kind.value();
          auto it = FindFirstMatch(route_shapes_, target);
          if (it != route_shapes_.end()) {
            route_shapes_.erase(it);
          }
          // V2.2.d: a Delete that matches no base entry is a
          // committed no-op (consistent with OverlayGeometryView's
          // tolerance of absent deletes). Return true because the
          // delta itself was processed.
          return true;
        } else if constexpr (std::is_same_v<T, DeleteVia>) {
          if (!kind.resolved_net_id.has_value()) {
            return false;
          }
          ShapeRef target;
          target.bbox = kind.bbox;
          target.layer = kind.cut_layer;
          target.net_id
              = static_cast<NetId>(kind.resolved_net_id.value());
          target.shape_kind
              = static_cast<std::uint8_t>(::drt::frcVia);
          auto it = FindFirstMatch(route_shapes_, target);
          if (it != route_shapes_.end()) {
            route_shapes_.erase(it);
          }
          return true;
        }
        // MoveCell / ChangePinAccess / ChangeLayerAssignment /
        // ResizeCell: V2.2.d does not apply these. Eval should
        // have produced a non-committable verdict; reaching this
        // branch is a programming error in the commit path.
        return false;
      },
      d);
}

MarkerQueryResult MutableGeometryStore::QueryMarkers(
    const Rect& box) const
{
  MarkerQueryResult out;
  for (const MarkerRef& m : markers_) {
    if (BboxOverlap(box, m.bbox)) {
      out.push_back(m);
    }
  }
  return out;
}

ShapeQueryResult MutableGeometryStore::QueryRouteShapes(
    const Rect& box,
    LayerNum layer) const
{
  ShapeQueryResult out;
  for (const ShapeRef& s : route_shapes_) {
    if (!BboxOverlap(box, s.bbox)) {
      continue;
    }
    if (s.layer.has_value() && s.layer.value() != layer) {
      continue;
    }
    out.push_back(s);
  }
  return out;
}

GuideQueryResult MutableGeometryStore::QueryGuides(
    const Rect& box) const
{
  GuideQueryResult out;
  for (const GuideRef& g : guides_) {
    if (BboxOverlap(box, g.bbox)) {
      out.push_back(g);
    }
  }
  return out;
}

BlockageQueryResult MutableGeometryStore::QueryBlockages(
    const Rect& box,
    LayerNum layer) const
{
  BlockageQueryResult out;
  for (const BlockageRef& b : blockages_) {
    if (!BboxOverlap(box, b.bbox)) {
      continue;
    }
    if (b.layer.has_value() && b.layer.value() != layer) {
      continue;
    }
    out.push_back(b);
  }
  return out;
}

PinAccessQueryResult MutableGeometryStore::QueryPinAccess(
    const Rect& box) const
{
  PinAccessQueryResult out;
  for (const PinAccessRef& p : pin_access_) {
    if (BboxOverlap(box, p.bbox)) {
      out.push_back(p);
    }
  }
  return out;
}

}  // namespace drt::redesign::overlay
