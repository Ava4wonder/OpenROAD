// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.c — Offline E2 audit reporting types + reducer.
//
// The audit ingests one or more (clip-dump, rule-deck-dump) pairs and
// produces a structured per-design / per-family / per-layer breakdown.
// Per the closeout review of R15: per-design and per-family granularity
// is mandatory; aggregate-only reports would hide cross-design
// irregularities (especially supported_unknown drift and
// LayerConflictsSeen on designs other than gcd).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ClipDump.h"
#include "NormalizedRule.h"
#include "RuleDeck.h"
#include "RuleDeckDump.h"

namespace drt::redesign::legality {

// Aggregate metrics for one (design, family) cell.
// Tier-aware per P2.2.e.2.c.4: exact and conservative are tracked
// separately so reporting never blurs faithful representation with
// safe overapproximation.
struct FamilyCellStats
{
  std::size_t supported_exact = 0;
  std::size_t supported_exact_explicit = 0;
  std::size_t supported_exact_unknown = 0;
  std::size_t supported_conservative = 0;
  std::size_t supported_conservative_explicit = 0;
  std::size_t supported_conservative_unknown = 0;
  std::size_t fallback = 0;
};

// Per-session join status. Each session is one openroad-process
// FlexGCWorker run.
enum class JoinStatus : std::uint8_t {
  // Both .ruledeck and clip-dump files for this session were found
  // and parsed.
  Joined,
  // Only the .ruledeck side was found (clip dump missing or empty).
  RuleDeckOnly,
  // Only the clip-dump side was found.
  ClipsOnly,
};

struct SessionStats
{
  std::uint64_t session_id = 0;
  std::string design;
  std::string pdk;
  JoinStatus join_status = JoinStatus::ClipsOnly;

  // Rule-deck side. Tier-aware per P2.2.e.2.c.4.
  std::size_t total_input = 0;
  std::size_t supported_exact = 0;
  std::size_t supported_exact_explicit = 0;
  std::size_t supported_exact_unknown = 0;
  std::size_t supported_conservative = 0;
  std::size_t supported_conservative_explicit = 0;
  std::size_t supported_conservative_unknown = 0;
  std::size_t fallback = 0;
  std::size_t unsupported = 0;
  std::uint64_t layer_conflicts_seen = 0;
  std::map<RuleFamily, FamilyCellStats> by_family;
  // Per-variant within family. Variant key is NormalizedRule.tag,
  // e.g. "frSpacingTablePrlConstraint". Splits fallback into the
  // specific upstream subclasses driving it so reporting shows which
  // variants would benefit from semantic widening (Step B2 onward).
  std::map<RuleFamily, std::map<std::string, FamilyCellStats>>
      by_family_variant;
  std::map<std::int16_t, std::size_t> rules_per_layer;

  // Clip-side.
  std::size_t clips_seen = 0;
  std::size_t clips_marker_present = 0;
};

struct AuditReport
{
  // Keyed by session_id (one entry per FlexGCWorker process).
  std::map<std::uint64_t, SessionStats> per_session;

  std::size_t sessions() const noexcept { return per_session.size(); }
};

// Build/extend an AuditReport from one rule-deck dump. Joined into the
// session keyed by prov.session_id.
void IngestRuleDeck(const RuleDeck& deck,
                    const RuleDeckProvenance& prov,
                    AuditReport* out);

// Build/extend an AuditReport from one clip-dump file's ClipRecords.
// Each record carries its own session_id.
void IngestClipRecords(const std::vector<ClipRecord>& clips,
                       AuditReport* out);

// Mark sessions that received content from both sides as Joined; the
// rest stay RuleDeckOnly or ClipsOnly. Idempotent.
void FinalizeJoinStatus(AuditReport* out);

// Render to a structured text report on the given stream.
void RenderReport(const AuditReport& report, std::ostream& os);

}  // namespace drt::redesign::legality
