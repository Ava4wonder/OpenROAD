// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.c — Read/Write footprints + stable DeltaId.
//
// The footprint types are the substrate for the OCC commit-eligibility
// invariant from v2_drt_redesign_plan.md §3:
//
//   A Delta is commit-eligible against snapshot S_{t+k} only if all
//   committed writes since S_t are disjoint from the Delta's declared
//   read footprint. The read footprint must include geometric DRC
//   halos AND any non-geometric state used during scoring. If the
//   footprint is invalidated, the Delta is re-evaluated or dropped.
//
// ReadFootprint is multi-domain on purpose. Routing's read footprint
// is not just geometry — it includes route guides, pin-access
// reservations, history-cost cells, marker state, same-net topology,
// and (later) timing/congestion proxy fields. V2.1 populates only the
// geometry and marker domains; later sub-phases fill the rest. The
// API has slots for all of them from day one so the wire format and
// commit-eligibility check don't need to change as proposers grow.

#pragma once

#include <cstdint>
#include <vector>

#include "Delta.h"

namespace drt::redesign {

using NetId = uint64_t;
using InstTermId = uint64_t;

// A stable identifier for a Delta. Derived from
// (region_id, proposer_id, attempt_index) — NOT from generation
// timestamp or thread id. The same proposer running against the same
// snapshot in the same region with the same attempt index produces the
// same DeltaId on every run, which is what §7 needs for canonical
// conflict-graph ordering.
struct DeltaId
{
  uint32_t region_id = 0;
  uint32_t proposer_id = 0;
  uint32_t attempt_index = 0;

  bool operator==(const DeltaId& o) const noexcept
  {
    return region_id == o.region_id && proposer_id == o.proposer_id
           && attempt_index == o.attempt_index;
  }
  bool operator<(const DeltaId& o) const noexcept
  {
    if (region_id != o.region_id) {
      return region_id < o.region_id;
    }
    if (proposer_id != o.proposer_id) {
      return proposer_id < o.proposer_id;
    }
    return attempt_index < o.attempt_index;
  }
};

// Bbox per layer that the Delta would write to.
struct WriteFootprint
{
  std::vector<Rect> shapes;
  std::vector<LayerNum> layers;

  // Extract the write footprint of a Delta variant. Each kind expands
  // its bbox by the appropriate halo (e.g. via enclosure for AddVia).
  static WriteFootprint Of(const Delta& d);
};

// Multi-domain read footprint. V2.1 populates GeometryDomain +
// MarkerDomain; the other domains exist as empty containers so V2.2+
// proposers can fill them without breaking wire format or
// commit-eligibility logic.
struct ReadFootprint
{
  // Shapes the proposer queried, expanded by the maximum DRC rule
  // halo for that family (PRL spacing reach, EOL extension window,
  // cut-spacing neighbourhood, via-enclosure expansion).
  struct GeometryDomain
  {
    std::vector<Rect> rects;
    std::vector<LayerNum> layers;
  } geometry;

  // Same-net topology depended on (e.g., the existing route tree).
  struct TopologyDomain
  {
    std::vector<NetId> nets;
  } topology;

  // Marker regions the proposer read (e.g., for repair targeting).
  struct MarkerDomain
  {
    std::vector<Rect> rects;
  } markers;

  // Route-guide corridors the proposer relied on.
  struct GuideDomain
  {
    std::vector<Rect> rects;
    std::vector<LayerNum> layers;
  } guides;

  // History-cost / congestion / timing-criticality cells the proposer
  // read during scoring or maze search. `fields_read` is a bitmask of
  // CostFieldKind so the eligibility check knows which fields matter.
  struct CostFieldDomain
  {
    enum CostFieldKind : uint8_t {
      None = 0,
      HistoryCost = 1u << 0,
      Congestion = 1u << 1,
      Timing = 1u << 2,
    };
    Rect region{};
    uint8_t fields_read = None;
  } cost_fields;

  // Pin-access reservations the proposer relied on.
  struct PinAccessDomain
  {
    std::vector<InstTermId> iterms;
  } pin_access;
};

// V2 ProposedDelta. Carries the Delta payload, the proposer-side
// metadata (source, worker_id, snapshot version it was generated
// against), the stable identifier, and the read/write footprints used
// for OCC commit-eligibility (§3) and conflict-graph construction
// (V2.4 §4.4). Forward-declared in Delta.h.
struct ProposedDelta
{
  Delta delta;
  DeltaSource source = DeltaSource::UnknownTest;
  int worker_id = -1;
  uint64_t snapshot_version = 0;
  DeltaId id{};
  ReadFootprint read_footprint{};
  WriteFootprint write_footprint{};
};

}  // namespace drt::redesign
