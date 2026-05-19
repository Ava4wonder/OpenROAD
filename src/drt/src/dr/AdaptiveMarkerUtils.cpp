// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 1 — stub implementations. All return identity
// values appropriate for the disabled-model path. Real implementations
// land in Patch 2 (observation + CSV logging).

#include "dr/AdaptiveMarkerUtils.h"

#include "db/obj/frMarker.h"

namespace drt {

AdaptiveRuleClass classifyConstraint(const frConstraint* /*constraint*/)
{
  // Patch 1: pessimistic default. Patch 2 switches on constraint typeId
  // and maps to the appropriate rule class.
  return AdaptiveRuleClass::Other;
}

std::vector<frNet*> extractMarkerNets(const frMarker& /*marker*/)
{
  // Patch 1: empty extraction. Patch 2 walks marker srcs + aggressors and
  // pulls out frcNet owners. Later patches add inst-term / pin-fig /
  // block-object owners.
  return {};
}

int getRuleWeight(AdaptiveRuleClass rule)
{
  // Weights match the weighted_marker_score formula in the design doc.
  // Used by both the model's heat accumulation and the worker's
  // AdaptiveWorkerStats output.
  switch (rule) {
    case AdaptiveRuleClass::Short:
      return 100;
    case AdaptiveRuleClass::CutShort:
      return 80;
    case AdaptiveRuleClass::MetalSpacing:
      return 40;
    case AdaptiveRuleClass::CutSpacing:
      return 35;
    case AdaptiveRuleClass::Eol:
      return 20;
    case AdaptiveRuleClass::MinArea:
      return 10;
    case AdaptiveRuleClass::NsMetal:
      return 10;
    case AdaptiveRuleClass::Other:
      return 10;
  }
  return 10;
}

odb::Rect getRuleAwareInflatedBox(const frMarker& marker,
                                  AdaptiveRuleClass /*rule*/,
                                  int /*default_bloat_dbu*/)
{
  // Patch 1: no inflation. Patch 2 returns rule-aware bloats so the heat
  // accumulator picks up tiles adjacent to the marker, not just the
  // marker's own footprint.
  return marker.getBBox();
}

}  // namespace drt
