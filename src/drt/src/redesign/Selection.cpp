// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.3.b — implementation of SelectBest.

#include "Selection.h"

#include <cstddef>
#include <limits>
#include <optional>

#include "ConflictGraph.h"
#include "ConflictPolicy.h"
#include "EvalOutcome.h"
#include "GreedyPriorityPolicy.h"  // MakeEdgeList
#include "LegalityVerdict.h"

namespace drt::redesign {

namespace {

// Map a non-commit-eligible outcome to a rejection reason.
//
// Inputs guaranteed by caller: outcome.legality.commit_eligible ==
// false. The classification rules:
//
//   adapter_unsupported            → UnsupportedDelta
//   source == UnresolvedFootprint  → UnresolvedFootprint
//   legal == false (any other source, i.e. real oracle)
//                                  → Illegal
//   legal == true but untrusted source (StubAssumeLegal, etc.)
//                                  → Unknown
//
// The order matters: UnsupportedDelta wins over UnresolvedFootprint
// because the adapter flag is the more specific signal.
RejectionReason ClassifyNonEligible(const EvalOutcome& outcome,
                                    bool adapter_unsupported) noexcept
{
  if (adapter_unsupported) {
    return RejectionReason::UnsupportedDelta;
  }
  if (outcome.legality.source == LegalitySource::UnresolvedFootprint) {
    return RejectionReason::UnresolvedFootprint;
  }
  if (!outcome.legality.legal) {
    return RejectionReason::Illegal;
  }
  return RejectionReason::Unknown;
}

}  // namespace

SelectionResult SelectBest(const BatchEvalResult& batch)
{
  SelectionResult out;

  const std::size_t n = batch.outcomes.size();
  // Defensive: BatchEvalResult is constructed with parallel vectors
  // by PhysicalState::batch_eval. If any are out of sync we still
  // produce a SelectionResult — the safest behaviour is to refuse a
  // winner and tag every present proposal as Unknown. This should
  // never happen in practice and would indicate a programming error
  // upstream.
  if (batch.outcome_proposal_ids.size() != n
      || batch.adapter_unsupported.size() != n) {
    for (std::size_t i = 0; i < batch.outcome_proposal_ids.size(); ++i) {
      out.rejected.push_back(
          {batch.outcome_proposal_ids[i], RejectionReason::Unknown});
    }
    return out;
  }

  // Pass 1 — find the best commit_eligible proposal.
  // Tie-break: input is DeltaId-sorted (PhysicalState::batch_eval
  // contract), and we keep the first max-score winner, so ties
  // resolve to the lowest DeltaId.
  std::optional<std::size_t> best_idx;
  for (std::size_t i = 0; i < n; ++i) {
    const auto& outcome = batch.outcomes[i];
    if (!outcome.legality.commit_eligible) {
      continue;
    }
    if (!outcome.score.has_value()) {
      // commit_eligible should imply a score is present by the
      // EvalOutcome contract — but if it isn't, we cannot rank, so
      // skip it. It will be tagged Unknown in pass 2.
      continue;
    }
    if (!best_idx.has_value()) {
      best_idx = i;
      continue;
    }
    const double cur = batch.outcomes[*best_idx].score->aggregate;
    const double cand = outcome.score->aggregate;
    if (cand > cur) {
      best_idx = i;
    }
    // cand == cur: keep the existing (lower-DeltaId) winner.
  }

  if (best_idx.has_value()) {
    out.has_winner = true;
    out.winner_id = batch.outcome_proposal_ids[*best_idx];
    out.winner_score = batch.outcomes[*best_idx].score;
  }

  // Pass 2 — tag every non-winner.
  out.rejected.reserve(best_idx.has_value() ? n - 1 : n);
  for (std::size_t i = 0; i < n; ++i) {
    if (best_idx.has_value() && i == *best_idx) {
      continue;
    }
    RejectionReason reason;
    const auto& outcome = batch.outcomes[i];
    if (outcome.legality.commit_eligible) {
      // The proposal could have committed — but it lost the
      // score tournament (or had no score). Tag LowerScore for
      // the score-bearing case; if score is somehow missing,
      // tag Unknown so log readers notice the anomaly.
      reason = outcome.score.has_value() ? RejectionReason::LowerScore
                                         : RejectionReason::Unknown;
    } else {
      reason = ClassifyNonEligible(outcome, batch.adapter_unsupported[i]);
    }
    out.rejected.push_back({batch.outcome_proposal_ids[i], reason});
  }

  return out;
}

MisSelectionResult SelectMis(const BatchEvalResult& batch,
                             const ProposalSet& set,
                             const ConflictGraph& graph,
                             ConflictPolicy& policy)
{
  MisSelectionResult out;

  const std::size_t n = batch.outcomes.size();
  if (n == 0) {
    return out;
  }

  // Defensive parallel-vector check (matches SelectBest pattern).
  // If any of the parallel vectors are out of sync, refuse to
  // select anything and tag everything Unknown — this is a
  // programming-error-detection path that should never trip in
  // practice.
  if (batch.outcome_proposal_ids.size() != n
      || batch.adapter_unsupported.size() != n
      || set.proposals.size() != n
      || graph.node_ids.size() != n) {
    for (std::size_t i = 0; i < batch.outcome_proposal_ids.size(); ++i) {
      out.rejected.push_back(
          {batch.outcome_proposal_ids[i], RejectionReason::Unknown});
    }
    return out;
  }

  // Build the ScoredProposal vector for the policy. Non-eligible
  // proposals get aggregate=-infinity so the policy never picks
  // them; the post-filter catches any edge cases.
  std::vector<ScoredProposal> scored;
  scored.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    ScoredProposal sp;
    sp.proposal = set.proposals[i];
    sp.legality = batch.outcomes[i].legality;
    if (batch.outcomes[i].score.has_value()) {
      sp.score = *batch.outcomes[i].score;
    }
    if (!batch.outcomes[i].legality.commit_eligible) {
      sp.score.aggregate = -std::numeric_limits<double>::infinity();
    }
    scored.push_back(sp);
  }

  // Run the policy with the ConflictGraph flattened to pair edges.
  const auto edges = MakeEdgeList(graph);
  const auto selected_indices = policy.select(scored, edges);

  // Membership lookup for the post-filter walk.
  std::vector<bool> in_selected(n, false);
  for (std::size_t s : selected_indices) {
    if (s < n) {
      in_selected[s] = true;
    }
  }

  out.selected_ids.reserve(selected_indices.size());
  out.rejected.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const bool eligible = batch.outcomes[i].legality.commit_eligible;
    if (in_selected[i] && eligible) {
      out.selected_ids.push_back(batch.outcome_proposal_ids[i]);
      continue;
    }

    RejectionReason reason;
    if (!eligible) {
      reason = ClassifyNonEligible(batch.outcomes[i],
                                   batch.adapter_unsupported[i]);
    } else {
      // Eligible but lost MIS — emit Conflict, the V2.3.b enum
      // slot reserved exactly for this.
      reason = RejectionReason::Conflict;
    }
    out.rejected.push_back({batch.outcome_proposal_ids[i], reason});
  }

  return out;
}

}  // namespace drt::redesign
