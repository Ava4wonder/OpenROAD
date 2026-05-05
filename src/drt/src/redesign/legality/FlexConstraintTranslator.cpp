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
#include "db/tech/frLookupTbl.h"
#include "frBaseTypes.h"

namespace drt::redesign::legality {

namespace {

std::atomic<std::uint64_t> g_layer_conflicts{0};

// P2.2.e.2.c.2 (Step C1) classifier + Step C2 widening.
//
// Classes:
//   ConstantSpacing        — all cells equal. C2: Supported (exact).
//                            params = (constant_value, prl_threshold=0).
//   SingleRow              — exactly one width row. C2: Supported with
//                            conservative collapse — params = (findMax,
//                            prl_threshold=0). Over-flags PRL-varying
//                            single-row tables but no false negatives.
//                            Per the user's tiering this is treated as
//                            exact-ish since one width-bin maps cleanly
//                            to PrlSpacingConfig.
//   MonotonicMultiWidth    — multi-row, multi-col, values non-decreasing
//                            in BOTH axes. Stays Fallback. C3 candidate
//                            (deferred): would land as a separate
//                            supported_conservative tier if added.
//   Exotic                 — anything else. Stays Fallback.
//   Null                   — null pointer (defensive).
enum class PrlTableShape : std::uint8_t {
  ConstantSpacing,
  SingleRow,
  MonotonicMultiWidth,
  Exotic,
  Null,
};

const char* PrlTableShapeName(PrlTableShape s)
{
  switch (s) {
    case PrlTableShape::ConstantSpacing:
      return "constant_spacing";
    case PrlTableShape::SingleRow:
      return "single_row";
    case PrlTableShape::MonotonicMultiWidth:
      return "monotonic_multi_width";
    case PrlTableShape::Exotic:
      return "exotic";
    case PrlTableShape::Null:
      return "null";
  }
  return "?";
}

bool PrlShapeIsExactish(PrlTableShape s)
{
  return s == PrlTableShape::ConstantSpacing
         || s == PrlTableShape::SingleRow;
}

PrlTableShape ClassifyPrlTable(const ::drt::frSpacingTablePrlConstraint* c)
{
  if (c == nullptr) {
    return PrlTableShape::Null;
  }
  const auto& tbl = c->getLookupTbl();
  if (tbl.findMin() == tbl.findMax()) {
    return PrlTableShape::ConstantSpacing;
  }
  // Upstream API quirk: getRows/getCols/getValues are not const member
  // functions despite returning by value. const_cast to call them is
  // safe; we do not mutate state. Documented in upstream as a known
  // const-correctness gap.
  using TblT = std::decay_t<decltype(tbl)>;
  TblT& mut_tbl = const_cast<TblT&>(tbl);
  const auto rows = mut_tbl.getRows();
  const auto vals = mut_tbl.getValues();
  if (rows.size() <= 1) {
    return PrlTableShape::SingleRow;
  }
  bool monotonic = true;
  for (std::size_t r = 0; r + 1 < vals.size() && monotonic; ++r) {
    for (std::size_t cc = 0;
         cc < vals[r].size() && cc < vals[r + 1].size();
         ++cc) {
      if (vals[r + 1][cc] < vals[r][cc]) {
        monotonic = false;
        break;
      }
    }
  }
  for (std::size_t r = 0; r < vals.size() && monotonic; ++r) {
    for (std::size_t cc = 0; cc + 1 < vals[r].size(); ++cc) {
      if (vals[r][cc + 1] < vals[r][cc]) {
        monotonic = false;
        break;
      }
    }
  }
  return monotonic ? PrlTableShape::MonotonicMultiWidth
                   : PrlTableShape::Exotic;
}

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
    case Type::frcSpacingTablePrlConstraint: {
      const auto* sc
          = static_cast<const ::drt::frSpacingTablePrlConstraint*>(c);
      out.rule.family = RuleFamily::PrlSpacing;
      const PrlTableShape shape = ClassifyPrlTable(sc);
      out.rule.tag = std::string("frSpacingTablePrlConstraint(")
                     + PrlTableShapeName(shape) + ")";
      // C2: widen exact-ish shapes (ConstantSpacing, SingleRow) to
      // Supported. Conservative-collapse interpretation:
      //   min_spacing = findMax (largest required spacing across the
      //                          whole table — never under-flags)
      //   prl_threshold = 0     (apply at any PRL >= 0)
      // For ConstantSpacing this is exact (findMin == findMax).
      // For SingleRow with PRL-varying values, this over-flags small-
      // PRL pairs but stays inside the no-false-negative contract.
      // MonotonicMultiWidth + Exotic stay Fallback (C3 candidate).
      if (PrlShapeIsExactish(shape)) {
        const auto& tbl = sc->getLookupTbl();
        const auto min_sp = static_cast<std::int32_t>(tbl.findMax());
        out.rule.coverage = RuleCoverage::Supported;
        out.rule.params = PrlSpacingConfig{min_sp, 0};
        out.rule.halo = min_sp;
        out.recognized = true;
        return out;
      }
      goto fallback;
    }
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

  // Layer attribution policy: discovered_layer WINS when present.
  // Object-layer is consulted only as fallback when discovered is null.
  // Conflicts are resolved in favor of discovered and counted via
  // LayerConflictsSeen() so audit can flag the disagreement.
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
