// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.d — MemoryBackedGeometryView implementation. See header for
// scope.

#include "MemoryBackedGeometryView.h"

#include <stdexcept>

namespace drt::redesign::overlay {

namespace {

bool BboxOverlap(const Rect& a, const Rect& b) noexcept
{
  return !(a.ur.x < b.ll.x || b.ur.x < a.ll.x || a.ur.y < b.ll.y
           || b.ur.y < a.ll.y);
}

}  // namespace

std::vector<MarkerRef> MemoryBackedGeometryView::QueryMarkers(
    const Rect& box) const
{
  std::vector<MarkerRef> out;
  for (const MarkerRef& m : markers_) {
    if (BboxOverlap(box, m.bbox)) {
      out.push_back(m);
    }
  }
  return out;
}

std::vector<ShapeRef> MemoryBackedGeometryView::QueryRouteShapes(
    const Rect& box,
    LayerNum layer) const
{
  (void) box;
  (void) layer;
  throw std::logic_error(
      "GeometryView: QueryRouteShapes not supported in V2.1");
}

std::vector<ShapeRef> MemoryBackedGeometryView::QueryGuides(
    const Rect& box) const
{
  (void) box;
  throw std::logic_error(
      "GeometryView: QueryGuides not supported in V2.1");
}

std::vector<ShapeRef> MemoryBackedGeometryView::QueryBlockages(
    const Rect& box,
    LayerNum layer) const
{
  (void) box;
  (void) layer;
  throw std::logic_error(
      "GeometryView: QueryBlockages not supported in V2.1");
}

}  // namespace drt::redesign::overlay
