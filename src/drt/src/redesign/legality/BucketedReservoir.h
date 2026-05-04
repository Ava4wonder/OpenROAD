// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.c — Bucketed reservoir sampling.
//
// Generic per-bucket reservoir: each bucket holds at most `cap` items.
// Once a bucket is full, subsequent items have probability cap/seen of
// replacing a uniform-random retained item. Standard Algorithm R per
// Vitter 1985, applied per bucket.
//
// Used by ClipDumpHook to keep at most `cap` clips per (cand_bin,
// ctx_bin, marker_present) triple — biases retained corpus toward
// representative + stress-weighted population rather than first-N.
//
// Sampling is deterministic given a fixed seed (env DRT_DUMP_SEED).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace drt::redesign::legality {

template <typename Key, typename T>
class BucketedReservoir
{
 public:
  struct BucketStats
  {
    std::uint64_t total_seen = 0;
    std::size_t retained = 0;
  };

  BucketedReservoir(std::size_t per_bucket_cap, std::uint64_t seed)
      : cap_(per_bucket_cap), rng_(seed)
  {
  }

  // Offer one item under bucket `key`. Either retained (push or
  // replace) or dropped; total_seen[key] increments either way.
  void Offer(const Key& key, T&& item)
  {
    Bucket& b = buckets_[key];
    ++b.total_seen;
    if (b.retained.size() < cap_) {
      b.retained.push_back(std::move(item));
      return;
    }
    // Reservoir replace: j uniform in [0, total_seen). If j < cap,
    // replace b.retained[j].
    std::uniform_int_distribution<std::uint64_t> dist(0, b.total_seen - 1);
    const std::uint64_t j = dist(rng_);
    if (j < cap_) {
      b.retained[static_cast<std::size_t>(j)] = std::move(item);
    }
  }

  template <typename F>
  void ForEachRetained(F&& f) const
  {
    for (const auto& [key, bucket] : buckets_) {
      for (const auto& item : bucket.retained) {
        f(key, item);
      }
    }
  }

  std::map<Key, BucketStats> Stats() const
  {
    std::map<Key, BucketStats> out;
    for (const auto& [key, bucket] : buckets_) {
      out[key] = BucketStats{bucket.total_seen, bucket.retained.size()};
    }
    return out;
  }

  std::size_t TotalRetained() const
  {
    std::size_t n = 0;
    for (const auto& [key, bucket] : buckets_) {
      n += bucket.retained.size();
    }
    return n;
  }

  std::uint64_t TotalSeen() const
  {
    std::uint64_t n = 0;
    for (const auto& [key, bucket] : buckets_) {
      n += bucket.total_seen;
    }
    return n;
  }

  std::size_t BucketCount() const { return buckets_.size(); }

 private:
  struct Bucket
  {
    std::vector<T> retained;
    std::uint64_t total_seen = 0;
  };
  std::map<Key, Bucket> buckets_;
  std::size_t cap_;
  std::mt19937_64 rng_;
};

}  // namespace drt::redesign::legality
