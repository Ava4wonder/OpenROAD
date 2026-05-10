// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding for the DRT redesign.
// Versioned, transactional physical state engine — the foundation of the
// redesign per drt_redesign_plan.md §1.
//
// Phase 1 status: interface only. eval() and try_commit() are stubs that
// throw; the indices, snapshot machinery, and per-stage workers land in
// later phases (§16 roadmap).

#pragma once

#include <memory>
#include <vector>

#include "BatchEval.h"
#include "CommitResult.h"
#include "ConflictPolicy.h"
#include "Delta.h"
#include "EvalOutcome.h"
#include "Footprint.h"
#include "Snapshot.h"

namespace drt::redesign::overlay {
class GeometryView;
class MutableGeometryStore;
}

namespace drt::redesign::legality {
class CpuDrcOracle;
struct RuleEntry;
}

namespace drt::redesign {

class CongestionTimingProvider;

class PhysicalState
{
 public:
  PhysicalState();
  ~PhysicalState();

  PhysicalState(const PhysicalState&) = delete;
  PhysicalState& operator=(const PhysicalState&) = delete;
  PhysicalState(PhysicalState&&) = delete;
  PhysicalState& operator=(PhysicalState&&) = delete;

  // Frozen, lock-free read view of the current state.
  Snapshot snapshot() const;

  // Cost of applying `delta` against `base` without mutating state.
  // Workers use this for proposal ranking before submitting to try_commit.
  // Returns hard LegalityVerdict and (optional) soft Score — the hard/
  // soft split is enforced by the type system; see v2 plan §4.3.
  //
  // V2.2.c.proj: stub — Snapshot does not yet expose a GeometryView.
  // Use the GeometryView overload below until V2.2.d wires
  // PhysicalStateImpl to provide one.
  EvalOutcome eval(const Snapshot& base,
                   const ProposedDelta& delta,
                   EvalOptions opts = {}) const;

  // V2.2.c.proj — real eval over an explicit GeometryView base.
  // Builds an OverlayGeometryView(base, [delta]) internally to score
  // the delta. LegalityVerdict source is StubAssumeLegal in V2.2.c.proj
  // (commit_eligible=false). V2.2.c.legality replaces the stub with
  // CpuDrcOracle.
  EvalOutcome eval(const overlay::GeometryView& base_geometry,
                   const ProposedDelta& delta,
                   EvalOptions opts = {}) const;

  // V2.3.a — batched multi-proposal evaluation. K candidate
  // ProposedDeltas evaluated against the same base. Result vectors
  // are sorted by stable DeltaId for canonical ordering — the
  // caller does not have to pre-sort. Each per-proposal outcome is
  // identical to a single-eval call (same logic, same legality
  // mode, same score formula). Per-proposal eval times sum into
  // summary.total_eval_time_ns.
  //
  // V2.3.a SCOPE — mechanics validation only. Batch eval is a
  // sorted loop over single eval(). No legality-oracle batching,
  // no GPU dispatch — those are V2.4+ work. The architectural
  // payoff in this commit is that ProposalSet → BatchEvalResult is
  // a typed call site that V2.3.b's selector can consume.
  BatchEvalResult batch_eval(const overlay::GeometryView& base_geometry,
                             const ProposalSet& set,
                             EvalOptions opts = {}) const;

  // Two-phase commit (plan §7). Resolves conflicts via `policy`; commits the
  // selected non-conflicting subset; rejects proposals built against a stale
  // snapshot. Atomic with respect to other commits.
  //
  // V2.2.d single-Delta scope: `policy` parameter is accepted but
  // not consulted. Uses default EvalOptions (StubAssumeLegal), which
  // means everything is rejected as non-committable. Use
  // try_commit_with_opts to drive the synthetic-oracle PoC path.
  CommitResult try_commit(const Snapshot& base,
                          std::vector<ProposedDelta> proposals,
                          ConflictPolicy& policy);

  // V2.2.d — explicit-opts entry point. Required when caller wants
  // SyntheticOracle / CpuDrcOracle commit eligibility.
  CommitResult try_commit_with_opts(
      const Snapshot& base,
      std::vector<ProposedDelta> proposals,
      ConflictPolicy& policy,
      EvalOptions opts);

  // V2.2.d test-only inspection of the writable backing.
  const overlay::GeometryView& geometry_view_for_test() const noexcept;
  overlay::MutableGeometryStore& mutable_store_for_test() noexcept;

  // V2.5.b — attach a CPU DRC oracle + rule list. When
  // EvalOptions::legality_mode == CpuDrcOracleRealDeck, eval()
  // routes to this oracle instead of the V2.2.c stub fallthrough.
  // Lifetime: the caller owns both `oracle` and the storage backing
  // each RuleEntry's `opaque` pointer; both must outlive the next
  // eval()/batch_eval()/try_commit() call. Pass nullptr to detach.
  //
  // V2.5.b SCOPE — minimum viable wiring. `rules` is expected to be
  // a small deck (e.g., MetalShort-only) so the architectural seam
  // can be exercised. Full PDK rule translation is V2.5.b.deck.
  void SetCpuDrcOracle(legality::CpuDrcOracle* oracle,
                       std::vector<legality::RuleEntry> rules);

  // V2.5.c — attach a per-net congestion + timing data provider.
  // When set, eval() populates Score::delta_congestion +
  // Score::delta_timing from the provider for every successful
  // candidate; aggregate then weights them via
  // CostWeights::congestion + CostWeights::timing. Pass nullptr
  // to detach. Lifetime: caller-owned. The provider MUST be
  // re-entrant (eval() runs inside V2.4.f's OMP-parallel region).
  void SetCongestionTimingProvider(
      const CongestionTimingProvider* provider);

  uint64_t current_version() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drt::redesign
