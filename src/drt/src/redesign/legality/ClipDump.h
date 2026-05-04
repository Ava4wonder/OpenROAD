// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.a — Clip dump format definition (no FlexGCWorker hook yet).
//
// This header defines the on-disk format for `FlexGCWorker` clip dumps
// used by the P2.2.e Mode B validation flow. Per the structural
// amendments to P2.2.e:
//   Layer A (ClipMeta + candidates + context) — raw clip state.
//   Layer B (ProjectedUpstreamLabel)          — derived labels.
//
// **CRITICAL semantic note on Layer B:**
// `FlexGCWorker` does NOT produce native per-candidate verdicts. Its
// output is a list of `frMarker` objects covering bounding boxes within
// the clip. The per-candidate labels in `ProjectedUpstreamLabel` are
// **PROJECTED** from those markers by the hook code (e.g. by counting
// markers whose bbox intersects the candidate's bbox). The projection
// is OUR construction, NOT something upstream emits.
//
// This naming distinction matters in P2.2.e.4 (Mode B comparison): when
// we say "oracle disagrees with upstream," we are really saying "oracle
// disagrees with our marker→candidate projection." A mismatch could
// stem from a flawed projection rather than from oracle false-negatives.
// The `projected_` prefix makes this visible at every read site.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "CpuDrcOracle.h"

namespace drt::redesign::legality {

inline constexpr std::uint32_t kClipDumpMagic = 0x44434450u;  // 'DCDP'
// v1.1 (0x00010001): adds session_id to ClipMeta. Old v1.0 readers
// reject via exact-version check.
inline constexpr std::uint32_t kClipDumpVersion = 0x00010001u;
inline constexpr std::uint32_t kMaxStringLen = 4096u;
inline constexpr std::uint32_t kMaxShapeCount = 1u << 24;  // sanity cap

// Layer A: raw clip state.
struct ClipMeta
{
  std::uint64_t clip_id = 0;
  // Process-local identifier matching RuleDeckProvenance.session_id
  // emitted by the same FlexGCWorker hook. Audit harness joins clip
  // dumps and rule-deck dumps on this field. Currently populated as
  // getpid() at hook init; one session per openroad process.
  std::uint64_t session_id = 0;
  std::string design;
  std::string pdk;
  std::uint64_t tech_hash = 0;             // populated by future commits
  std::uint64_t rule_deck_fingerprint = 0; // populated by future commits
  std::int32_t clip_x1 = 0, clip_y1 = 0, clip_x2 = 0, clip_y2 = 0;
  std::int32_t route_x1 = 0, route_y1 = 0, route_x2 = 0, route_y2 = 0;
};

// Layer B: per-candidate label PROJECTED from upstream markers.
// See top-of-file note on "projected" semantics.
struct ProjectedUpstreamLabel
{
  // Number of upstream markers (frMarker) whose bbox intersects the
  // corresponding candidate's bbox. Computed by the dump hook; not a
  // native upstream output.
  std::uint32_t projected_marker_count = 0;
  // Boolean reduction: 1 iff projected_marker_count == 0. Sense matches
  // CpuDrcOracle's Verdict::legal.
  std::uint8_t projected_legal = 1;
  // Set if any marker contributing to projected_marker_count was also
  // counted toward another candidate (i.e. its bbox overlapped multiple
  // candidates). Mode B should treat clips with many ambiguous mappings
  // as low-confidence rather than as oracle failures.
  std::uint8_t projection_ambiguous = 0;
};

struct ClipRecord
{
  ClipMeta meta;
  std::vector<Shape> candidates;
  std::vector<Shape> context;
  // Size == candidates.size(); 1:1 alignment.
  std::vector<ProjectedUpstreamLabel> labels;
};

// File layout:
//   Header (once): magic (u32 LE) + version (u32 LE)
//   Records (sequence): record_length (u32 LE) + record body bytes
// All fields are little-endian. Big-endian hosts unsupported in v1.0.

// Write the file header (magic + version) to an empty output stream.
void WriteHeader(std::ostream& os);

// Read the file header. Returns true on success and writes the file's
// version into *version_out. Returns false if the magic is wrong or
// the stream is too short.
bool ReadHeader(std::istream& is, std::uint32_t* version_out);

// Append one record to the stream after the header. Throws on stream
// failure (callers in the FlexGCWorker hook MUST wrap in try/catch and
// not propagate per guardrail 5).
void WriteRecord(std::ostream& os, const ClipRecord& record);

// Read one record from the stream. Returns true on success. Returns
// false on clean EOF (no more records) or on a format violation. The
// `*out` value is left in an indeterminate state on false return.
// Callers should compare the version returned from ReadHeader against
// kClipDumpVersion before calling ReadRecord; this function does not
// re-validate the version.
bool ReadRecord(std::istream& is, ClipRecord* out);

}  // namespace drt::redesign::legality
