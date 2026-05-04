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
struct FamilyCellStats
{
  std::size_t supported = 0;
  std::size_t supported_explicit = 0;
  std::size_t supported_unknown = 0;
  std::size_t fallback = 0;
};

struct PerDesignStats
{
  std::string design;  // ClipMeta.design or RuleDeckProvenance hint

  // From RuleDeck::Coverage (rolled up across all .ruledeck files for
  // this design).
  std::size_t total_input = 0;
  std::size_t supported = 0;
  std::size_t supported_explicit = 0;
  std::size_t supported_unknown = 0;
  std::size_t fallback = 0;
  std::size_t unsupported = 0;

  // Sum of LayerConflictsSeen counters reported via provenance for
  // every process. Currently we don't ship this counter into the file;
  // see TODO in CollectAudit. Stays 0 until added.
  std::uint64_t layer_conflicts_seen = 0;

  // Per-family breakdown.
  std::map<RuleFamily, FamilyCellStats> by_family;

  // Layer set discovered in the rule deck (Explicit only).
  std::map<std::int16_t, std::size_t> rules_per_layer;

  // Clip-side rollup (from clip-dump files for this design).
  std::size_t clips_seen = 0;
  std::size_t clips_marker_present = 0;
};

struct AuditReport
{
  std::map<std::string, PerDesignStats> per_design;

  // Aggregate cross-design view, derived from per_design at
  // serialization time.
  std::size_t designs() const noexcept { return per_design.size(); }
};

// Build/extend an AuditReport from one rule-deck dump.
void IngestRuleDeck(const RuleDeck& deck,
                    const RuleDeckProvenance& prov,
                    const std::string& design_hint,
                    AuditReport* out);

// Build/extend an AuditReport from one clip-dump file's ClipRecords.
void IngestClipRecords(const std::vector<ClipRecord>& clips,
                       const std::string& design_hint,
                       AuditReport* out);

// Render to a structured text report on the given stream.
void RenderReport(const AuditReport& report, std::ostream& os);

}  // namespace drt::redesign::legality
