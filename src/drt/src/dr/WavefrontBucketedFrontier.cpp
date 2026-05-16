// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "WavefrontBucketedFrontier.h"

#include <algorithm>
#include <cassert>

namespace drt {

namespace {
struct HeapCmp
{
  bool operator()(const std::pair<frCost, FlexWavefrontGrid>& a,
                  const std::pair<frCost, FlexWavefrontGrid>& b) const
  {
    return a.first > b.first;
  }
};
}  // namespace

void WavefrontBucketedFrontier::clear_for_epoch()
{
  buckets_.clear();
  overflow_.clear();
  base_ = 0;
  scan_ = 0;
  total_ = 0;
}

void WavefrontBucketedFrontier::push(const FlexWavefrontGrid& grid)
{
  const frCost key = grid.getCost();
  ++total_;
  if (key < base_) {
    overflow_.emplace_back(key, grid);
    std::push_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
    return;
  }
  const std::size_t bucket_idx
      = static_cast<std::size_t>((key - base_) / width_);
  if (bucket_idx < kBucketCap) {
    if (bucket_idx >= buckets_.size()) {
      buckets_.resize(bucket_idx + 1);
    }
    buckets_[bucket_idx].push_back(grid);
  } else {
    overflow_.emplace_back(key, grid);
    std::push_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
  }
}

void WavefrontBucketedFrontier::rebucket_overflow()
{
  if (overflow_.empty()) {
    return;
  }
  std::make_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
  base_ = overflow_.front().first;
  scan_ = 0;
  buckets_.clear();
  std::vector<std::pair<frCost, FlexWavefrontGrid>> still_overflow;
  for (auto& [k, g] : overflow_) {
    const std::size_t bi = static_cast<std::size_t>((k - base_) / width_);
    if (bi < kBucketCap) {
      if (bi >= buckets_.size()) {
        buckets_.resize(bi + 1);
      }
      buckets_[bi].push_back(g);
    } else {
      still_overflow.emplace_back(k, g);
    }
  }
  overflow_.swap(still_overflow);
  std::make_heap(overflow_.begin(), overflow_.end(), HeapCmp{});
}

std::vector<FlexWavefrontGrid>& WavefrontBucketedFrontier::pop_min_bucket()
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
