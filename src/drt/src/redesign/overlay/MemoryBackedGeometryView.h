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

  // V2.2.a.1.mem — overload accepting both backing entity vectors.
  MemoryBackedGeometryView(std::vector<MarkerRef> markers,
                           std::vector<ShapeRef> route_shapes)
      : markers_(std::move(markers)),
        route_shapes_(std::move(route_shapes))
  {
  }

  // V2.2.a.2 — overload also accepting guides.
  MemoryBackedGeometryView(std::vector<MarkerRef> markers,
                           std::vector<ShapeRef> route_shapes,
                           std::vector<GuideRef> guides)
      : markers_(std::move(markers)),
        route_shapes_(std::move(route_shapes)),
        guides_(std::move(guides))
  {
  }

  // V2.2.a.3 — overload also accepting blockages.
  MemoryBackedGeometryView(std::vector<MarkerRef> markers,
                           std::vector<ShapeRef> route_shapes,
                           std::vector<GuideRef> guides,
                           std::vector<BlockageRef> blockages)
      : markers_(std::move(markers)),
        route_shapes_(std::move(route_shapes)),
        guides_(std::move(guides)),
        blockages_(std::move(blockages))
  {
  }

  MarkerQueryResult QueryMarkers(const Rect& box) const override;

  // V2.2.a.1.mem — real implementation. Filters route_shapes_ by
  // bbox overlap AND layer match (treating absent ShapeRef.layer as
  // "matches any" for synthetic-data convenience).
  ShapeQueryResult QueryRouteShapes(const Rect& box,
                                    LayerNum layer) const override;

  // V2.2.a.2 — real implementation. Filters guides_ by bbox overlap.
  // The guide query is layer-less (matching frRegionQuery's unlayered
  // queryGuide overload).
  GuideQueryResult QueryGuides(const Rect& box) const override;

  // V2.2.a.3 — real implementation. Filters blockages_ by bbox
  // overlap AND layer match (absent BlockageRef.layer = "any layer"
  // for synthetic data convenience).
  BlockageQueryResult QueryBlockages(const Rect& box,
                                     LayerNum layer) const override;

  // V2.2+ methods — still throw per GeometryView contract until
  // their per-entity sub-commit (V2.2.a.4) lands.
  PinAccessQueryResult QueryPinAccess(const Rect& box) const override;

 private:
  std::vector<MarkerRef> markers_;
  std::vector<ShapeRef> route_shapes_;
  std::vector<GuideRef> guides_;
  std::vector<BlockageRef> blockages_;
};

}  // namespace drt::redesign::overlay
