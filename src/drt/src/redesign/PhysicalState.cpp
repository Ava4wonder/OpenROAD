// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 stub: keeps the symbols linkable but rejects calls into
// unimplemented logic. Real implementation lands in Phase 3 per
// drt_redesign_plan.md §16.

#include "PhysicalState.h"

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include "overlay/OracleCandidate.h"
#include "overlay/OverlayGeometryView.h"
#include "overlay/SyntheticOracle.h"

namespace drt::redesign {

class PhysicalStateImpl
{
 public:
  std::atomic<uint64_t> version{1};
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
  (void) base;
  (void) delta;
  (void) opts;
  throw std::logic_error(
      "drt::redesign::PhysicalState::eval(Snapshot,...) is a stub "
      "through V2.2.c.proj; the GeometryView overload is the real "
      "V2.2.c.proj entry point. PhysicalStateImpl→GeometryView "
      "wiring lands in V2.2.d.");
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
  (void) base;
  (void) proposals;
  (void) policy;
  throw std::logic_error(
      "drt::redesign::PhysicalState::try_commit is a Phase 1 stub; "
      "implementation lands in Phase 3 per drt_redesign_plan.md §16.");
}

}  // namespace drt::redesign
