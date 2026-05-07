// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.e — MazeSearchProposer minimal/synthetic implementation.
// See header for the why-synthetic-first staging.

#include "MazeSearchProposer.h"

namespace drt::redesign::overlay {

ProposedDelta MazeSearchProposer::Propose(const GeometryView& base,
                                          const Input& in)
{
  // V2.2.e read-footprint capture. Even the synthetic proposer
  // records WHAT it queried so V2.4's OCC commit-eligibility check
  // can intersect against committed-write footprints. The actual
  // returned shapes don't matter for the footprint — only the
  // queried region does.
  (void) base.QueryRouteShapes(in.route_box, in.layer);

  // Construct the synthetic AddWire. V2.2.e leaves frNet* nullptr
  // and width=0 — V2.2.e.real populates these from the real
  // FlexDR net handle and DBU width.
  AddWire add;
  add.bbox = in.route_box;
  add.layer = in.layer;
  add.net = nullptr;

  ProposedDelta pd;
  pd.delta = add;
  pd.source = DeltaSource::DetailedRoutePatch;
  pd.worker_id = -1;  // unbound in V2.2.e minimal
  pd.snapshot_version = in.snapshot_version;
  pd.id = in.delta_id;

  // Read footprint: the (route_box, layer) the proposer "read."
  pd.read_footprint.geometry.rects.push_back(in.route_box);
  pd.read_footprint.geometry.layers.push_back(in.layer);
  // Topology / markers / guides / cost-fields / pin-access not
  // populated in V2.2.e — the synthetic proposer doesn't consult
  // those domains. V2.2.e.real expands the read footprint to match
  // what real maze search actually queries.

  // Write footprint: derive from the Delta. For AddWire this is
  // sound (unknown=false).
  pd.write_footprint = WriteFootprint::Of(pd.delta);

  return pd;
}

}  // namespace drt::redesign::overlay
