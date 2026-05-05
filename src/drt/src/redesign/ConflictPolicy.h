// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding. See drt_redesign_plan.md §7D.
// ConflictPolicy chooses which proposals to commit given the conflict graph.
// Implementations differ in objective and completeness:
//   GreedyPriority         — fastest, best-first
//   ParallelGraphColoring  — high concurrency, may sacrifice ranking
//   LagrangianRelaxation   — handles soft constraints
//   LargeNeighborhoodSearch — quality, slowest
//   LearnedRanking         — ML-driven, see plan §13
//   MaximalIndependentSet  — concurrency-maximizing baseline

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Delta.h"
#include "LegalityVerdict.h"
#include "Score.h"

namespace drt::redesign {

enum class PolicyKind : uint8_t {
  GreedyPriority,
  ParallelGraphColoring,
  LagrangianRelaxation,
  LargeNeighborhoodSearch,
  LearnedRanking,
  MaximalIndependentSet,
};

// Inputs to ConflictPolicy::select. Per v2 plan §4.3, all inputs MUST
// have already passed legality (legality.legal == true) before the
// policy sees them; the policy is not a legality filter and never
// commits something illegal. The legality field is retained on the
// struct only so consumers can audit / log the verdict trail.
struct ScoredProposal
{
  ProposedDelta proposal;
  LegalityVerdict legality;
  Score score;
};

class ConflictPolicy
{
 public:
  virtual ~ConflictPolicy() = default;

  // Returns indices into `scored` that should be committed.
  // `conflict_edges` is an undirected edge list over the same indices.
  virtual std::vector<size_t> select(
      const std::vector<ScoredProposal>& scored,
      const std::vector<std::pair<size_t, size_t>>& conflict_edges)
      = 0;

  virtual PolicyKind kind() const noexcept = 0;
};

}  // namespace drt::redesign
