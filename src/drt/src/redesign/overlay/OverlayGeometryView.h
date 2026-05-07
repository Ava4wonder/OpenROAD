// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.b — `OverlayGeometryView`, the hinge primitive of the V2
// redesign per v2_drt_redesign_plan.md §2.5.
//
// OverlayGeometryView composes a base GeometryView with a list of
// Deltas and answers queries as if the Deltas were applied:
//
//     base ∖ deleted ∪ added
//
// This is the abstraction that makes multi-candidate evaluation
// possible: a worker can construct K different OverlayGeometryViews
// over the same base with different Delta lists, and each scores
// against an independent hypothetical state without mutating
// anything. V2.3 (best-of-K) and V2.4 (cross-worker MIS commit)
// both consume this primitive.
//
// Composability: because OverlayGeometryView IS a GeometryView, an
// OverlayGeometryView can BE the base for another OverlayGeometryView.
// Stacking multiple Delta groups is a valid pattern (overlay over
// overlay over base). V2 won't depend on stacking, but the
// architecture supports it for free.
//
// V2.2.b SCOPE — Add-only composition:
//   - AddWire / AddVia / InsertShield: appended to QueryRouteShapes
//     results when their bbox/layer match the query.
//   - All Delete* delta kinds: ignored in V2.2.b. V2.2.b.del adds
//     removal semantics (requires extending DeleteWire/DeleteVia
//     with explicit bbox+layer+net so OverlayGeometryView can match
//     against base entries without a design-side lookup).
//   - MoveCell / ChangePinAccess / ChangeLayerAssignment / ResizeCell:
//     ignored — these affect QueryBlockages / QueryPinAccess in non-
//     trivial ways and land alongside V2.2.b.del.
//
// V2.2.b NET-ID LIMITATION:
//   AddWire / AddVia / InsertShield carry frNet*, but resolving
//   frNet*->getId() inside OverlayGeometryView would require including
//   frNet.h (breaking the include discipline that only
//   RegionQueryGeometryView.cpp may include OpenROAD internals).
//   V2.2.b therefore produces overlay shapes with net_id=nullopt.
//   Future producers (V2.2.c+ MazeSearchProposer) will pre-resolve
//   the NetId at proposal time when consumers need it.

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "../Delta.h"
#include "GeometryView.h"

namespace drt::redesign::overlay {

class OverlayGeometryView final : public GeometryView
{
 public:
  OverlayGeometryView(std::shared_ptr<const GeometryView> base,
                      std::vector<Delta> deltas)
      : base_(std::move(base)), deltas_(std::move(deltas))
  {
  }

  // V2.2.b query methods. Each calls the base view, then composes
  // with the additive Delta kinds that match the query parameters.
  MarkerQueryResult QueryMarkers(const Rect& box) const override;
  ShapeQueryResult QueryRouteShapes(const Rect& box,
                                    LayerNum layer) const override;
  GuideQueryResult QueryGuides(const Rect& box) const override;
  BlockageQueryResult QueryBlockages(const Rect& box,
                                     LayerNum layer) const override;
  PinAccessQueryResult QueryPinAccess(const Rect& box) const override;

 private:
  std::shared_ptr<const GeometryView> base_;
  std::vector<Delta> deltas_;
};

}  // namespace drt::redesign::overlay
