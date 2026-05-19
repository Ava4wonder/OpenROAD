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

    // Metal-spacing family. Also folds in enclosure/forbidden/wrong-dir
    // /keep-out/influence rules whose marker arises from the same kind
    // of mistake (wire-vs-wire / wire-vs-cut clearance).
    case frConstraintTypeEnum::frcSpacingConstraint:
    case frConstraintTypeEnum::frcSpacingTablePrlConstraint:
    case frConstraintTypeEnum::frcSpacingTableTwConstraint:
    case frConstraintTypeEnum::frcSpacingSamenetConstraint:
    case frConstraintTypeEnum::frcSpacingRangeConstraint:
    case frConstraintTypeEnum::frcLef58MaxSpacingConstraint:
    case frConstraintTypeEnum::frcSpacingTableInfluenceConstraint:
    case frConstraintTypeEnum::frcSpacingTableOrth:
    case frConstraintTypeEnum::frcLef58WidthTableOrth:
    case frConstraintTypeEnum::frcLef58SpacingWrongDirConstraint:
    case frConstraintTypeEnum::frcLef58KeepOutZoneConstraint:
    case frConstraintTypeEnum::frcLef58TwoWiresForbiddenSpcConstraint:
    case frConstraintTypeEnum::frcLef58ForbiddenSpcConstraint:
    case frConstraintTypeEnum::frcLef58EnclosureConstraint:
    case frConstraintTypeEnum::frcMetalWidthViaConstraint:
    case frConstraintTypeEnum::frcLef58RightWayOnGridOnlyConstraint:
    case frConstraintTypeEnum::frcLef58RectOnlyConstraint:
      return AdaptiveRuleClass::MetalSpacing;

    case frConstraintTypeEnum::frcCutSpacingConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingParallelWithinConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingAdjacentCutsConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingLayerConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingTableConstraint:
    case frConstraintTypeEnum::frcLef58CutSpacingTablePrlConstraint:
      return AdaptiveRuleClass::CutSpacing;

    // EOL family. The LEF58 EOL "within-*" sub-variants are folded in
    // since they're triggered by the same geometric condition (line-end
    // clearance) as the base EOL.
    case frConstraintTypeEnum::frcSpacingEndOfLineConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinEncloseCutConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinParallelEdgeConstraint:
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinMaxMinLengthConstraint:
    case frConstraintTypeEnum::frcLef58EolExtensionConstraint:
    case frConstraintTypeEnum::frcLef58EolKeepOutConstraint:
      return AdaptiveRuleClass::Eol;

    case frConstraintTypeEnum::frcAreaConstraint:
    case frConstraintTypeEnum::frcMinEnclosedAreaConstraint:
    case frConstraintTypeEnum::frcLef58AreaConstraint:
      return AdaptiveRuleClass::MinArea;

    case frConstraintTypeEnum::frcNonSufficientMetalConstraint:
      return AdaptiveRuleClass::NsMetal;

    // Patch 2.1 — min-step / minimum-cut family. Distinct from spacing
    // and area: these markers fire on small geometric features (notches,
    // step risers, isolated cuts). Promoted to its own bucket because
    // measurement on test9 8t showed this group dominated the prior
    // "Other" bucket.
    case frConstraintTypeEnum::frcMinStepConstraint:
    case frConstraintTypeEnum::frcLef58MinStepConstraint:
    case frConstraintTypeEnum::frcMinimumcutConstraint:
    case frConstraintTypeEnum::frcLef58MinimumCutConstraint:
      return AdaptiveRuleClass::MinStep;

    default:
      // Truly residual cases: frcMinWidthConstraint (rare on routed
      // designs), frcOffGridConstraint, frcRecheckConstraint, the
      // "not-supported" LEF58 corner-spacing variants, etc.
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
    case AdaptiveRuleClass::MinStep:
      // Min-step / minimum-cut violations are local feature issues —
      // small in DRV severity (similar to MinArea) but cheap to fix
      // by re-shaping. Same weight as MinArea/NsMetal in the
      // weighted_score formula.
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
