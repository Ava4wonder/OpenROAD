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

namespace overlay {
class GeometryView;
}

class Snapshot
{
 public:
  Snapshot();
  Snapshot(std::shared_ptr<const PhysicalStateImpl> impl, uint64_t version);
  ~Snapshot();

  uint64_t version() const noexcept;
  bool valid() const noexcept;
  const PhysicalStateImpl* impl() const noexcept;

  // V2.2.d — typed read view over the physical state's writable
  // backing. Returns nullptr if the snapshot is invalid (default-
  // constructed). The pointer remains valid for the lifetime of
  // this Snapshot's shared_ptr.
  //
  // V2.1.d/.e disclaimer still applies: this is a read-through
  // view over the live state, NOT multi-version snapshot isolation.
  // V2.4 replaces with persistent CoW backing.
  const overlay::GeometryView* geometry() const noexcept;

 private:
  std::shared_ptr<const PhysicalStateImpl> impl_;
  uint64_t version_;
};

}  // namespace drt::redesign
