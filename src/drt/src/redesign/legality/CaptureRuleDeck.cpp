// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.b — CaptureRuleDeck implementation.
//
// See GcWorkerClipBuilder.cpp top-of-file for bridge conventions
// (include placement, ::drt:: anchoring, single-bridge-per-upstream
// rule).

#include "CaptureRuleDeck.h"

#include <vector>

#include "FlexConstraintTranslator.h"
#include "db/tech/frConstraint.h"
#include "db/tech/frLayer.h"
#include "db/tech/frTechObject.h"

namespace drt::redesign::legality {

namespace {

void AccountConstraint(const ::drt::frConstraint* c, CaptureCounters* cnt)
{
  if (c == nullptr || cnt == nullptr) {
    return;
  }
  ++cnt->constraints_seen;
  ++cnt->per_type_counts[static_cast<std::uint32_t>(c->typeId())];
}

}  // namespace

RuleDeck CaptureRuleDeck(const ::drt::frTechObject* tech,
                         CaptureCounters* counters)
{
  RuleDeck deck;
  if (tech == nullptr) {
    return deck;
  }

  std::vector<const ::drt::frConstraint*> all;
  for (const auto& layer : tech->getLayers()) {
    if (counters != nullptr) {
      ++counters->layers_walked;
    }

    // Single-pointer accessors. nullptr-safe.
    if (auto* c = layer->getShortConstraint()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    if (auto* c = layer->getMinSpacing()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    if (auto* c = layer->getSpacingSamenet()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    if (auto* c = layer->getSpacingTableInfluence()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }

    // Vector accessors.
    for (auto* c : layer->getEolSpacing()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    for (auto* c : layer->getCutSpacing(/*samenet=*/false)) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    for (auto* c : layer->getCutSpacing(/*samenet=*/true)) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
    if (layer->hasLef58SpacingEndOfLineConstraints()) {
      for (const auto& cu : layer->getLef58SpacingEndOfLineConstraints()) {
        AccountConstraint(cu.get(), counters);
        all.push_back(cu.get());
      }
    }
    for (const auto& cu :
         layer->getLef58CutSpacingConstraints(/*samenet=*/false)) {
      AccountConstraint(cu.get(), counters);
      all.push_back(cu.get());
    }
    for (const auto& cu :
         layer->getLef58CutSpacingConstraints(/*samenet=*/true)) {
      AccountConstraint(cu.get(), counters);
      all.push_back(cu.get());
    }
    for (auto* c : layer->getSpacingRangeConstraints()) {
      AccountConstraint(c, counters);
      all.push_back(c);
    }
  }

  // Hand the flat vector to the translator. Provenance counters above
  // describe traversal; deck.GetCoverage() will describe translation
  // outcomes; both get serialized so audit can distinguish them.
  FlexConstraintTranslator t;
  return t.Translate(all.data(), all.size());
}

}  // namespace drt::redesign::legality
