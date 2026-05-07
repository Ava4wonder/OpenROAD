// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.c.bridge — adapter layer between V2's Delta + GeometryView
// abstractions and a legality oracle's candidate-shape input
// vocabulary. This is intentionally a NARROW seam: it produces a
// pointer-free, oracle-shaped batch that any legality backend
// (synthetic, CpuDrcOracle, future GPU oracle) can consume.
//
// V2.2.c.bridge SCOPE — the adapter only. No real legality oracle is
// wired, no rule-deck integration, no actual evaluation. The bridge
// proves the seam:
//
//   Delta → DeltaToOracleInput → OracleCandidateBatch → (legality)
//
// V2.2.c.legality.synthetic wires a tiny synthetic-rule oracle
// against this batch type; V2.2.c.legality.realpdk later replaces
// the synthetic with CpuDrcOracle backed by a real RuleDeck.
//
// Design choices:
//   - Pointer-free everywhere (matches V2.1.c.1 / V2.1.e discipline).
//   - `kind` distinguishes route-shape variants so a synthetic rule
//     deck can apply kind-specific checks (a wire and a via cut are
//     not interchangeable for spacing rules).
//   - `deleted_context` separates "what the delta removes from base"
//     from "what the delta adds." Oracles evaluate ADDED candidates
//     for legality; deleted_context exists so oracles that need to
//     know "what base shape is no longer there" can use it without
//     the bridge re-querying the view.
//   - `Status` reports adapter-level outcomes BEFORE legality
//     evaluation. UnsupportedDelta and UnresolvedFootprint are loud
//     non-committable signals (the eval path translates them to
//     LegalityVerdict.commit_eligible=false).

#pragma once

#include <cstdint>
#include <vector>

#include "../Delta.h"
#include "../Footprint.h"
#include "GeometryView.h"

namespace drt::redesign::overlay {

// One candidate-shape entry, oracle-vocabulary, pointer-free.
struct OracleCandidateShape
{
  enum class Kind : std::uint8_t {
    Wire,           // frPathSeg-style routing rectangle on a layer
    PatchWire,      // frPatchWire-style small patch shape
    ViaCut,         // via cut on the cut layer (frVia derived)
    Shield,         // InsertShield-derived shape
  };
  Kind kind = Kind::Wire;
  Rect bbox{};
  LayerNum layer = 0;
  std::optional<NetId> net_id;
};

struct OracleCandidateBatch
{
  enum class Status : std::uint8_t {
    Ok,
    // V2.2.c.bridge does not handle MoveCell / ChangePinAccess /
    // ChangeLayerAssignment / ResizeCell — those mutate non-shape
    // state in non-trivial ways. Eval must mark such proposals
    // non-committable. Future bridge sub-commits add per-kind
    // adapters when proposers actually emit them.
    UnsupportedDelta,
    // DeleteWire / DeleteVia without resolved identity. The oracle
    // can't reason about a deletion whose target is unknown. Eval
    // must surface UnresolvedFootprint at the LegalityVerdict layer.
    UnresolvedFootprint,
  };

  // Shapes the delta would ADD. The oracle evaluates these for
  // legality against the surrounding context.
  std::vector<OracleCandidateShape> added;
  // Shapes the delta would REMOVE from base. Oracles use this
  // mostly to update the context view (base ∖ deleted) before
  // checking added candidates.
  std::vector<OracleCandidateShape> deleted_context;

  Status status = Status::Ok;
};

// V2.2.c.bridge — Delta → OracleCandidateBatch adapter.
//
// `view` is the base GeometryView the delta would be applied to;
// the bridge does not query it in V2.2.c.bridge but the parameter is
// part of the public API so legality backends can use the same
// signature when they also need contextual queries (V2.2.c.legality
// onward). For V2.2.c.bridge the parameter is reserved.
//
// Returned batch's status field tells the caller whether the delta
// is even adapter-eligible. Eval translates non-Ok status into
// LegalitySource::UnresolvedFootprint or a similar
// non-committable verdict.
OracleCandidateBatch DeltaToOracleInput(const GeometryView& view,
                                        const ProposedDelta& delta);

}  // namespace drt::redesign::overlay
