// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.d — in-memory GeometryView for unit testing.
//
// V2.1 only: read-through view over live FlexDR/FlexGridGraph state.
// Provides API compatibility, not multi-version snapshot isolation.
//
// MemoryBackedGeometryView holds a vector<MarkerRef> in memory and
// answers QueryMarkers from it. Used by redesign_overlay_test to
// exercise the typed-view API without dragging in FlexDR or
// FlexGridGraph. The real region-query-backed view (with frMarker*
// projection) is added in V2.1.e — that's where the
// projection-from-OpenROAD-objects hash test lives.

#pragma once

#include <vector>

#include "GeometryView.h"

namespace drt::redesign::overlay {

class MemoryBackedGeometryView : public GeometryView
{
 public:
  explicit MemoryBackedGeometryView(std::vector<MarkerRef> markers)
      : markers_(std::move(markers))
  {
  }

  std::vector<MarkerRef> QueryMarkers(const Rect& box) const override;

  // V2.2+ methods — throw per GeometryView contract.
  std::vector<ShapeRef> QueryRouteShapes(const Rect& box,
                                         LayerNum layer) const override;
  std::vector<ShapeRef> QueryGuides(const Rect& box) const override;
  std::vector<ShapeRef> QueryBlockages(const Rect& box,
                                       LayerNum layer) const override;

 private:
  std::vector<MarkerRef> markers_;
};

}  // namespace drt::redesign::overlay
