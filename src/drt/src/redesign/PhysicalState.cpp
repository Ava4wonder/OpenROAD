// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 stub: keeps the symbols linkable but rejects calls into
// unimplemented logic. Real implementation lands in Phase 3 per
// drt_redesign_plan.md §16.

#include "PhysicalState.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include "overlay/MutableGeometryStore.h"
#include "overlay/OracleCandidate.h"
#include "overlay/OverlayGeometryView.h"
#include "overlay/SyntheticOracle.h"

namespace drt::redesign {

class PhysicalStateImpl
{
 public:
  std::atomic<uint64_t> version{1};
  // V2.2.d — writable backing. PhysicalState is the only thing
  // allowed to mutate this; eval/Snapshot consumers see it through
  // the GeometryView interface.
  overlay::MutableGeometryStore store;
  // V2.2.d single-Delta scope: one commit at a time. V2.4's MIS
  // commit path takes this lock around the whole batch.
  mutable std::mutex commit_mu;
};

struct PhysicalState::Impl
{
  std::shared_ptr<PhysicalStateImpl> state = std::make_shared<PhysicalStateImpl>();
};

PhysicalState::PhysicalState() : impl_(std::make_unique<Impl>())
{
}

PhysicalState::~PhysicalState() = default;

Snapshot::Snapshot() : version_(0)
{
}

Snapshot::Snapshot(std::shared_ptr<const PhysicalStateImpl> impl, uint64_t version)
    : impl_(std::move(impl)), version_(version)
{
}

Snapshot::~Snapshot() = default;

uint64_t Snapshot::version() const noexcept
{
  return version_;
}

bool Snapshot::valid() const noexcept
{
  return static_cast<bool>(impl_);
}

const PhysicalStateImpl* Snapshot::impl() const noexcept
{
  return impl_.get();
}

const overlay::GeometryView* Snapshot::geometry() const noexcept
{
  if (!impl_) {
    return nullptr;
  }
  return &impl_->store;
}

uint64_t PhysicalState::current_version() const noexcept
{
  return impl_->state->version.load(std::memory_order_acquire);
}

Snapshot PhysicalState::snapshot() const
{
  return Snapshot(impl_->state, current_version());
}

EvalOutcome PhysicalState::eval(const Snapshot& base,
                                const ProposedDelta& delta,
                                EvalOptions opts) const
{
  // V2.2.d — Snapshot now carries a GeometryView pointer via
  // PhysicalStateImpl::store. Delegate to the GeometryView overload.
  if (!base.valid() || base.geometry() == nullptr) {
    EvalOutcome bad;
    bad.legality.legal = false;
    bad.legality.source = LegalitySource::UnresolvedFootprint;
    bad.legality.commit_eligible = false;
    return bad;
  }
  return eval(*base.geometry(), delta, opts);
}

namespace {

// V2.2.c.proj — Manhattan length proxy. Sum of max(dx, dy) over the
// route-shape bbox. NOT bbox perimeter (which would overcount for
// horizontal/vertical wires that are long on one axis and track-width
// on the other). Refined when V2.2.e wires real frPathSeg
// path-length accessors.
double ManhattanLengthProxy(const Rect& r) noexcept
{
  const auto dx = r.ur.x > r.ll.x ? r.ur.x - r.ll.x : r.ll.x - r.ur.x;
  const auto dy = r.ur.y > r.ll.y ? r.ur.y - r.ll.y : r.ll.y - r.ur.y;
  return static_cast<double>(dx > dy ? dx : dy);
}

// V2.2.c.proj — does the delta carry an unresolved delete identity?
// If yes, eval cannot validate it and must surface a non-committable
// EvalOutcome (LegalitySource::UnresolvedFootprint). Mirrors the
// OverlayGeometryView "silent skip" behaviour but here, eval must
// be loud about it because consumers might otherwise compute a
// score as if the delete had taken effect.
bool DeltaHasUnresolvedDeleteIdentity(const Delta& d) noexcept
{
  return std::visit(
      [](const auto& kind) -> bool {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, DeleteWire>) {
          if (!kind.resolved_net_id.has_value()
              || !kind.shape_kind.has_value()) {
            return true;
          }
          if (kind.bbox.ll.x == kind.bbox.ur.x
              && kind.bbox.ll.y == kind.bbox.ur.y) {
            return true;
          }
          return false;
        }
        if constexpr (std::is_same_v<T, DeleteVia>) {
          if (!kind.resolved_net_id.has_value()) {
            return true;
          }
          if (kind.bbox.ll.x == kind.bbox.ur.x
              && kind.bbox.ll.y == kind.bbox.ur.y) {
            return true;
          }
          return false;
        }
        return false;
      },
      d);
}

}  // namespace

EvalOutcome PhysicalState::eval(
    const overlay::GeometryView& base_geometry,
    const ProposedDelta& delta,
    EvalOptions opts) const
{
  EvalOutcome outcome;

  // Step 1: unresolved delete identity → loud non-committable.
  if (DeltaHasUnresolvedDeleteIdentity(delta.delta)) {
    outcome.legality.legal = false;
    outcome.legality.source = LegalitySource::UnresolvedFootprint;
    outcome.legality.commit_eligible = false;
    // No score: we cannot meaningfully cost a delta whose effect
    // is unknown. (V2.2.c.proj convention: caller should not
    // examine score when commit_eligible=false unless
    // opts.score_even_if_illegal is set explicitly.)
    if (opts.score_even_if_illegal) {
      outcome.score = Score{};  // zero-filled stub
    }
    return outcome;
  }

  // Step 2: legality dispatch on opts.legality_mode.
  switch (opts.legality_mode) {
    case LegalityMode::SyntheticOracle: {
      // V2.2.c.legality.synthetic — bridge the delta into oracle
      // input and run the synthetic oracle. commit_eligible follows
      // the trusted-source rule (SyntheticOracle is trusted).
      const auto batch
          = overlay::DeltaToOracleInput(base_geometry, delta);
      auto verdict = overlay::SyntheticOracle::Evaluate(batch,
                                                        base_geometry);
      verdict.commit_eligible
          = verdict.legal && IsTrustedLegalitySource(verdict.source);
      outcome.legality = verdict;
      break;
    }
    case LegalityMode::CpuDrcOracleRealDeck:
      // V2.2.c.legality.realpdk lands later. For V2.2.c.legality.
      // synthetic, falling through to the stub path keeps the
      // behaviour predictable: a caller that requested real-PDK
      // before it's wired gets a clearly non-committable verdict.
      outcome.legality.legal = true;
      outcome.legality.source = LegalitySource::StubAssumeLegal;
      outcome.legality.commit_eligible = false;
      break;
    case LegalityMode::StubAssumeLegal:
    default:
      // V2.2.c.proj behaviour preserved. The V2.1.b hard-gate
      // discipline requires commit_eligible=false even when
      // legal=true because the verdict is unvalidated.
      outcome.legality.legal = true;
      outcome.legality.source = LegalitySource::StubAssumeLegal;
      outcome.legality.commit_eligible = false;
      break;
  }

  // Step 3: simple Score. V2.2.c.score adds congestion/timing/
  // history terms.
  Score score;
  std::visit(
      [&](const auto& kind) {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, AddWire>) {
          score.delta_wirelength_proxy
              += ManhattanLengthProxy(kind.bbox);
        } else if constexpr (std::is_same_v<T, DeleteWire>) {
          // Delete identity is resolved (Step 1 returned earlier).
          score.delta_wirelength_proxy
              -= ManhattanLengthProxy(kind.bbox);
        } else if constexpr (std::is_same_v<T, AddVia>) {
          score.delta_via_count += 1;
        } else if constexpr (std::is_same_v<T, DeleteVia>) {
          score.delta_via_count -= 1;
        } else if constexpr (std::is_same_v<T, InsertShield>) {
          // Shields contribute to wirelength as routing area.
          score.delta_wirelength_proxy
              += ManhattanLengthProxy(kind.coverage);
        }
        // MoveCell / ChangePinAccess / ChangeLayerAssignment /
        // ResizeCell: V2.2.c.proj scores them as zero. Real
        // accounting lands when these kinds are exercised by real
        // proposers (likely V2.3+).
      },
      delta.delta);

  // V2.2.c.proj aggregate: simple sum. V2.2.c.score adds
  // phase-dependent weighted aggregation.
  score.aggregate
      = score.delta_wirelength_proxy + 100.0 * score.delta_via_count;

  outcome.score = score;
  return outcome;
}

CommitResult PhysicalState::try_commit(const Snapshot& base,
                                       std::vector<ProposedDelta> proposals,
                                       ConflictPolicy& policy)
{
  // V2.2.d — single-Delta commit path. Conflict policy parameter is
  // accepted but not consulted; V2.4 wires the cross-delta conflict
  // graph and the policy.select() call. For V2.2.d the loop runs
  // proposals in order and commits each independently.
  (void) policy;

  CommitResult result;
  std::lock_guard<std::mutex> g(impl_->state->commit_mu);

  // V2.2.d uses the EvalOptions default (StubAssumeLegal). Real
  // PoC commits require the caller to call try_commit_with_opts
  // (added below) and pass LegalityMode::SyntheticOracle. The
  // default-mode try_commit therefore ALWAYS rejects everything as
  // non-committable — which is correct: stub-source proposals are
  // not commit-eligible.
  EvalOptions opts;

  for (std::size_t i = 0; i < proposals.size(); ++i) {
    const ProposedDelta& p = proposals[i];

    // Stale-snapshot detection. The proposal was generated against
    // some snapshot version (p.snapshot_version) and base.version()
    // is the snapshot the caller is currently committing against.
    // If they don't match, or if base is stale relative to the
    // current PhysicalState version, reject as snapshot_stale.
    const std::uint64_t cur = current_version();
    if (!base.valid() || base.version() != cur
        || p.snapshot_version != cur) {
      result.snapshot_stale.push_back(i);
      continue;
    }

    // Eval against the current store.
    EvalOutcome outcome = eval(impl_->state->store, p, opts);
    if (!outcome.legality.commit_eligible) {
      result.rejected_indices.push_back(i);
      continue;
    }

    // Apply.
    if (impl_->state->store.Apply(p.delta)) {
      result.committed_indices.push_back(i);
      impl_->state->version.fetch_add(1, std::memory_order_acq_rel);
    } else {
      // commit_eligible was true but Apply refused — programming
      // error in the eval/Apply contract. Surface as rejected and
      // do not bump the version.
      result.rejected_indices.push_back(i);
    }
  }

  result.new_version = impl_->state->version.load(
      std::memory_order_acquire);
  return result;
}

// V2.2.d — opt-in entry point for PoC commits using the synthetic
// oracle. Production code paths will land their own opts handling
// in V2.2.f integration.
CommitResult PhysicalState::try_commit_with_opts(
    const Snapshot& base,
    std::vector<ProposedDelta> proposals,
    ConflictPolicy& policy,
    EvalOptions opts)
{
  (void) policy;
  CommitResult result;
  std::lock_guard<std::mutex> g(impl_->state->commit_mu);

  for (std::size_t i = 0; i < proposals.size(); ++i) {
    const ProposedDelta& p = proposals[i];

    const std::uint64_t cur = current_version();
    if (!base.valid() || base.version() != cur
        || p.snapshot_version != cur) {
      result.snapshot_stale.push_back(i);
      continue;
    }

    EvalOutcome outcome = eval(impl_->state->store, p, opts);
    if (!outcome.legality.commit_eligible) {
      result.rejected_indices.push_back(i);
      continue;
    }

    if (impl_->state->store.Apply(p.delta)) {
      result.committed_indices.push_back(i);
      impl_->state->version.fetch_add(1, std::memory_order_acq_rel);
    } else {
      result.rejected_indices.push_back(i);
    }
  }

  result.new_version = impl_->state->version.load(
      std::memory_order_acquire);
  return result;
}

// V2.2.d test-only inspection of the writable store. PhysicalState
// owns the store; tests use this accessor to assert post-commit
// state without going through Snapshot.
const overlay::GeometryView& PhysicalState::geometry_view_for_test()
    const noexcept
{
  return impl_->state->store;
}

overlay::MutableGeometryStore& PhysicalState::mutable_store_for_test()
    noexcept
{
  return impl_->state->store;
}

}  // namespace drt::redesign
