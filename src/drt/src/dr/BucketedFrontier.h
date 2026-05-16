// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Bucketed/vector frontier for the SoA maze search. See plan.md §4.3
// and §5 Phase 3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "MazeNodeIndex.h"
#include "frBaseTypes.h"

namespace drt {

class BucketedFrontier
{
 public:
  // bucket_width=1 preserves strict A*-like ordering. Larger widths
  // amortize bucket-management overhead at the cost of some ordering
  // imprecision (callers may then need within-bucket tie-breaking).
  explicit BucketedFrontier(frCost bucket_width = 1)
      : width_(bucket_width == 0 ? 1 : bucket_width)
  {
  }

  // Drop all queued items and reset base. Use at the start of a search.
  void clear_for_epoch();

  // Insert a node at the given priority key.
  void push(MazeNodeId node, frCost key);

  bool empty() const { return total_ == 0; }
  std::size_t size() const { return total_; }
  frCost width() const { return width_; }

  // Return a non-empty bucket containing the current minimum-priority
  // items. Returned reference is valid until the next push() or pop.
  // Caller is expected to consume the batch before the next pop call.
  std::vector<MazeNodeId>& pop_min_bucket();

 private:
  // Soft cap on resident bucket count. At width=1 this covers a
  // kBucketCap-wide cost window before items spill to overflow.
  static constexpr std::size_t kBucketCap = 4096;

  void rebucket_overflow();

  std::vector<std::vector<MazeNodeId>> buckets_;
  // (key, node) pairs; min-heap on key for items outside buckets_ window.
  std::vector<std::pair<frCost, MazeNodeId>> overflow_;
  frCost base_ = 0;
  frCost width_ = 1;
  std::size_t total_ = 0;
  std::size_t scan_ = 0;  // index of next bucket to drain
};

}  // namespace drt
