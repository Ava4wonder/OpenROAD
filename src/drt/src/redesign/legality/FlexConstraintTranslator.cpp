// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight stub. Real translation logic lands in P2.2.d proper.

#include "FlexConstraintTranslator.h"

namespace drt::redesign::legality {

RuleDeck FlexConstraintTranslator::Translate(
    const drt::frConstraint* const* /*upstream*/,
    std::size_t upstream_count)
{
  // Stub: every input is accounted as Unsupported so the coverage counters
  // reflect the upstream input population. The oracle sees zero rules
  // until P2.2.d wires real translation.
  RuleDeck deck;
  for (std::size_t i = 0; i < upstream_count; ++i) {
    deck.AddUnsupported();
  }
  return deck;
}

}  // namespace drt::redesign::legality
