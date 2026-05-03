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

#include "CommitResult.h"
#include "ConflictPolicy.h"
#include "Delta.h"
#include "EvaluationResult.h"
#include "Snapshot.h"

namespace drt::redesign {

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
  EvaluationResult eval(const Snapshot& base, const ProposedDelta& delta) const;

  // Two-phase commit (plan §7). Resolves conflicts via `policy`; commits the
  // selected non-conflicting subset; rejects proposals built against a stale
  // snapshot. Atomic with respect to other commits.
  CommitResult try_commit(const Snapshot& base,
                          std::vector<ProposedDelta> proposals,
                          ConflictPolicy& policy);

  uint64_t current_version() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drt::redesign
