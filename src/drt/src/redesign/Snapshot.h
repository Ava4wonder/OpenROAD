// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding. See drt_redesign_plan.md §1.
// A Snapshot is a read-only handle to a frozen version of PhysicalState.
// Workers hold a Snapshot for the duration of one proposal cycle; the
// underlying state is shared via shared_ptr so concurrent commits do not
// invalidate the snapshot's view.

#pragma once

#include <cstdint>
#include <memory>

namespace drt::redesign {

class PhysicalStateImpl;

class Snapshot
{
 public:
  Snapshot();
  Snapshot(std::shared_ptr<const PhysicalStateImpl> impl, uint64_t version);
  ~Snapshot();

  uint64_t version() const noexcept;
  bool valid() const noexcept;
  const PhysicalStateImpl* impl() const noexcept;

 private:
  std::shared_ptr<const PhysicalStateImpl> impl_;
  uint64_t version_;
};

}  // namespace drt::redesign
