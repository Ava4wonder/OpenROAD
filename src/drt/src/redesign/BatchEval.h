// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.3.a — batched multi-proposal evaluation mechanics.
//
// Validates that K candidate ProposedDeltas can be evaluated against
// the same base GeometryView in one call, with stable proposal IDs
// and a canonically-ordered result. This is the architectural
// substrate for V2.3.b (deterministic selector) and the eventual
// V2.4 (compatible-subset commit / conflict graph).
//
// V2.3 SCOPE — architecture validation, NOT speedup claims. Batch
// eval just runs eval() per proposal in sorted order. The
// payoff is in V2.4+ when the legality oracle batches across
// candidates.

#pragma once

#include <cstdint>
#include <vector>

#include "EvalOutcome.h"
#include "Footprint.h"

namespace drt::redesign {

// V2.3.a — a set of proposals to evaluate against a shared base.
struct ProposalSet
{
  std::vector<ProposedDelta> proposals;
  // Snapshot version the proposals were generated against. All
  // proposals in the set must agree on this; batch_eval() asserts
  // (or rejects) mismatched proposals.
  std::uint64_t base_snapshot_version = 0;
};

// V2.3.a — batch summary counters. Lifted from per-proposal outcomes
// so callers (and the shadow dump) can record one row per batch.
struct BatchEvalSummary
{
  std::size_t batch_size = 0;
  std::size_t legal_count = 0;
  std::size_t commit_eligible_count = 0;
  std::size_t unsupported_count = 0;   // adapter-level UnsupportedDelta
  std::size_t unresolved_count = 0;    // adapter-level UnresolvedFootprint
                                       // OR detected at eval time
  // Cumulative wall-time across per-proposal eval calls. Useful as
  // a coarse PoC sanity number; not a perf claim. nanoseconds.
  std::int64_t total_eval_time_ns = 0;
};

struct BatchEvalResult
{
  // outcomes[i] corresponds to the i-th proposal in the
  // canonically-sorted order (sorted by DeltaId). Callers wanting
  // the original input order must remember it themselves; the batch
  // eval re-orders for determinism.
  std::vector<EvalOutcome> outcomes;
  // Stable order: outcome_proposal_ids[i] is the DeltaId of
  // outcomes[i]. Lets the caller correlate back to a known proposal
  // even though the internal vector was sorted.
  std::vector<DeltaId> outcome_proposal_ids;
  BatchEvalSummary summary;
};

}  // namespace drt::redesign
