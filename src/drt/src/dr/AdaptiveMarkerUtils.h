// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 1 — stub declarations for marker interpretation
// utilities used by AdaptiveMarkerModel. Implementations land in
// later patches; Patch 1 provides linkage-only stubs so the model file
// compiles.

#pragma once

#include <vector>

#include "dr/AdaptiveMarkerTypes.h"

namespace drt {

class frConstraint;
class frMarker;
class frNet;

// Conservative initial classification: switches on the constraint typeId.
// Patch 1 returns AdaptiveRuleClass::Other for everything — the real
// implementation lands in Patch 2 alongside marker observation.
AdaptiveRuleClass classifyConstraint(const frConstraint* constraint);

// Extract net owners from a marker. Patch 1 returns an empty vector;
// Patch 2 populates with frcNet owners; later patches add inst-term /
// pin-fig / block-object owners.
std::vector<frNet*> extractMarkerNets(const frMarker& marker);

// Per-rule severity weight used by the model's heat updates and by the
// AdaptiveWorkerStats weighted_marker_score field.
int getRuleWeight(AdaptiveRuleClass rule);

// Inflate the marker bbox by a rule-appropriate amount in DBU. Used by
// the model to attribute heat to nearby tiles, not just the marker's
// own footprint. Patch 1 returns the marker's bbox unchanged.
odb::Rect getRuleAwareInflatedBox(const frMarker& marker,
                                  AdaptiveRuleClass rule,
                                  int default_bloat_dbu);

}  // namespace drt
