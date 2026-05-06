// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.5 — best-effort CSV dump of shadow marker-hash comparisons.
//
// Activation:
//   OPENROAD_OVERLAY_DUMP_HASHES=1   enable
//   OPENROAD_OVERLAY_DUMP_PATH=/...  override default file path
//   default path: /tmp/openroad-overlay-hashes/<pid>.csv
//
// Discipline (per V2.1.e.5 review):
//   - Diagnostic-only. Never throws, never blocks routing, never
//     alters the legacy frMarker* path. File I/O failures are
//     logged ONCE to stderr and dumping is then silently disabled
//     for the rest of the process — the router still finishes and
//     produces identical output.
//   - Multi-threaded callers serialise via an internal mutex. The
//     overhead is acceptable because A/B with dumping enabled is a
//     diagnostic mode, not a perf-measurement mode. Per-thread file
//     sharding is a future optimisation if needed.
//   - CSV columns: pid,tid,seqno,xmin,ymin,xmax,ymax,legacy_hash,
//     overlay_hash,match,legacy_count,overlay_count.
//   - `seqno` is a per-process shadow-call sequence number, NOT a
//     DR search-repair iteration index, NOT a checkerboard batch
//     id, NOT a worker iteration. Just a monotonic counter so rows
//     can be ordered.
//   - On process exit (atexit), a one-line summary is emitted to
//     stderr: total comparisons + mismatch count.

#pragma once

#include <cstddef>
#include <cstdint>

#include "GeometryView.h"  // Rect

namespace drt::redesign::overlay {

class ShadowDump
{
 public:
  // Best-effort record of a shadow comparison. Returns nothing — the
  // caller must not be able to take a behaviour-altering branch on
  // I/O state.
  static void RecordMarkerComparison(uint64_t legacy_hash,
                                     uint64_t overlay_hash,
                                     std::size_t legacy_count,
                                     std::size_t overlay_count,
                                     const Rect& drc_box);

  // Process-wide comparison and mismatch counters. Updated for every
  // shadow comparison regardless of whether dumping is enabled —
  // the atexit summary line uses these.
  static std::uint64_t comparison_count() noexcept;
  static std::uint64_t mismatch_count() noexcept;

  // Test-only hook. Resets all internal state so unit tests can
  // exercise the env-var paths in isolation. Not for production use.
  static void ResetForTest();
};

}  // namespace drt::redesign::overlay
