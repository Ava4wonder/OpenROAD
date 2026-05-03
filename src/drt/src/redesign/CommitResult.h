// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding. See drt_redesign_plan.md §7E.
// Returned from PhysicalState::try_commit. Reports which proposals landed,
// which were conflicted out, and which were rejected because they were
// proposed against a stale snapshot version.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace drt::redesign {

struct CommitResult
{
  uint64_t new_version = 0;
  std::vector<size_t> committed_indices;
  std::vector<size_t> rejected_indices;
  std::vector<size_t> snapshot_stale;

  bool ok() const noexcept { return new_version != 0; }
};

}  // namespace drt::redesign
