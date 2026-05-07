// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.e — MazeSearchProposer.
//
// V2.2.e SCOPE — minimal synthetic proposer. Given a target net, a
// route box, and a layer, emits a single AddWire ProposedDelta. Does
// NOT invoke OpenROAD's FlexDR maze search yet; V2.2.e.real wires
// the real maze search invocation later, after V2.2.f integrates
// the propose → eval → commit loop into FlexDRWorker.
//
// Why synthetic first: V2.2.e.minimal proves the proposer interface
// + its consumption by try_commit. The actual maze search is
// stateful and orientation-sensitive (see v2 plan §4.2 — pin-access
// proposer / detour proposer / etc. are all proposers in the same
// abstract sense). Validating the seam before wiring real algorithm
// keeps V2.2.e reviewable.
//
// V2.2.e proposer contract:
//   - read_footprint records the (route_box, layer) the proposer
//     queried as its decision context. ProposedDelta carries this
//     forward so V2.4's OCC commit-eligibility check has the
//     read-set per-proposal.
//   - write_footprint comes from WriteFootprint::Of(delta). For
//     AddWire emitted by V2.2.e, this is sound (unknown=false).
//   - id is caller-assigned. Tests use a deterministic DeltaId so
//     ordering across proposers is stable; production proposers
//     will derive from (region, proposer_id, attempt_index) per
//     v2 §3.

#pragma once

#include <cstdint>

#include "../Delta.h"
#include "../Footprint.h"
#include "GeometryView.h"

namespace drt::redesign::overlay {

class MazeSearchProposer
{
 public:
  // Inputs to a single proposal call.
  struct Input
  {
    NetId net_id = 0;
    Rect route_box{};
    LayerNum layer = 0;
    DeltaId delta_id{};        // caller-assigned, stable
    std::uint64_t snapshot_version = 0;  // matches the base snapshot
  };

  // V2.2.e minimal — synthetic propose. Returns a ProposedDelta
  // wrapping an AddWire whose bbox is `in.route_box` on `in.layer`.
  // `base` is queried for read-footprint context (the proposer
  // "looks at" the route box on the target layer; synthetic but
  // architecturally correct so V2.4's OCC check has the right
  // read-set).
  //
  // V2.2.e.real (later): replaces with actual FlexDR maze search
  // invocation that produces a real route path, possibly composed
  // of multiple AddWire / AddVia deltas.
  static ProposedDelta Propose(const GeometryView& base,
                               const Input& in);
};

}  // namespace drt::redesign::overlay
