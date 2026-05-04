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
#include <map>
#include <string>

#include "RuleDeck.h"

namespace drt::redesign::legality {

inline constexpr std::uint32_t kRuleDeckDumpMagic = 0x50444452u;  // 'RDDP'
// v1.1: adds provenance block between header and coverage.
// v1.2 (0x00010002): adds session_id, design, pdk to provenance so the
//   audit can join clip-dump and rule-deck artifacts on session_id and
//   surface the design name uniformly across both sides.
inline constexpr std::uint32_t kRuleDeckDumpVersion = 0x00010002u;

// Per amendment-derived requirements from P2.2.e.2.b review:
// provenance lives INSIDE the file (not just in the filename) so audit
// can be done on a moved file.
struct RuleDeckProvenance
{
  // Process-local session identifier matching ClipMeta.session_id from
  // the same FlexGCWorker hook. Audit joins on this. Currently
  // populated as getpid().
  std::uint64_t session_id = 0;
  // Bumped by FlexConstraintTranslator when its translation logic
  // changes meaningfully. Different from the file format version.
  std::uint32_t translator_version = 1;
  std::uint32_t pid = 0;
  // Design + PDK names as supplied via DRT_DUMP_DESIGN / DRT_DUMP_PDK.
  // Mirror the same fields in ClipMeta so audit shows one design key
  // for both rule-deck and clip artifacts in the same session.
  std::string design;
  std::string pdk;
  // Unix epoch seconds at capture time. 0 if not set.
  std::int64_t capture_timestamp = 0;
  // Optional git SHAs (env-var supplied at process start). Empty when
  // not detectable.
  std::string openroad_git_sha;
  std::string redesign_git_sha;

  // Traversal instrumentation per yellow flag 1 of the P2.2.e.2.b
  // review: separates "translator scope is too narrow" from "we walked
  // the wrong tree."
  std::uint32_t layers_walked = 0;
  std::uint32_t constraints_seen = 0;
  // Count of seen constraints keyed by frConstraintTypeEnum value
  // (cast to u32). Allows offline auditor to spot families that
  // upstream emits but the translator drops as Unsupported.
  std::map<std::uint32_t, std::uint32_t> per_type_counts;
};

// Format:
//   Header: magic + version
//   Provenance:
//     translator_version    (u32)
//     pid                   (u32)
//     capture_timestamp     (i64)
//     openroad_git_sha      (length-prefixed string)
//     redesign_git_sha      (length-prefixed string)
//     layers_walked         (u32)
//     constraints_seen      (u32)
//     per_type_count_n      (u32)
//     per type: type_id u32 + count u32
//   Coverage + rules: same as v1.0 body.
void WriteRuleDeck(std::ostream& os,
                   const RuleDeck& deck,
                   const RuleDeckProvenance& prov);

// Reads header + provenance + body. Returns true on success. On
// failure leaves *out / *prov_out in an indeterminate state.
bool ReadRuleDeck(std::istream& is,
                  RuleDeck* out,
                  RuleDeckProvenance* prov_out);

}  // namespace drt::redesign::legality
