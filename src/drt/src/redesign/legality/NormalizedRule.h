// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight — normalized rule schema. This is the abstraction
// boundary between upstream OpenROAD's rule storage (FlexConstraint) and
// the oracle's RuleEntry. The oracle NEVER sees FlexConstraint directly.
// Translation is one-way: FlexConstraint -> NormalizedRule -> RuleEntry.
//
// Why a separate type:
//   1) Keeps the oracle backend-portable: a future GPU oracle ingests
//      NormalizedRule (or a SoA cousin), not upstream pointers.
//   2) Lets us mark rules Unsupported / Fallback honestly without
//      synthesizing predicates that don't actually capture the rule.
//   3) Coverage counters become an explicit honesty metric in reporting.

#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "Predicates.h"

namespace drt::redesign::legality {

enum class RuleFamily : std::uint8_t {
  MetalShort = 0,
  PrlSpacing = 1,
  EolSpacing = 2,
  CutSpacing = 3,
};

// Per-rule coverage. Honest reporting requires accounting for all three
// outcomes; "unsupported" is not the same as "fallback."
enum class RuleCoverage : std::uint8_t {
  // Translated to a NormalizedRule with valid params; oracle predicate
  // evaluates it.
  Supported,
  // Translation recognized the rule family/shape but the parameters fall
  // outside the in-scope subset (see FlexConstraintTranslator.h). The
  // oracle does NOT evaluate it; clips containing geometry potentially
  // governed by this rule must fall through to the upstream exact
  // checker. Coverage counter incremented.
  Fallback,
  // Translation did not recognize the upstream constraint. Same
  // fall-through behavior as Fallback, but accounted separately so we
  // can tell "we know what we don't know" from "we don't know what we
  // don't know."
  Unsupported,
};

// Per-family parameter payload. Only populated when coverage == Supported.
using RuleParams = std::variant<MetalShortConfig,
                                PrlSpacingConfig,
                                EolSpacingConfig,
                                CutSpacingConfig>;

struct NormalizedRule
{
  RuleFamily family;
  RuleCoverage coverage = RuleCoverage::Unsupported;
  RuleParams params;          // valid iff coverage == Supported
  std::int16_t layer = -1;    // -1 means "applies to all layers"
  std::string tag;            // human-readable for trace output

  // x-axis halo upper bound for the oracle's per-rule pre-filter.
  // Computed per-family from the params; only meaningful when
  // coverage == Supported. See RuleEntry::halo CONTRACT in CpuDrcOracle.h.
  //
  // NOTE on halo's role: this is the first safe pruning key — an x-axis
  // upper bound on where the predicate can fire. It is NOT a universal
  // pruning key; later rule families may need additional pruning
  // dimensions (layer equality, parallel-run-length, EOL-side geometry,
  // cut neighborhood, shape class). Halo stays humble in this API.
  std::int32_t halo = 0;
};

}  // namespace drt::redesign::legality
