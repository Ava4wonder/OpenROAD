// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.b — CaptureRuleDeck implementation.
//
// See GcWorkerClipBuilder.cpp top-of-file for bridge conventions
// (include placement, ::drt:: anchoring, single-bridge-per-upstream
// rule).

#include "CaptureRuleDeck.h"

#include <utility>
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
  FlexConstraintTranslator t;

  // Per-layer translation so the discovered layer pointer flows into
  // the translator's attribution contract. The discovered layer is the
  // authoritative source; the object-stored layer (when populated) is
  // a fallback. See FlexConstraintTranslator::TranslateOne for the
  // contract details.
  auto handle = [&](const ::drt::frConstraint* c,
                    const ::drt::frLayer* discovered_layer) {
    AccountConstraint(c, counters);
    auto opt = t.TranslateOne(c, discovered_layer);
    if (opt.has_value()) {
      deck.Add(*opt);
    } else {
      deck.AddUnsupported();
    }
  };

  for (const auto& layer_uptr : tech->getLayers()) {
    const ::drt::frLayer* lp = layer_uptr.get();
    if (counters != nullptr) {
      ++counters->layers_walked;
    }

    // Single-pointer accessors.
    if (auto* c = layer_uptr->getShortConstraint()) {
      handle(c, lp);
    }
    if (auto* c = layer_uptr->getMinSpacing()) {
      handle(c, lp);
    }
    if (auto* c = layer_uptr->getSpacingSamenet()) {
      handle(c, lp);
    }
    if (auto* c = layer_uptr->getSpacingTableInfluence()) {
      handle(c, lp);
    }

    // Vector accessors.
    for (auto* c : layer_uptr->getEolSpacing()) {
      handle(c, lp);
    }
    for (auto* c : layer_uptr->getCutSpacing(/*samenet=*/false)) {
      handle(c, lp);
    }
    for (auto* c : layer_uptr->getCutSpacing(/*samenet=*/true)) {
      handle(c, lp);
    }
    if (layer_uptr->hasLef58SpacingEndOfLineConstraints()) {
      for (auto* c : layer_uptr->getLef58SpacingEndOfLineConstraints()) {
        handle(c, lp);
      }
    }
    for (auto* c :
         layer_uptr->getLef58CutSpacingConstraints(/*samenet=*/false)) {
      handle(c, lp);
    }
    for (auto* c :
         layer_uptr->getLef58CutSpacingConstraints(/*samenet=*/true)) {
      handle(c, lp);
    }
    for (auto* c : layer_uptr->getSpacingRangeConstraints()) {
      handle(c, lp);
    }
  }

  return deck;
}

}  // namespace drt::redesign::legality
