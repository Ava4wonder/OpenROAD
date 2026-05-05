// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.d — SnapshotHandle.
//
// V2.1 only: read-through view over live FlexDR/FlexGridGraph state.
// Provides API compatibility, not multi-version snapshot isolation.
// V2.4 replaces the V2.1 stub with persistent CoW backing.
//
// SnapshotHandle is the user-facing object workers obtain from
// FlexDRWorker::freeze() (V2.1.e). It additively wraps the existing
// drt::redesign::Snapshot with a typed GeometryView, leaving the
// underlying Snapshot type unchanged.
//
// Ownership: GeometryView is held by std::shared_ptr<const>. This is
// intentional — V2.4's persistent backing structure may produce
// views whose lifetime exceeds any single worker, and shared_ptr
// gives us reference-counted safety without forcing every consumer
// to reason about lifetime by hand. The cost (one atomic refcount
// per copy) is acceptable on the current call frequency; V2.4 may
// revisit if it shows up in profiles.

#pragma once

#include <memory>
#include <utility>

#include "../Snapshot.h"
#include "GeometryView.h"

namespace drt::redesign::overlay {

class SnapshotHandle
{
 public:
  SnapshotHandle() = default;

  SnapshotHandle(drt::redesign::Snapshot snap,
                 std::shared_ptr<const GeometryView> geometry)
      : snap_(std::move(snap)), geometry_(std::move(geometry))
  {
  }

  bool valid() const noexcept
  {
    return snap_.valid() && static_cast<bool>(geometry_);
  }

  uint64_t version() const noexcept { return snap_.version(); }

  const drt::redesign::Snapshot& snapshot() const noexcept { return snap_; }

  // Precondition: valid() == true. UB to call on a default-constructed
  // SnapshotHandle.
  const GeometryView& geometry() const noexcept { return *geometry_; }

 private:
  drt::redesign::Snapshot snap_;
  std::shared_ptr<const GeometryView> geometry_;
};

}  // namespace drt::redesign::overlay
