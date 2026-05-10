// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.c — GreedyPriorityPolicy implementation.

#include "GreedyPriorityPolicy.h"

#include <algorithm>
#include <unordered_set>

#include "ConflictGraph.h"
#include "Footprint.h"  // DeltaId

namespace drt::redesign {

namespace {

// 64-bit hash for an unordered (a, b) edge pair so an edge can be
// found regardless of which side queries it. Caller normalises
// (a < b) before keying.
std::uint64_t EdgeKey(std::size_t a, std::size_t b) noexcept
{
  return (static_cast<std::uint64_t>(a) << 32)
         | static_cast<std::uint64_t>(b);
}

}  // namespace

std::vector<std::size_t> GreedyPriorityPolicy::select(
    const std::vector<ScoredProposal>& scored,
    const std::vector<std::pair<std::size_t, std::size_t>>&
        conflict_edges)
{
  const std::size_t n = scored.size();
  if (n == 0) {
    return {};
  }

  // Build an O(1) edge lookup. Normalise to (min, max) so direction
  // doesn't matter at query time.
  std::unordered_set<std::uint64_t> edge_set;
  edge_set.reserve(conflict_edges.size() * 2);
  for (const auto& e : conflict_edges) {
    const std::size_t a = std::min(e.first, e.second);
    const std::size_t b = std::max(e.first, e.second);
    edge_set.insert(EdgeKey(a, b));
  }

  // Sort indices: descending by Score::aggregate, ties broken by
  // ascending DeltaId. Stable order: same input → same priority list.
  std::vector<std::size_t> by_priority(n);
  for (std::size_t i = 0; i < n; ++i) {
    by_priority[i] = i;
  }
  std::sort(by_priority.begin(), by_priority.end(),
            [&](std::size_t l, std::size_t r) {
              const double sl = scored[l].score.aggregate;
              const double sr = scored[r].score.aggregate;
              if (sl != sr) {
                return sl > sr;  // higher score first
              }
              return scored[l].proposal.id < scored[r].proposal.id;
            });

  // Walk in priority order; admit if no edge to any already-selected.
  // Using a vector<bool> for membership (n is small per batch).
  std::vector<bool> selected(n, false);
  for (std::size_t k : by_priority) {
    bool blocked = false;
    for (std::size_t s = 0; s < n; ++s) {
      if (!selected[s]) {
        continue;
      }
      const std::size_t a = std::min(k, s);
      const std::size_t b = std::max(k, s);
      if (edge_set.find(EdgeKey(a, b)) != edge_set.end()) {
        blocked = true;
        break;
      }
    }
    if (!blocked) {
      selected[k] = true;
    }
  }

  // Return selected indices in ascending order — matches the
  // canonical ordering of BatchEvalResult / ConflictGraph nodes.
  std::vector<std::size_t> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (selected[i]) {
      out.push_back(i);
    }
  }
  return out;
}

std::vector<std::pair<std::size_t, std::size_t>> MakeEdgeList(
    const ConflictGraph& g)
{
  std::vector<std::pair<std::size_t, std::size_t>> out;
  out.reserve(g.edges.size());
  for (const auto& e : g.edges) {
    out.emplace_back(e.a, e.b);
  }
  return out;
}

}  // namespace drt::redesign
