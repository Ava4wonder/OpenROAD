// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.b — Capture a translated RuleDeck from a live frTechObject.
// Third bridge file (after FlexConstraintTranslator.cpp and
// GcWorkerClipBuilder.cpp). Per the bridge conventions documented in
// GcWorkerClipBuilder.cpp, this is the only file in legality/ allowed
// to include frTechObject.h / frLayer.h.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "RuleDeck.h"
#include "RuleDeckDump.h"

namespace drt {
class frTechObject;
}  // namespace drt

namespace drt::redesign::legality {

// Counters populated during the walk. Distinct from RuleDeck::Coverage
// which tracks translator outcomes; these track upstream traversal.
// "Constraints translated by RuleCoverage" = RuleDeck::Coverage,
// reported separately so we can distinguish "we walked too narrowly"
// from "translator scope is too narrow."
struct CaptureCounters
{
  std::uint32_t layers_walked = 0;
  std::uint32_t constraints_seen = 0;
  // Keyed by frConstraintTypeEnum value (cast to u32). Honest reporting
  // metric per the P2.2.e.2.b yellow-flag-1 review.
  std::map<std::uint32_t, std::uint32_t> per_type_counts;
};

// Walk tech, gather every constraint from the per-layer accessors we
// know about (frShortConstraint, frSpacingConstraint,
// frSpacingSamenetConstraint, frSpacingEndOfLineConstraint,
// frCutSpacingConstraint, frSpacingTableInfluenceConstraint, and the
// LEF58 spacing/cut variants), and run them through
// FlexConstraintTranslator. Counters reflect what was walked, not what
// landed in Coverage::supported.
//
// This routine SCOPE is documented honestly:
//   We walk constraints reachable through frLayer accessors. Constraints
//   stored elsewhere (e.g. on frViaDef, on frTechObject directly) are
//   NOT walked in v1. If later coverage looks suspiciously low, the
//   per-type counter shows whether we walked enough; if a known type
//   has 0 count, the walk is incomplete.
RuleDeck CaptureRuleDeck(const ::drt::frTechObject* tech,
                         CaptureCounters* counters);

}  // namespace drt::redesign::legality
