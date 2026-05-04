// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight — RuleDeck implementation.

#include "RuleDeck.h"

#include <stdexcept>
#include <variant>

namespace drt::redesign::legality {

RuleDeck::RuleDeck() = default;

void RuleDeck::Add(const NormalizedRule& rule)
{
  rules_.push_back(rule);
  ++coverage_.total_input;
  switch (rule.coverage) {
    case RuleCoverage::Supported:
      ++coverage_.supported;
      if (rule.layer_knownness == LayerKnownness::Explicit) {
        ++coverage_.supported_explicit;
      } else {
        ++coverage_.supported_unknown;
      }
      break;
    case RuleCoverage::Fallback:
      ++coverage_.fallback;
      break;
    case RuleCoverage::Unsupported:
      ++coverage_.unsupported;
      break;
  }
}

void RuleDeck::AddUnsupported()
{
  ++coverage_.total_input;
  ++coverage_.unsupported;
}

namespace {

RuleType FamilyToType(RuleFamily f)
{
  switch (f) {
    case RuleFamily::MetalShort:
      return RuleType::MetalShort;
    case RuleFamily::PrlSpacing:
      return RuleType::PrlSpacing;
    case RuleFamily::EolSpacing:
      return RuleType::EolSpacing;
    case RuleFamily::CutSpacing:
      return RuleType::CutSpacing;
  }
  throw std::logic_error("FamilyToType: unknown RuleFamily");
}

RulePredicate FamilyToPredicate(RuleFamily f)
{
  switch (f) {
    case RuleFamily::MetalShort:
      return MetalShortReference;
    case RuleFamily::PrlSpacing:
      return PrlSpacingReference;
    case RuleFamily::EolSpacing:
      return EolSpacingReference;
    case RuleFamily::CutSpacing:
      return CutSpacingReference;
  }
  throw std::logic_error("FamilyToPredicate: unknown RuleFamily");
}

void* ParamPointer(const RuleParams& params)
{
  // The variant alternatives are config structs; we hand back a void*
  // alias to the live storage inside the variant. Lifetime tied to the
  // RuleDeck (which owns the NormalizedRule).
  return std::visit(
      [](auto& cfg) -> void* { return const_cast<void*>(static_cast<const void*>(&cfg)); },
      const_cast<RuleParams&>(params));
}

}  // namespace

std::vector<RuleEntry> RuleDeck::ToRuleEntries()
{
  std::vector<RuleEntry> out;
  out.reserve(coverage_.supported);
  for (auto& nr : rules_) {
    if (nr.coverage != RuleCoverage::Supported) {
      continue;
    }
    out.push_back(RuleEntry{FamilyToType(nr.family),
                            nr.halo,
                            FamilyToPredicate(nr.family),
                            ParamPointer(nr.params)});
  }
  return out;
}

}  // namespace drt::redesign::legality
