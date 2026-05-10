// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.c — GreedyPriorityPolicy. Concrete ConflictPolicy that picks a
// maximal independent set greedily by score: walk proposals in
// descending score order, ties broken by ascending DeltaId, and
// admit each proposal if it has no edge to any already-selected one.
//
// Deterministic by construction (the sort uses score then DeltaId,
// both stable). Same inputs → byte-identical selection. This is the
// V2.4.c implementation of the determinism contract from §7 of
// v2_drt_redesign_plan.md, and it satisfies R3 (MIS solver
// non-determinism) by avoiding randomized tie-breaks.
//
// V2.4.c SCOPE — solver only. The multi-Delta commit path that
// consumes this policy is V2.4.d; the cross-worker barrier that
// stages proposals across workers is V2.4.e.
//
// Future siblings (per §4.5): GraphColoringPolicy,
// LagrangianRelaxationPolicy, LearnedRankingPolicy,
// MaximalIndependentSetPolicy. None are scoped here.

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "ConflictPolicy.h"

namespace drt::redesign {

class GreedyPriorityPolicy : public ConflictPolicy
{
 public:
  // Returns indices into `scored` in ascending order. The selected
  // subset is a maximal (not maximum) independent set — exact MIS
  // is NP-hard, greedy is correctness-preserving and good enough
  // for V2.4.
  //
  // Tie-break order on equal Score::aggregate is by DeltaId ascending
  // (stable, deterministic). For a chain A-B-C with equal scores,
  // greedy admits A first, rejects B (conflicts with A), then admits
  // C (no remaining conflict).
  //
  // `conflict_edges` is treated as undirected — the (a, b) and
  // (b, a) representations are equivalent. ConflictGraph already
  // emits edges in (a < b) canonical order; this implementation
  // does not require that order but does not benefit from
  // double-listing either.
  std::vector<std::size_t> select(
      const std::vector<ScoredProposal>& scored,
      const std::vector<std::pair<std::size_t, std::size_t>>&
          conflict_edges) override;

  PolicyKind kind() const noexcept override
  {
    return PolicyKind::GreedyPriority;
  }
};

// V2.4.c — convenience converter from the ConflictGraph type to the
// pair-of-indices format that ConflictPolicy::select expects.
// Defined here so callers (V2.4.d's commit path, the V2.4.f shadow
// integration) don't need to write the same loop.
class ConflictGraph;
std::vector<std::pair<std::size_t, std::size_t>> MakeEdgeList(
    const ConflictGraph& g);

}  // namespace drt::redesign
