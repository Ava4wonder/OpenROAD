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
  // Tier-aware coverage accounting. supported_exact + supported_conservative
  // = total rules the oracle will evaluate. supported_exact alone is the
  // bypass-eligible count. The (explicit, unknown) layer split applies
  // to BOTH exact and conservative tiers per amendment 4; aggregated
  // here for simplicity (per-tier explicit/unknown can be reconstructed
  // from per-rule data when the audit needs it).
  //   total_input = supported_exact + supported_conservative
  //                 + fallback + unsupported
  struct Coverage
  {
    std::size_t total_input = 0;
    std::size_t supported_exact = 0;
    std::size_t supported_exact_explicit = 0;
    std::size_t supported_exact_unknown = 0;
    std::size_t supported_conservative = 0;
    std::size_t supported_conservative_explicit = 0;
    std::size_t supported_conservative_unknown = 0;
    std::size_t fallback = 0;
    std::size_t unsupported = 0;

    std::size_t supported_total() const
    {
      return supported_exact + supported_conservative;
    }
    double supported_exact_fraction() const
    {
      return total_input == 0 ? 0.0
                              : static_cast<double>(supported_exact)
                                    / static_cast<double>(total_input);
    }
    double supported_total_fraction() const
    {
      return total_input == 0 ? 0.0
                              : static_cast<double>(supported_total())
                                    / static_cast<double>(total_input);
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
