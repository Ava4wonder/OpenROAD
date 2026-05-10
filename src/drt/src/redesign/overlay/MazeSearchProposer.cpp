// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.e — MazeSearchProposer minimal/synthetic implementation +
// V2.2.e.real — captured-route conversion (no maze search; pure
// upstream-route → Delta-sequence projection).

#include "MazeSearchProposer.h"

#include <utility>

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
  // V2.4.b — propagate the net identity to the canonical
  // proposal_net_id field so the conflict graph can detect
  // same-net writes. Input.net_id was previously dropped.
  pd.proposal_net_id = in.net_id;

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

std::vector<ProposedDelta> MazeSearchProposer::ProposeFromCaptured(
    const GeometryView& base,
    const std::vector<CapturedConnFig>& captured,
    const Input& in)
{
  // Same OCC read context as the synthetic Propose: the
  // proposer "looked at" the route box on the target layer to
  // produce these shapes. The captured shapes' own bboxes go in
  // each Delta's per-shape read footprint below.
  (void) base.QueryRouteShapes(in.route_box, in.layer);

  std::vector<ProposedDelta> out;
  out.reserve(captured.size());

  std::uint32_t attempt = 0;
  for (const auto& cap : captured) {
    ProposedDelta pd;
    pd.source = DeltaSource::DetailedRoutePatch;
    pd.worker_id = -1;  // unbound; V2.6.b's wiring may set a real id
    pd.snapshot_version = in.snapshot_version;
    pd.id.region_id = in.delta_id.region_id;
    pd.id.proposer_id = in.delta_id.proposer_id;
    pd.id.attempt_index = attempt++;
    // V2.4.b — propagate net id so Net-conflict detection treats
    // every captured shape of this net as one logical group.
    pd.proposal_net_id = in.net_id;

    switch (cap.kind) {
      case CapturedConnFig::Kind::PathSeg: {
        AddWire add;
        add.bbox = cap.bbox;
        add.layer = cap.layer;
        add.net = nullptr;
        add.width = 0;  // V2.6.b's wiring may populate width from
                        // upstream's drPathSeg endStyle / track
                        // width; V2.2.e.real leaves it unset.
        pd.delta = add;
        break;
      }
      case CapturedConnFig::Kind::Via: {
        AddVia av;
        av.location = cap.via_origin;
        av.via_def = nullptr;  // V2.6.b's wiring may populate
        av.net = nullptr;
        pd.delta = av;
        break;
      }
      case CapturedConnFig::Kind::PatchWire: {
        // Patch wires are partial-coverage shapes used to fix
        // min-area violations. Map to InsertShield (V2's nearest
        // analog with a coverage rect + layer).
        InsertShield is;
        is.coverage = cap.bbox;
        is.layer = cap.layer;
        is.shield_net = nullptr;
        pd.delta = is;
        break;
      }
    }

    // Per-shape read footprint: this shape's bbox + layer. Lets
    // V2.5.b's CpuDrcOracleRealDeck dispatch query the right
    // neighborhood when it re-evals captured shapes.
    pd.read_footprint.geometry.rects.push_back(cap.bbox);
    pd.read_footprint.geometry.layers.push_back(cap.layer);

    pd.write_footprint = WriteFootprint::Of(pd.delta);

    out.push_back(std::move(pd));
  }

  return out;
}

}  // namespace drt::redesign::overlay
