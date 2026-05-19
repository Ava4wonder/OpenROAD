// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 2 — real marker interpretation logic.

#include "dr/AdaptiveMarkerUtils.h"

#include "db/obj/frMarker.h"
#include "db/obj/frNet.h"
#include "db/tech/frConstraint.h"
#include "frBaseTypes.h"

namespace drt {

AdaptiveRuleClass classifyConstraint(const frConstraint* constraint)
{
  if (constraint == nullptr) {
    return AdaptiveRuleClass::Other;
  }
  switch (constraint->typeId()) {
    case frConstraintTypeEnum::frcShortConstraint:
      return AdaptiveRuleClass::Short;

    case frConstraintTypeEnum::frcSpacingConstraint:
    case frConstraintTypeEnum::frcSpacingTablePrlConstraint:
    case frConstraintTypeEnum::frcSpacingTableTwConstraint:
    case frConstraintTypeEnum::frcSpacingSamenetConstraint:
    case frConstraintTypeEnum::frcSpacingRangeConstraint:
    case frConstraintTypeEnum::frcLef58MaxSpacingConstraint:
      return AdaptiveRuleClass::MetalSpacing;

    case frConstraintTypeEnum::frcCutSpacingConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingParallelWithinConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingAdjacentCutsConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingLayerConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingTableConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingTablePrlConstraint:
      return AdaptiveRuleClass::CutSpacing;

    case frConstraintTypeEnum::frcSpacingEndOfLineConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinConstraint:
    case frConstraintTypeEnum::frcLef58EolExtensionConstraint:
    case frConstraintTypeEnum::frcLef58EolKeepOutConstraint:
      return AdaptiveRuleClass::Eol;

    case frConstraintTypeEnum::frcAreaConstraint:
    case frConstraintTypeEnum::frcMinEnclosedAreaConstraint:
    case frConstraintTypeEnum::frcLef58AreaConstraint:
      return AdaptiveRuleClass::MinArea;

    case frConstraintTypeEnum::frcNonSufficientMetalConstraint:
      return AdaptiveRuleClass::NsMetal;

    default:
      return AdaptiveRuleClass::Other;
  }
}

std::vector<frNet*> extractMarkerNets(const frMarker& marker)
{
  // Patch 2: conservative extraction — walk marker srcs and grab only
  // direct frcNet owners. Later patches can add inst-term / pin-fig /
  // block-object owner resolution (each requires a different upward
  // walk to the owning frNet).
  std::vector<frNet*> nets;
  for (frBlockObject* obj : marker.getSrcs()) {
    if (obj == nullptr) {
      continue;
    }
    if (obj->typeId() == frcNet) {
      nets.push_back(static_cast<frNet*>(obj));
    }
  }
  return nets;
}

int getRuleWeight(AdaptiveRuleClass rule)
{
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
                                  AdaptiveRuleClass rule,
                                  int default_bloat_dbu)
{
  // Inflate the marker's bbox by a rule-aware delta so the heat
  // accumulator catches tiles adjacent to the marker, not just the
  // marker's own footprint. Patches 5/6 may refine these — for
  // Patch 2 we use the default for Short/EOL/MinArea and a 50%
  // increase for spacing rules (which propagate further).
  odb::Rect bb = marker.getBBox();
  int bloat = default_bloat_dbu;
  switch (rule) {
    case AdaptiveRuleClass::MetalSpacing:
    case AdaptiveRuleClass::CutSpacing:
      bloat = default_bloat_dbu * 3 / 2;
      break;
    default:
      break;
  }
  if (bloat > 0) {
    bb.bloat(bloat, bb);
  }
  return bb;
}

}  // namespace drt
