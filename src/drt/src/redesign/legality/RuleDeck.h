// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight — RuleDeck owns a translated set of NormalizedRules
// and exposes (a) honest coverage accounting, (b) materialization to the
// oracle's RuleEntry array.

#pragma once

#include <cstddef>
#include <vector>

#include "CpuDrcOracle.h"
#include "NormalizedRule.h"

namespace drt::redesign::legality {

class RuleDeck
{
 public:
  // Honest coverage accounting from translation. Reportable in P2.2.e.
  // Per amendment 4, the four-category split distinguishes Supported
  // rules with explicit-layer info from Supported rules with unknown
  // layer info. Both still evaluate identically in the oracle (predicates
  // self-filter by Shape::layer), but reporting can show how much
  // per-layer attribution we lose to the upstream getLayer() limitation.
  //   supported = supported_explicit + supported_unknown
  struct Coverage
  {
    std::size_t total_input = 0;        // upstream constraints inspected
    std::size_t supported = 0;          // oracle evaluates these
    std::size_t supported_explicit = 0; // ... with extractable layer info
    std::size_t supported_unknown = 0;  // ... with layer info missing
    std::size_t fallback = 0;           // oracle declines; exact-check fallback
    std::size_t unsupported = 0;        // unrecognized

    double supported_fraction() const
    {
      return total_input == 0 ? 0.0
                              : static_cast<double>(supported)
                                    / static_cast<double>(total_input);
    }
    double explicit_layer_fraction() const
    {
      return supported == 0 ? 0.0
                            : static_cast<double>(supported_explicit)
                                  / static_cast<double>(supported);
    }
  };

  RuleDeck();

  void Add(const NormalizedRule& rule);
  void AddUnsupported();  // accounting-only: increments total_input + unsupported

  std::size_t Size() const noexcept { return rules_.size(); }
  const NormalizedRule& At(std::size_t i) const { return rules_.at(i); }

  Coverage GetCoverage() const noexcept { return coverage_; }

  // Materialize the Supported-coverage subset into oracle-ingestible
  // RuleEntry objects. The returned vector references config storage
  // owned by this RuleDeck; the deck must outlive the oracle's next
  // Evaluate() call.
  std::vector<RuleEntry> ToRuleEntries();

 private:
  std::vector<NormalizedRule> rules_;
  Coverage coverage_;
};

}  // namespace drt::redesign::legality
