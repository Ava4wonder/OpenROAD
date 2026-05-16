// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Bucketed frontier holding FlexWavefrontGrid payloads.
// Parallels src/drt/src/dr/BucketedFrontier.{h,cpp} but stores the
// existing FlexWavefrontGrid object so the new SoA search loop can
// reuse the existing expandWavefront/getNextPathCost cost machinery
// without rewriting it.
//
// Compared with std::priority_queue<FlexWavefrontGrid>:
//   - push():       O(1) instead of O(log N)
//   - pop():        amortized O(1) per item; pops a full bucket
//                   (items with the same coarse priority key) at once.
//   - tie-breaking: caller may sort the popped batch by
//                   FlexWavefrontGrid::operator< to preserve A* tie-
//                   breaking semantics within a bucket.

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "FlexWavefront.h"
#include "frBaseTypes.h"

namespace drt {

class WavefrontBucketedFrontier
{
 public:
  // bucket_width=1 keeps strict A*-like ordering between buckets.
  explicit WavefrontBucketedFrontier(frCost bucket_width = 1)
      : width_(bucket_width == 0 ? 1 : bucket_width)
  {
  }

  // Drop all queued items and reset base. Use at the start of a search.
  void clear_for_epoch();

  // Insert grid at priority key = grid.getCost().
  void push(const FlexWavefrontGrid& grid);

  bool empty() const { return total_ == 0; }
  std::size_t size() const { return total_; }
  frCost width() const { return width_; }

  // Return a non-empty bucket containing the current min-priority items.
  // The reference is valid until the next push() or pop call.
  std::vector<FlexWavefrontGrid>& pop_min_bucket();

 private:
  static constexpr std::size_t kBucketCap = 4096;

  void rebucket_overflow();

  std::vector<std::vector<FlexWavefrontGrid>> buckets_;
  // (key, payload); min-heap on key for items outside buckets_ window.
  std::vector<std::pair<frCost, FlexWavefrontGrid>> overflow_;
  frCost base_ = 0;
  frCost width_ = 1;
  std::size_t total_ = 0;
  std::size_t scan_ = 0;  // index of next bucket to drain
};

}  // namespace drt
