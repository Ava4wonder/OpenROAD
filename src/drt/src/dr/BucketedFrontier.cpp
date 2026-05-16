// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "BucketedFrontier.h"

#include <algorithm>
#include <cassert>

namespace drt {

namespace {
struct HeapCmp
{
  bool operator()(const std::pair<frCost, MazeNodeId>& a,
                  const std::pair<frCost, MazeNodeId>& b) const
  {
    return a.first > b.first;
  }
};
}  // namespace

void BucketedFrontier::clear_for_epoch()
{
  buckets_.clear();
  overflow_.clear();
  base_ = 0;
  scan_ = 0;
  total_ = 0;
}

void BucketedFrontier::push(MazeNodeId node, frCost key)
{
  ++total_;
  if (key < base_) {
    // Lower than current base; defer to overflow rather than reshuffle
    // existing buckets.
    overflow_.emplace_back(key, node);
    std::push_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
    return;
  }
  const std::size_t bucket_idx
      = static_cast<std::size_t>((key - base_) / width_);
  if (bucket_idx < kBucketCap) {
    if (bucket_idx >= buckets_.size()) {
      buckets_.resize(bucket_idx + 1);
    }
    buckets_[bucket_idx].push_back(node);
  } else {
    overflow_.emplace_back(key, node);
    std::push_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
  }
}

void BucketedFrontier::rebucket_overflow()
{
  if (overflow_.empty()) {
    return;
  }
  std::make_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
  base_ = overflow_.front().first;
  scan_ = 0;
  buckets_.clear();
  std::vector<std::pair<frCost, MazeNodeId>> still_overflow;
  for (auto& [k, n] : overflow_) {
    const std::size_t bi = static_cast<std::size_t>((k - base_) / width_);
    if (bi < kBucketCap) {
      if (bi >= buckets_.size()) {
        buckets_.resize(bi + 1);
      }
      buckets_[bi].push_back(n);
    } else {
      still_overflow.emplace_back(k, n);
    }
  }
  overflow_.swap(still_overflow);
  std::make_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
}

std::vector<MazeNodeId>& BucketedFrontier::pop_min_bucket()
{
  assert(!empty());
  while (scan_ < buckets_.size() && buckets_[scan_].empty()) {
    ++scan_;
  }
  if (scan_ >= buckets_.size()) {
    rebucket_overflow();
    return pop_min_bucket();
  }
  auto& batch = buckets_[scan_];
  total_ -= batch.size();
  ++scan_;
  return batch;
}

}  // namespace drt
