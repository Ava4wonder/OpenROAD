// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.5 + V2.2.a.0 — best-effort CSV dump of shadow comparisons.
//
// V2.1.e.5 shipped marker-only.  V2.2.a.0 generalises to per-entity:
// every shadow comparison (marker / route_shape / guide / blockage /
// pin_access / cost) records into one combined CSV with an entity
// column. This way, all V2.2.a sub-commits share the same evidence
// pipeline and a single A/B run produces a uniform record across
// entities.
//
// Activation:
//   OPENROAD_OVERLAY_DUMP_HASHES=1   enable
//   OPENROAD_OVERLAY_DUMP_PATH=/...  override default file path
//   default path: /tmp/openroad-overlay-hashes/<pid>.csv
//
// Discipline (preserved across V2.1.e.5 → V2.2.a.0):
//   - Diagnostic-only. Never throws, never blocks routing, never
//     alters the legacy authoritative path. File I/O failures are
//     logged ONCE to stderr and dumping is then silently disabled
//     for the rest of the process.
//   - Multi-threaded callers serialise via internal mutex.
//   - CSV columns:
//       pid,tid,seqno,entity,query_kind,xmin,ymin,xmax,ymax,layer,
//       legacy_hash,overlay_hash,match,legacy_count,overlay_count
//     `entity` is one of "marker","route_shape","guide","blockage",
//     "pin_access","cost". `query_kind` is the underlying API
//     (e.g., "queryMarker","query","queryGuide"). `layer` is empty
//     for layer-less queries (markers).
//   - `seqno` is per-process shadow-call sequence number, NOT DR
//     iteration / checkerboard / worker iteration.
//   - On process exit (atexit), a one-line summary lists per-entity
//     counts: "shadow summary: total_calls=N total_mismatches=M
//     (marker: c/m, route_shape: c/m, ...)".

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "GeometryView.h"  // Rect

namespace drt::redesign::overlay {

class ShadowDump
{
 public:
  enum class Entity : std::uint8_t {
    Marker = 0,
    RouteShape = 1,
    Guide = 2,
    Blockage = 3,
    PinAccess = 4,
    Cost = 5,
    // Keep last:
    kCount,
  };

  // Generic record. Per-entity convenience wrappers below.
  struct ComparisonRecord
  {
    Entity entity;
    const char* query_kind;  // e.g. "queryMarker", "query"
    Rect box{};
    std::optional<std::int32_t> layer;
    std::uint64_t legacy_hash = 0;
    std::uint64_t overlay_hash = 0;
    std::size_t legacy_count = 0;
    std::size_t overlay_count = 0;
  };

  static void Record(const ComparisonRecord& rec);

  // Convenience wrappers used by the existing shadow comparators.
  // V2.1.e.5 marker API preserved unchanged — callers see no diff.
  static void RecordMarkerComparison(std::uint64_t legacy_hash,
                                     std::uint64_t overlay_hash,
                                     std::size_t legacy_count,
                                     std::size_t overlay_count,
                                     const Rect& drc_box);

  // V2.2.a.1.shadow — route-shape comparison wrapper.
  static void RecordRouteShapeComparison(std::uint64_t legacy_hash,
                                         std::uint64_t overlay_hash,
                                         std::size_t legacy_count,
                                         std::size_t overlay_count,
                                         const Rect& box,
                                         std::int32_t layer);

  // Process-wide aggregates (across all entities).
  static std::uint64_t comparison_count() noexcept;
  static std::uint64_t mismatch_count() noexcept;

  // Per-entity aggregates.
  static std::uint64_t comparison_count_for(Entity e) noexcept;
  static std::uint64_t mismatch_count_for(Entity e) noexcept;

  // Test-only hook. Resets all internal state so unit tests can
  // exercise the env-var paths in isolation. Not for production use.
  static void ResetForTest();
};

}  // namespace drt::redesign::overlay
