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
#include <vector>

#include "../Delta.h"
#include "../Footprint.h"
#include "GeometryView.h"

namespace drt::redesign::overlay {

// V2.2.e.real — one captured shape from upstream's drNet routes.
// The caller (V2.6.b's FlexDR_maze.cpp wiring) walks
// drNet::getRouteConnFigs() and projects each drConnFig into one
// of these. Keeping CapturedConnFig free of upstream types
// (drPathSeg / drVia / drPatchWire) lets the proposer be
// unit-tested without dragging in the drt namespace.
struct CapturedConnFig
{
  enum class Kind : std::uint8_t {
    PathSeg = 0,    // drPathSeg → AddWire
    Via = 1,        // drVia → AddVia
    PatchWire = 2,  // drPatchWire → InsertShield (nearest V2 analog
                    // — V2 has no first-class patch-wire variant
                    // today; InsertShield carries a coverage rect +
                    // layer which is enough to round-trip the
                    // upstream geometry).
  };
  Kind kind;
  Rect bbox{};
  LayerNum layer = 0;
  // Via origin point. Ignored for PathSeg / PatchWire. Maps to
  // drVia::getOrigin() on the wiring side.
  Point via_origin{};
};

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
  // V2.2.e.real lives in ProposeFromCaptured below; this synthetic
  // overload is preserved for tests and for the V2.3.c top-of-
  // routeNet shadow path.
  static ProposedDelta Propose(const GeometryView& base,
                               const Input& in);

  // V2.2.e.real — emit one ProposedDelta per CapturedConnFig.
  //
  // Mapping:
  //   Kind::PathSeg   → AddWire(bbox, layer)
  //   Kind::Via       → AddVia(via_origin)        (via_def=nullptr)
  //   Kind::PatchWire → InsertShield(bbox, layer)
  //
  // DeltaId discipline: each emitted Delta gets
  //   DeltaId{ in.delta_id.region_id,
  //            in.delta_id.proposer_id,
  //            attempt_index = i }
  // where i is the captured-shape index. proposal_net_id is
  // copied from in.net_id; that lets V2.4.b's Net-conflict
  // detector group all of these as one logical net.
  //
  // Read-footprint discipline: each emitted Delta records its
  // own bbox + layer in the geometry domain (matches
  // CpuDrcOracleRealDeck's expectations from V2.5.b). The
  // route_box is also queried via `base` for OCC context, mirroring
  // synthetic Propose.
  //
  // SCOPE — pure conversion. The drNet → CapturedConnFig
  // extraction (which needs upstream drNet headers) is the
  // caller's job; lives in V2.6.b's FlexDR_maze.cpp wiring.
  static std::vector<ProposedDelta> ProposeFromCaptured(
      const GeometryView& base,
      const std::vector<CapturedConnFig>& captured,
      const Input& in);
};

}  // namespace drt::redesign::overlay
