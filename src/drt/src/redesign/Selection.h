// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.3.b — deterministic selector + rejection taxonomy on top of
// BatchEvalResult. Filters commit_eligible candidates, picks the
// highest-aggregate-score winner, and tags every other proposal with
// a RejectionReason so the shadow dump (V2.3.c) and (eventually) the
// compatible-subset commit (V2.4) can record *why* a proposal lost.
//
// V2.3.b SCOPE — architecture validation only. There is no
// learned ranking, no congestion/timing-driven scoring, no
// cross-worker MIS, no real conflict graph. Conflict is reserved
// in the enum because the selector is the natural place where a
// future V2.4 pass would tag conflict-loser rejections.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "BatchEval.h"
#include "Footprint.h"
#include "Score.h"

namespace drt::redesign {

// Why a proposal did not become the winner of a batch.
//
// V2.3.b classifies all non-winners. The taxonomy is deliberately a
// flat enum (not a sum type) because (a) the shadow dump will write
// a single column, and (b) the V2.4 conflict graph wants to filter
// "Conflict losers" without unpacking a variant. Add new reasons
// only when the selector itself can produce them; do not abuse the
// enum to encode arbitrary error states.
enum class RejectionReason : std::uint8_t {
  // Sentinel — used in tests / dumps to indicate a proposal was the
  // winner (and is therefore not in `rejected`). Never appears in
  // SelectionResult::rejected.
  None,
  // Verdict said legal=false from a real oracle (SyntheticOracle /
  // CpuDrcOracle / UpstreamExact).
  Illegal,
  // Adapter / eval could not validate this proposal — DeleteWire
  // without resolved identity, zero-area bbox, etc. The verdict
  // source is LegalitySource::UnresolvedFootprint.
  UnresolvedFootprint,
  // The Delta variant is one the adapter does not yet support
  // (MoveCell, ChangeLayerAssignment, etc.). Verdict source
  // collapses to UnresolvedFootprint, but the parallel
  // BatchEvalResult::adapter_unsupported flag distinguishes them.
  UnsupportedDelta,
  // The proposal was commit_eligible but lost the tournament to a
  // higher-aggregate-score sibling. Tie-break: lowest DeltaId wins.
  LowerScore,
  // V2.3.b reserved for V2.4 — when the compatible-subset commit
  // path drops a proposal because it conflicts with another
  // already-selected proposal in the same batch. The selector in
  // V2.3.b never emits this; the enum slot exists so call sites
  // (especially the shadow dump column) are stable across V2.3.b →
  // V2.4 without enum renumbering.
  Conflict,
  // Catch-all for non-eligible outcomes that did not match any of
  // the above. Today this is the StubAssumeLegal-source bucket
  // (legal=true but commit_eligible=false because the source is
  // untrusted). Should NEVER apply to a verdict produced by a
  // real oracle.
  Unknown,
};

struct RejectedProposal
{
  DeltaId id;
  RejectionReason reason = RejectionReason::Unknown;
};

struct SelectionResult
{
  // True iff at least one commit_eligible proposal was found and
  // selected as the winner. When false, `winner_id` and
  // `winner_score` are not meaningful.
  bool has_winner = false;
  DeltaId winner_id{};
  std::optional<Score> winner_score;

  // Every non-winner proposal in the batch, in DeltaId-sorted order
  // (BatchEvalResult is already canonically sorted). The reason
  // field uses RejectionReason::None ONLY as a sentinel — it never
  // appears here.
  std::vector<RejectedProposal> rejected;
};

// Pick the single best commit_eligible proposal from a
// BatchEvalResult and tag the rest with rejection reasons.
//
// Determinism contract:
//   * BatchEvalResult must be DeltaId-sorted (the contract of
//     PhysicalState::batch_eval).
//   * Among commit_eligible proposals with equal aggregate score,
//     the lowest DeltaId wins. Because the input is sorted, this is
//     equivalent to "first commit_eligible with the max score wins."
//   * The `rejected` vector preserves DeltaId order.
//
// V2.3.b never emits RejectionReason::Conflict — that slot is
// reserved for V2.4. If the input is empty, has_winner is false and
// rejected is empty.
SelectionResult SelectBest(const BatchEvalResult& batch);

}  // namespace drt::redesign
