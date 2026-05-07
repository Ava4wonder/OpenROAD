// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.d — writable in-memory backing for PhysicalState. Implements
// GeometryView for read paths (eval/scoring) and adds an `Apply`
// method that mutates the backing vectors in response to a
// committed Delta.
//
// V2.2.d SCOPE: synthetic / PoC backing only. The production
// integration with frBlock/frRegionQuery (where a real commit also
// updates upstream OpenROAD state) lands in V2.2.f when
// FlexDRWorker::route_queue is wired. V2.2.d ships the loop:
//
//   propose → eval → commit → store mutation → version bump
//
// against MutableGeometryStore as the writable medium, so the V2
// PoC can prove the loop end-to-end without waiting on upstream
// integration.
//
// Thread-safety: PhysicalState's commit mutex serialises Apply
// calls. QueryX methods are const and may run concurrently with
// each other (read-only) but NOT concurrently with Apply.

#pragma once

#include <vector>

#include "GeometryView.h"

namespace drt::redesign::overlay {

class MutableGeometryStore final : public GeometryView
{
 public:
  // V2.2.d apply path. Returns true if the delta was applied.
  // Returns false for unresolved-identity deletes and for
  // V2.2.d-unsupported kinds (MoveCell / ChangePinAccess /
  // ChangeLayerAssignment / ResizeCell). The eval/commit caller
  // SHOULD have rejected those before calling Apply, so a false
  // return here indicates a programming error in the commit path.
  bool Apply(const Delta& d);

  // Test seeds. Production code uses Apply.
  void SeedMarkers(std::vector<MarkerRef> markers)
  {
    markers_ = std::move(markers);
  }
  void SeedRouteShapes(std::vector<ShapeRef> shapes)
  {
    route_shapes_ = std::move(shapes);
  }
  void SeedGuides(std::vector<GuideRef> guides)
  {
    guides_ = std::move(guides);
  }
  void SeedBlockages(std::vector<BlockageRef> blockages)
  {
    blockages_ = std::move(blockages);
  }
  void SeedPinAccess(std::vector<PinAccessRef> pa)
  {
    pin_access_ = std::move(pa);
  }

  // Direct read accessors so PhysicalState tests can verify state
  // after a commit without going through the QueryX API.
  const std::vector<ShapeRef>& route_shapes() const noexcept
  {
    return route_shapes_;
  }
  std::size_t route_shape_count() const noexcept
  {
    return route_shapes_.size();
  }

  // GeometryView API (delegates to bbox-overlap filters over the
  // backing vectors, identical logic to MemoryBackedGeometryView).
  MarkerQueryResult QueryMarkers(const Rect& box) const override;
  ShapeQueryResult QueryRouteShapes(const Rect& box,
                                    LayerNum layer) const override;
  GuideQueryResult QueryGuides(const Rect& box) const override;
  BlockageQueryResult QueryBlockages(const Rect& box,
                                     LayerNum layer) const override;
  PinAccessQueryResult QueryPinAccess(const Rect& box) const override;

 private:
  std::vector<MarkerRef> markers_;
  std::vector<ShapeRef> route_shapes_;
  std::vector<GuideRef> guides_;
  std::vector<BlockageRef> blockages_;
  std::vector<PinAccessRef> pin_access_;
};

}  // namespace drt::redesign::overlay
