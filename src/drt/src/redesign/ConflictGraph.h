// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.a + V2.4.b — ConflictGraph data type + intra-batch conflict
// detection. Per v2_drt_redesign_plan.md §4.4, two Deltas conflict if
// they cannot both be applied. The plan splits conflicts into three
// kinds:
//
//   Geometry  — write footprints overlap on the same layer (a short
//               or spacing violation if both committed). Also fires
//               when either footprint has unknown=true (per
//               Footprint.h SAFETY INVARIANT).
//   Net       — Two Deltas mutate the same net (only one route per
//               net per commit). Detected via
//               ProposedDelta::proposal_net_id; both must be
//               non-zero and equal.
//   ReadWrite — Delta A's read footprint overlaps Delta B's write
//               footprint (or symmetrically). A would have made a
//               different decision under B's outcome; committing
//               both is unsafe.
//
// One edge per pair, with kind set to the FIRST detected by the
// precedence Geometry > Net > ReadWrite (deterministic ordering;
// the strongest reason wins). MIS solvers (V2.4.c) only need the
// pair-incompatibility signal; the kind is informational for the
// shadow dump and future log readers.
//
// V2.4.a/b SCOPE — type definitions + intra-batch detection only.
// The MIS solver (V2.4.c) and the multi-Delta commit path (V2.4.d)
// consume this graph; cross-worker barriering (V2.4.e) and shadow
// integration (V2.4.f) come later.
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

// V2.4.a/b — conflict graph over the proposals in `set`.
//
// Preconditions:
//   * `set.proposals` SHOULD be in DeltaId-sorted order. This matches
//     PhysicalState::batch_eval's canonical ordering, so callers
//     wiring the two together get the indices to align for free. If
//     the input is unsorted, BuildConflictGraph still produces a
//     well-formed graph but the caller must be careful when
//     correlating to BatchEvalResult outcomes.
//
// Per-pair check (precedence Geometry > Net > ReadWrite — first
// hit wins, one edge per pair):
//   1. Geometry — either footprint unknown=true, OR some pair
//      (a, b) of write-footprint shapes overlaps on the same layer
//      (inclusive of edge-touching, matching
//      SyntheticOracle::BboxOverlap).
//   2. Net — both proposals have non-zero proposal_net_id and the
//      values are equal.
//   3. ReadWrite — A.read_footprint.geometry intersects
//      B.write_footprint on the same layer (or symmetrically B
//      reads ∩ A writes).
//
// Time: O(N^2 * S^2) where N = batch size, S = max shapes per
// footprint. Naive nested loop — fine for K ≤ 64 proposals from a
// single worker. V2.4.e (cross-worker barrier) will move to a
// spatial index when N grows.
ConflictGraph BuildConflictGraph(const ProposalSet& set);

}  // namespace drt::redesign
