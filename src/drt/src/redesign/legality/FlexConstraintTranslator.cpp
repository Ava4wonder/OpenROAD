// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d / P2.2.e.2.b.1 — FlexConstraint -> NormalizedRule translation.
// The only file in the legality/ tree that includes upstream constraint
// headers; this is the abstraction boundary the user explicitly
// required.
//
// See GcWorkerClipBuilder.cpp top-of-file for the bridge-file
// conventions that apply here as well (include placement, `::drt::`
// anchoring, single-bridge-per-upstream-subsystem rule).

#include "FlexConstraintTranslator.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "db/tech/frConstraint.h"
#include "db/tech/frLayer.h"
#include "frBaseTypes.h"

namespace drt::redesign::legality {

namespace {

std::atomic<std::uint64_t> g_layer_conflicts{0};

// Outcome of deriving family + params + halo from one constraint's
// SUBCLASS only. Layer attribution is filled in by the caller per the
// TranslateOne contract.
struct SemanticResult
{
  bool recognized = false;  // false => caller should AddUnsupported
  NormalizedRule rule;
};

SemanticResult DeriveSemantics(const ::drt::frConstraint* c)
{
  SemanticResult out;
  if (c == nullptr) {
    return out;
  }
  using Type = ::drt::frConstraintTypeEnum;

  switch (c->typeId()) {
    case Type::frcShortConstraint: {
      out.rule.family = RuleFamily::MetalShort;
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = MetalShortConfig{};
      out.rule.halo = 0;
      out.rule.tag = "frShortConstraint";
      out.recognized = true;
      return out;
    }
    case Type::frcSpacingConstraint: {
      const auto* sc = static_cast<const ::drt::frSpacingConstraint*>(c);
      const auto min_sp = static_cast<std::int32_t>(sc->getMinSpacing());
      out.rule.family = RuleFamily::PrlSpacing;
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = PrlSpacingConfig{min_sp, 0};
      out.rule.halo = min_sp;
      out.rule.tag = "frSpacingConstraint";
      out.recognized = true;
      return out;
    }
    case Type::frcSpacingEndOfLineConstraint: {
      const auto* ec
          = static_cast<const ::drt::frSpacingEndOfLineConstraint*>(c);
      out.rule.family = RuleFamily::EolSpacing;
      if (ec->hasParallelEdge() || ec->hasTwoEdges()) {
        out.rule.coverage = RuleCoverage::Fallback;
        out.rule.tag = "frSpacingEndOfLineConstraint(parallel/twoEdges)";
        out.recognized = true;
        return out;
      }
      const auto eol_width = static_cast<std::int32_t>(ec->getEolWidth());
      const auto eol_within
          = static_cast<std::int32_t>(ec->getEolWithin());
      const auto eol_spacing
          = static_cast<std::int32_t>(ec->getMinSpacing());
      EolSpacingConfig cfg{eol_width, eol_spacing, eol_within};
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = cfg;
      out.rule.halo = std::max(cfg.eol_spacing, cfg.eol_within);
      out.rule.tag = "frSpacingEndOfLineConstraint";
      out.recognized = true;
      return out;
    }
    case Type::frcCutSpacingConstraint: {
      const auto* cc = static_cast<const ::drt::frCutSpacingConstraint*>(c);
      out.rule.family = RuleFamily::CutSpacing;
      const bool extended
          = cc->isAdjacentCuts() || cc->hasSecondLayer()
            || cc->isParallelOverlap() || cc->isArea() || cc->hasStack()
            || cc->hasSameNet() || cc->hasExceptSamePGNet()
            || cc->hasCenterToCenter() || cc->getCutSpacing() < 0;
      if (extended) {
        out.rule.coverage = RuleCoverage::Fallback;
        out.rule.tag = "frCutSpacingConstraint(extended)";
        out.recognized = true;
        return out;
      }
      CutSpacingConfig cfg{static_cast<std::int32_t>(cc->getCutSpacing())};
      out.rule.coverage = RuleCoverage::Supported;
      out.rule.params = cfg;
      out.rule.halo = cfg.min_spacing;
      out.rule.tag = "frCutSpacingConstraint";
      out.recognized = true;
      return out;
    }

    // Recognized as one of our families but explicitly out of scope.
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
      // Unrecognized; not in any of our four families.
      return out;  // recognized=false
  }

fallback:
  out.rule.coverage = RuleCoverage::Fallback;
  out.recognized = true;
  return out;
}

void NoteLayerConflict(const ::drt::frLayer* discovered,
                       const ::drt::frLayer* obj)
{
  const auto count = g_layer_conflicts.fetch_add(1) + 1;
  // Warn on the first few; silence afterwards to avoid log spam.
  if (count <= 5) {
    std::fprintf(
        stderr,
        "[drt::redesign] WARNING traversal-context layer conflict #%llu: "
        "discovered layer #%d but constraint->getLayer() reports #%d. "
        "Discovered layer is preferred per attribution contract.\n",
        static_cast<unsigned long long>(count),
        discovered != nullptr ? discovered->getLayerNum() : -1,
        obj != nullptr ? obj->getLayerNum() : -1);
  }
}

}  // namespace

std::uint64_t FlexConstraintTranslator::LayerConflictsSeen()
{
  return g_layer_conflicts.load();
}

std::optional<NormalizedRule> FlexConstraintTranslator::TranslateOne(
    const ::drt::frConstraint* c,
    const ::drt::frLayer* discovered_layer)
{
  auto sem = DeriveSemantics(c);
  if (!sem.recognized) {
    return std::nullopt;
  }
  NormalizedRule& rule = sem.rule;

  // Layer attribution: discovered_layer wins when present.
  ::drt::frLayer* obj_layer = c->getLayer();
  if (discovered_layer != nullptr) {
    if (obj_layer != nullptr && obj_layer != discovered_layer) {
      NoteLayerConflict(discovered_layer, obj_layer);
    }
    rule.layer_filter
        = static_cast<std::int16_t>(discovered_layer->getLayerNum());
    rule.layer_knownness = LayerKnownness::Explicit;
  } else if (obj_layer != nullptr) {
    rule.layer_filter
        = static_cast<std::int16_t>(obj_layer->getLayerNum());
    rule.layer_knownness = LayerKnownness::Explicit;
  } else {
    rule.layer_filter = std::nullopt;
    rule.layer_knownness = LayerKnownness::Unknown;
  }
  return rule;
}

RuleDeck FlexConstraintTranslator::Translate(
    const ::drt::frConstraint* const* upstream,
    std::size_t upstream_count)
{
  RuleDeck deck;
  for (std::size_t i = 0; i < upstream_count; ++i) {
    auto opt = TranslateOne(upstream[i], /*discovered_layer=*/nullptr);
    if (opt.has_value()) {
      deck.Add(*opt);
    } else {
      deck.AddUnsupported();
    }
  }
  return deck;
}

}  // namespace drt::redesign::legality
