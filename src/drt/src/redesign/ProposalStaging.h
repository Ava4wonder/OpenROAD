// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.e — cross-worker barrier primitive.
//
// Per v2_drt_redesign_plan.md §6 (V2.4): "across workers within an
// iteration, collect candidate Deltas, build conflict graph, solve
// MIS, commit subset atomically."
//
// V2.4.e ships the *staging* half of that loop — a process-wide,
// thread-safe queue that workers push their per-call selected
// proposals into. The *resolve* half drains the queue, builds a
// cross-worker ConflictGraph, runs SelectMis, and returns the
// cross-worker selection. V2.4.f wires the staging point into
// FlexDR's worker hook (extending V2.3.c's shadow integration).
//
// V2.4.e SCOPE — primitive only. Shadow mode per the V2.4 gating
// decision: never mutates production routing; ResolveStagedProposals
// returns the would-have-committed subset and the dump captures it,
// but no try_commit fires against any real PhysicalState. Drive mode
// is V2.6.

#pragma once

#include <cstddef>
#include <vector>

#include "BatchEval.h"
#include "ConflictGraph.h"
#include "EvalOutcome.h"
#include "Footprint.h"
#include "Selection.h"

namespace drt::redesign {

class ConflictPolicy;

// One staged unit. The (proposal, outcome) pair is what each worker
// has after running its per-call batch_eval. adapter_unsupported is
// the per-proposal flag from BatchEvalResult — preserves the V2.3.b
// taxonomy distinction between UnresolvedFootprint and
// UnsupportedDelta during cross-worker resolve.
struct StagedProposal
{
  ProposedDelta proposal;
  EvalOutcome outcome;
  bool adapter_unsupported = false;
};

class ProposalStaging
{
 public:
  // Process-wide singleton. Thread-safe: all methods take an
  // internal mutex.
  static ProposalStaging& Instance();

  // Push one staged unit. Workers call this from inside their
  // OMP-parallel hook. Internal mutex serialises pushes — enough
  // for V2.4.e/f shadow scale (~K * batches_per_iter pushes).
  // V2.5+ may want a sharded queue.
  void Stage(StagedProposal sp);

  // Atomically remove all staged proposals; returns the drained
  // set in canonical (DeltaId-sorted) order. Empty after.
  std::vector<StagedProposal> Drain();

  // Test / introspection.
  std::size_t size() const noexcept;

  // Test-only — discard everything without returning. Used by
  // unit tests to reset between cases (the singleton would
  // otherwise carry state across tests).
  void ResetForTest();
};

// V2.4.e — cross-worker resolve. Drains the singleton staging area,
// reconstructs a synthetic BatchEvalResult + ConflictGraph from the
// drained units, runs SelectMis with the caller-supplied policy, and
// returns all three for downstream BatchSummaryDump capture.
//
// "Synthetic" BatchEvalResult: a regular BatchEvalResult assembled
// from the drained outcomes, so downstream consumers (the dump,
// MakeBatchSummaryRow, etc.) can treat the cross-worker batch
// uniformly with per-worker batches.
//
// SCOPE — shadow only. No PhysicalState mutation; the caller is
// expected to pass the result to BatchSummaryDump and discard.
struct CrossWorkerResolveResult
{
  BatchEvalResult batch;
  ConflictGraph graph;
  MisSelectionResult selection;
};

CrossWorkerResolveResult ResolveStagedProposals(ConflictPolicy& policy);

}  // namespace drt::redesign
