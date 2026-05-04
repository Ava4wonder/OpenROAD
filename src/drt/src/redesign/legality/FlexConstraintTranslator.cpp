// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d — FlexConstraint -> NormalizedRule translation. The only file
// in the legality/ tree that includes upstream constraint headers; this
// is the abstraction boundary the user explicitly required.

#include "FlexConstraintTranslator.h"

#include <algorithm>

#include "db/tech/frConstraint.h"
#include "db/tech/frLayer.h"
#include "frBaseTypes.h"

namespace drt::redesign::legality {

namespace {

// Outcome of translating ONE upstream constraint.
struct OneResult
{
  bool produced_rule = false;  // false => caller calls AddUnsupported()
  NormalizedRule rule;
};

// Pull layer info out of an upstream constraint via the public
// frConstraint::getLayer() accessor. Sets layer_filter + layer_knownness
// in-place. If the upstream binding is absent (constraint not yet
// attached to a layer, e.g. a freshly-constructed test fixture), we
// stay Unknown rather than fabricate a layer number.
void PopulateLayer(NormalizedRule& nr, const drt::frConstraint* c)
{
  drt::frLayer* layer = c->getLayer();
  if (layer == nullptr) {
    nr.layer_filter = std::nullopt;
    nr.layer_knownness = LayerKnownness::Unknown;
    return;
  }
  nr.layer_filter
      = static_cast<std::int16_t>(layer->getLayerNum());
  nr.layer_knownness = LayerKnownness::Explicit;
}

// Tag literals are static-storage; NormalizedRule.tag is std::string and
// owns its copy.
OneResult TranslateOne(const drt::frConstraint* c)
{
  OneResult out;
  if (c == nullptr) {
    out.rule.tag = "null";
    return out;  // counts as Unsupported
  }

  using Type = drt::frConstraintTypeEnum;

  switch (c->typeId()) {
    // --- MetalShort: same-layer different-net AABB overlap ----------
    case Type::frcShortConstraint: {
      out.rule.family = RuleFamily::MetalShort;
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = MetalShortConfig{};
      out.rule.halo = 0;
      PopulateLayer(out.rule, c);
      out.rule.tag = "frShortConstraint";
      out.produced_rule = true;
      return out;
    }

    // --- PrlSpacing: basic min-spacing rule ------------------------
    case Type::frcSpacingConstraint: {
      const auto* sc = static_cast<const drt::frSpacingConstraint*>(c);
      const auto min_sp = static_cast<std::int32_t>(sc->getMinSpacing());
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = PrlSpacingConfig{min_sp, 0};
      out.rule.halo = min_sp;
      PopulateLayer(out.rule, c);
      out.rule.tag = "frSpacingConstraint";
      out.produced_rule = true;
      return out;
    }

    // --- EolSpacing: simple eolWidth/eolWithin/spacing tuple --------
    case Type::frcSpacingEndOfLineConstraint: {
      const auto* ec
          = static_cast<const drt::frSpacingEndOfLineConstraint*>(c);
      out.rule.family = RuleFamily::EolSpacing;
      // Out-of-scope: parallel-edge condition or two-edges condition.
      // Both broaden the rule into territory the simple predicate does
      // not model; mark Fallback so the upstream exact checker handles
      // affected geometry.
      if (ec->hasParallelEdge() || ec->hasTwoEdges()) {
        out.rule.coverage = RuleCoverage::Fallback;
        out.rule.tag = "frSpacingEndOfLineConstraint(parallel/twoEdges)";
        out.produced_rule = true;
        return out;
      }
      const auto eol_width = static_cast<std::int32_t>(ec->getEolWidth());
      const auto eol_within = static_cast<std::int32_t>(ec->getEolWithin());
      const auto eol_spacing = static_cast<std::int32_t>(ec->getMinSpacing());
      EolSpacingConfig cfg{eol_width, eol_spacing, eol_within};
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = cfg;
      out.rule.halo = std::max(cfg.eol_spacing, cfg.eol_within);
      PopulateLayer(out.rule, c);
      out.rule.tag = "frSpacingEndOfLineConstraint";
      out.produced_rule = true;
      return out;
    }

    // --- CutSpacing: minimal edge-to-edge variant ------------------
    case Type::frcCutSpacingConstraint: {
      const auto* cc = static_cast<const drt::frCutSpacingConstraint*>(c);
      out.rule.family = RuleFamily::CutSpacing;
      const bool extended
          = cc->isAdjacentCuts() || cc->hasSecondLayer()
            || cc->isParallelOverlap() || cc->isArea() || cc->hasStack()
            || cc->hasSameNet() || cc->hasExceptSamePGNet()
            || cc->hasCenterToCenter() || cc->getCutSpacing() < 0;
      if (extended) {
        out.rule.coverage = RuleCoverage::Fallback;
        out.rule.tag = "frCutSpacingConstraint(extended)";
        out.produced_rule = true;
        return out;
      }
      CutSpacingConfig cfg{static_cast<std::int32_t>(cc->getCutSpacing())};
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = cfg;
      out.rule.halo = cfg.min_spacing;
      PopulateLayer(out.rule, c);
      out.rule.tag = "frCutSpacingConstraint";
      out.produced_rule = true;
      return out;
    }

    // --- Recognized as one of our families but explicitly out of scope.
    //     We add to the deck (with Fallback coverage) so the deck
    //     reflects "the design has rules of this family that we declined
    //     to evaluate" — distinct from "we didn't recognize the rule
    //     at all" (Unsupported, counter-only).
    case Type::frcSpacingSamenetConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frSpacingSamenetConstraint";
      goto fallback;
    case Type::frcSpacingTablePrlConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frSpacingTablePrlConstraint";
      goto fallback;
    case Type::frcSpacingTableTwConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frSpacingTableTwConstraint";
      goto fallback;
    case Type::frcSpacingTableConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frSpacingTableConstraint";
      goto fallback;
    case Type::frcLef58SpacingTableConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frLef58SpacingTableConstraint";
      goto fallback;
    case Type::frcSpacingRangeConstraint:
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.tag = "frSpacingRangeConstraint";
      goto fallback;
    case Type::frcLef58SpacingEndOfLineConstraint:
      out.rule.family = RuleFamily::EolSpacing;
      out.rule.tag = "frLef58SpacingEndOfLineConstraint";
      goto fallback;
    case Type::frcLef58SpacingEndOfLineWithinConstraint:
      out.rule.family = RuleFamily::EolSpacing;
      out.rule.tag = "frLef58SpacingEndOfLineWithinConstraint";
      goto fallback;
    case Type::frcLef58CutSpacingConstraint:
      out.rule.family = RuleFamily::CutSpacing;
      out.rule.tag = "frLef58CutSpacingConstraint";
      goto fallback;
    case Type::frcLef58CutSpacingTableConstraint:
      out.rule.family = RuleFamily::CutSpacing;
      out.rule.tag = "frLef58CutSpacingTableConstraint";
      goto fallback;

    default:
      // Unrecognized: not in any of our four families. Counter-only.
      out.rule.tag = "unrecognized";
      return out;
  }

fallback:
  out.rule.coverage = RuleCoverage::Fallback;
  out.produced_rule = true;
  return out;
}

}  // namespace

RuleDeck FlexConstraintTranslator::Translate(
    const drt::frConstraint* const* upstream,
    std::size_t upstream_count)
{
  RuleDeck deck;
  for (std::size_t i = 0; i < upstream_count; ++i) {
    auto r = TranslateOne(upstream[i]);
    if (r.produced_rule) {
      deck.Add(r.rule);
    } else {
      deck.AddUnsupported();
    }
  }
  return deck;
}

}  // namespace drt::redesign::legality
