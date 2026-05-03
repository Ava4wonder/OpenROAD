// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 stub: keeps the symbols linkable but rejects calls into
// unimplemented logic. Real implementation lands in Phase 3 per
// drt_redesign_plan.md §16.

#include "PhysicalState.h"

#include <atomic>
#include <stdexcept>

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

EvaluationResult PhysicalState::eval(const Snapshot& base,
                                     const ProposedDelta& delta) const
{
  (void) base;
  (void) delta;
  throw std::logic_error(
      "drt::redesign::PhysicalState::eval is a Phase 1 stub; "
      "implementation lands in Phase 3 per drt_redesign_plan.md §16.");
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
