// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.a + V2.4.b — ConflictGraph builder.

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

// V2.4.b — Net conflict: both proposals have non-zero net_id and
// the values match. Zero is the "unset" sentinel and never matches
// (otherwise unset proposals would all collide with each other).
bool HasNetConflict(const ProposedDelta& a,
                    const ProposedDelta& b) noexcept
{
  return a.proposal_net_id != 0
         && a.proposal_net_id == b.proposal_net_id;
}

// V2.4.b — Read-write asymmetric overlap on the same layer: does
// any rect in `read.geometry` overlap any shape in `write` where
// the layers match? Same overlap rule as the geometry check
// (inclusive of edge-touching).
bool HasReadOverWriteSameLayer(const ReadFootprint::GeometryDomain& read,
                               const WriteFootprint& write) noexcept
{
  const std::size_t nr = read.rects.size();
  const std::size_t nw = write.shapes.size();
  for (std::size_t i = 0; i < nr; ++i) {
    for (std::size_t j = 0; j < nw; ++j) {
      if (read.layers[i] == write.layers[j]
          && BboxesOverlap(read.rects[i], write.shapes[j])) {
        return true;
      }
    }
  }
  return false;
}

// V2.4.b — Symmetric ReadWrite check: A reads B's writes OR B
// reads A's writes. Either direction means committing both is
// unsafe.
bool HasReadWriteConflict(const ProposedDelta& a,
                          const ProposedDelta& b) noexcept
{
  return HasReadOverWriteSameLayer(a.read_footprint.geometry,
                                   b.write_footprint)
         || HasReadOverWriteSameLayer(b.read_footprint.geometry,
                                      a.write_footprint);
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
  //
  // Per-pair precedence: Geometry > Net > ReadWrite. First hit wins
  // (one edge per pair); the kind is informational for the dump and
  // future log readers — MIS only needs the pair-incompatibility
  // signal regardless of which kind triggered it.
  for (std::size_t i = 0; i < n; ++i) {
    const auto& pa = set.proposals[i];
    for (std::size_t j = i + 1; j < n; ++j) {
      const auto& pb = set.proposals[j];

      ConflictEdge e;
      e.a = i;
      e.b = j;

      // (1) Geometry. SAFETY INVARIANT (Footprint.h): unknown=true
      // means the write footprint is incomplete, so this Delta MUST
      // conflict with every other proposal. V2.4's commit path then
      // rejects it from the parallel batch via the conflict-loser
      // tagging.
      const bool unknown_either
          = pa.write_footprint.unknown || pb.write_footprint.unknown;
      if (unknown_either
          || HasGeometryOverlapSameLayer(pa.write_footprint,
                                         pb.write_footprint)) {
        e.kind = ConflictKind::Geometry;
        g.edges.push_back(e);
        continue;
      }

      // (2) Net. Both proposals have non-zero matching net_id.
      if (HasNetConflict(pa, pb)) {
        e.kind = ConflictKind::Net;
        g.edges.push_back(e);
        continue;
      }

      // (3) ReadWrite. Either A reads B's writes or B reads A's.
      if (HasReadWriteConflict(pa, pb)) {
        e.kind = ConflictKind::ReadWrite;
        g.edges.push_back(e);
        continue;
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
