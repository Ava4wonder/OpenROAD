// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.a — On-disk format for a translated RuleDeck.
//
// The clip dump (ClipDump.h) captures runtime per-clip state. For E2 we
// also need to capture the design's TRANSLATED rule deck so the offline
// audit can:
//   1) Reconstruct the per-design Coverage statistics without re-running
//      OpenROAD against the LEF/DEF.
//   2) Cross-reference per-clip layers against per-rule layer_filter to
//      tag each clip with a per-family in-scope mask.
//
// Format version is independent from kClipDumpVersion. File layout:
//   Header (once): magic 'RDDP' (u32 LE) + version (u32 LE)
//   Body (once):
//     Coverage record:
//       total_input          (u64)
//       supported            (u64)
//       supported_explicit   (u64)
//       supported_unknown    (u64)
//       fallback             (u64)
//       unsupported          (u64)
//     rule_count             (u32)
//     For each rule:
//       family               (u8)   RuleFamily enum value
//       coverage             (u8)   RuleCoverage enum value
//       layer_knownness      (u8)   LayerKnownness enum value
//       has_layer_filter     (u8)   1 if std::optional<int16_t> set
//       layer_filter         (i16)  present iff has_layer_filter == 1
//       halo                 (i32)
//       tag                  length-prefixed string (u32 + bytes)
//       params (only when coverage == Supported):
//         MetalShortConfig: 0 bytes
//         PrlSpacingConfig: 2 × i32 (min_spacing, prl_threshold)
//         EolSpacingConfig: 3 × i32 (eol_width_threshold, eol_spacing,
//                                    eol_within)
//         CutSpacingConfig: 1 × i32 (min_spacing)
//
// The whole body is NOT length-prefixed because there's exactly one
// deck per file. Multiple decks (e.g. one per process) live in separate
// files keyed by PID, mirroring the clip-dump convention.

#pragma once

#include <cstdint>
#include <iosfwd>

#include "RuleDeck.h"

namespace drt::redesign::legality {

inline constexpr std::uint32_t kRuleDeckDumpMagic = 0x50444452u;  // 'RDDP'
inline constexpr std::uint32_t kRuleDeckDumpVersion = 0x00010000u;

void WriteRuleDeck(std::ostream& os, const RuleDeck& deck);

// Reads header + body. Returns true on success. On failure (bad magic,
// version mismatch, truncated stream, or invalid family/coverage byte)
// leaves *out in an indeterminate state and returns false.
bool ReadRuleDeck(std::istream& is, RuleDeck* out);

}  // namespace drt::redesign::legality
