// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.a — ConflictGraph builder. Geometry-only.

#include "ConflictGraph.h"

#include <algorithm>

namespace drt::redesign {

namespace {

// Inclusive overlap (touching edges count as overlap). Matches
// SyntheticOracle::BboxOverlap so a proposal that the legality oracle
// would mark as overlapping a sibling is also flagged here as a
// conflict — keeps the two layers semantically consistent.
bool BboxesOverlap(const Rect& a, const Rect& b) noexcept
{
  return !(a.ur.x < b.ll.x || b.ur.x < a.ll.x || a.ur.y < b.ll.y
           || b.ur.y < a.ll.y);
}

// Pairwise check: do A and B have any pair of (shape, layer) entries
// that overlap on the same layer? Returns on first hit (we only need
// the existence answer, not the count).
//
// Caller is responsible for the unknown-flag short-circuit; this
// helper assumes both footprints are exhaustively known.
bool HasGeometryOverlapSameLayer(const WriteFootprint& a,
                                 const WriteFootprint& b) noexcept
{
  const std::size_t na = a.shapes.size();
  const std::size_t nb = b.shapes.size();
  for (std::size_t i = 0; i < na; ++i) {
    for (std::size_t j = 0; j < nb; ++j) {
      if (a.layers[i] == b.layers[j]
          && BboxesOverlap(a.shapes[i], b.shapes[j])) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

ConflictGraph BuildConflictGraph(const ProposalSet& set)
{
  ConflictGraph g;
  const std::size_t n = set.proposals.size();
  g.node_ids.reserve(n);
  for (const auto& p : set.proposals) {
    g.node_ids.push_back(p.id);
  }

  // Naive O(N^2) pairwise scan. K ≤ 64 expected for a single-worker
  // batch in V2.4.a–d; cross-worker N (V2.4.e) may need a spatial
  // index.
  for (std::size_t i = 0; i < n; ++i) {
    const auto& wfa = set.proposals[i].write_footprint;
    for (std::size_t j = i + 1; j < n; ++j) {
      const auto& wfb = set.proposals[j].write_footprint;

      // SAFETY INVARIANT (Footprint.h): unknown=true means the write
      // footprint is incomplete, so this Delta MUST conflict with
      // every other proposal. V2.4's commit path then rejects it
      // from the parallel batch via the conflict-loser tagging.
      const bool unknown_either = wfa.unknown || wfb.unknown;
      if (unknown_either || HasGeometryOverlapSameLayer(wfa, wfb)) {
        ConflictEdge e;
        e.a = i;
        e.b = j;
        e.kind = ConflictKind::Geometry;
        g.edges.push_back(e);
      }
    }
  }

  // Edges already in (a, b) sorted order from the nested-loop
  // construction. Sort defensively in case future code paths emit
  // edges out of order — cheap (E log E) and locks in the
  // determinism contract regardless of how the loops evolve.
  std::sort(g.edges.begin(), g.edges.end(),
            [](const ConflictEdge& l, const ConflictEdge& r) {
              if (l.a != r.a) {
                return l.a < r.a;
              }
              if (l.b != r.b) {
                return l.b < r.b;
              }
              return static_cast<std::uint8_t>(l.kind)
                     < static_cast<std::uint8_t>(r.kind);
            });

  return g;
}

}  // namespace drt::redesign
