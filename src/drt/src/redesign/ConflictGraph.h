// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.a — ConflictGraph data type + intra-batch geometry conflict
// detection. Per v2_drt_redesign_plan.md §4.4, two Deltas conflict if
// they cannot both be applied. The plan splits conflicts into three
// kinds; this commit ships geometry only:
//
//   Geometry  — write footprints overlap on the same layer (a short
//               or spacing violation if both committed).
//   Net       — V2.4.b. Two Deltas mutate the same net.
//   ReadWrite — V2.4.b. Delta A's read footprint overlaps Delta B's
//               write footprint; A would have made a different
//               decision under B's outcome.
//
// V2.4.a SCOPE — type definitions + geometry detection only. The MIS
// solver (V2.4.c) and the multi-Delta commit path (V2.4.d) consume
// this graph; cross-worker barriering (V2.4.e) and shadow integration
// (V2.4.f) come later.
//
// Determinism contract: BuildConflictGraph re-orders nothing — it
// expects the input ProposalSet to already be in canonical
// (DeltaId-sorted) order, which matches PhysicalState::batch_eval's
// output ordering. Edges are emitted in (a, b, kind) sorted order
// where a < b. The same input must produce a byte-identical graph
// across runs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BatchEval.h"
#include "Footprint.h"

namespace drt::redesign {

enum class ConflictKind : std::uint8_t {
  // V2.4.a — write footprints overlap on the same layer. Also emitted
  // when one of the two Deltas has WriteFootprint::unknown=true (per
  // Footprint.h SAFETY INVARIANT: such Deltas conflict with everything
  // because their writes are not exhaustively known).
  Geometry = 0,
  // V2.4.b — same net (two Deltas write to the same NetId). Reserved
  // here so consumers can lock in the encoding now; not emitted by
  // V2.4.a.
  Net = 1,
  // V2.4.b — Delta A's read footprint intersects Delta B's write
  // footprint. Reserved here; not emitted by V2.4.a.
  ReadWrite = 2,
};

struct ConflictEdge
{
  // Indices into ConflictGraph::node_ids. Canonical: a < b.
  std::size_t a = 0;
  std::size_t b = 0;
  ConflictKind kind = ConflictKind::Geometry;

  bool operator==(const ConflictEdge& o) const noexcept
  {
    return a == o.a && b == o.b && kind == o.kind;
  }
};

struct ConflictGraph
{
  // Per-node DeltaId, in the same canonical order as the input
  // ProposalSet (which the caller is expected to have DeltaId-sorted).
  // node_ids[i] is the DeltaId of the i-th node; consumers correlate
  // back to the original ProposedDelta via this id.
  std::vector<DeltaId> node_ids;
  // Edges in (a, b, kind) sorted order. Canonical for determinism.
  std::vector<ConflictEdge> edges;
};

// V2.4.a — geometry-only conflict graph over the proposals in `set`.
//
// Preconditions:
//   * `set.proposals` SHOULD be in DeltaId-sorted order. This matches
//     PhysicalState::batch_eval's canonical ordering, so callers
//     wiring the two together get the indices to align for free. If
//     the input is unsorted, BuildConflictGraph still produces a
//     well-formed graph but the caller must be careful when
//     correlating to BatchEvalResult outcomes.
//
// Geometry-conflict rule:
//   * If either proposal's write_footprint.unknown == true, conflict
//     (safety invariant from Footprint.h).
//   * Otherwise, conflict iff some pair (i, j) of write-footprint
//     shapes satisfies layers[i] == layers[j] AND shapes[i] overlaps
//     shapes[j] (inclusive of edge-touching, matching
//     SyntheticOracle's BboxOverlap convention).
//
// Time: O(N^2 * S^2) where N = batch size, S = max shapes per
// footprint. V2.4.a uses the naive nested loop — fine for K ≤ 64
// proposals from a single worker. V2.4.e (cross-worker barrier) will
// move to a spatial index when N grows.
ConflictGraph BuildConflictGraph(const ProposalSet& set);

}  // namespace drt::redesign
