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
#include <optional>
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

// Four-tier support classification. Per the P2.2.e.2.c.4 review
// (architecture discipline): translator only normalizes semantics; tier
// captures HOW the normalized rule should be used downstream. Audit
// counts tiers separately so reporting never blurs faithful
// representation with safe overapproximation.
enum class SupportTier : std::uint8_t {
  // Translated to a NormalizedRule whose params are SEMANTICALLY
  // FAITHFUL to the upstream constraint. Oracle can evaluate directly;
  // results are usable as-is for bypass decisions.
  Exact = 0,
  // Translated rule is a SAFE OVERAPPROXIMATION (no false negatives,
  // possibly over-flags). Oracle evaluates the rule; a "violation"
  // verdict still requires upstream exact validation before commit; a
  // "legal" verdict is trusted (since conservative rules can't say
  // "legal" when the exact rule would say "violation").
  Conservative = 1,
  // Translation recognized the rule family/shape but the parameters
  // fall outside any in-scope subset. Oracle does NOT evaluate; clips
  // containing geometry potentially governed by this rule must fall
  // through to the upstream exact checker.
  Fallback = 2,
  // Translation did not recognize the upstream constraint at all. Same
  // fall-through behavior as Fallback; accounted separately so audit
  // can distinguish "we know what we don't know" from "we don't know
  // what we don't know."
  Unsupported = 3,
};

// Per-family parameter payload. Only populated when coverage == Supported.
using RuleParams = std::variant<MetalShortConfig,
                                PrlSpacingConfig,
                                EolSpacingConfig,
                                CutSpacingConfig>;

// Per amendment 4 of the P2.2.e structural review. Disambiguates "no
// layer filter set" between "explicitly all layers" and "couldn't
// determine the layer at translation time" — both behave identically in
// the oracle (predicates do their own per-pair layer check via Shape),
// but the reporting layer needs to distinguish them. Avoids the prior
// `layer == -1` sentinel that conflated the two.
enum class LayerKnownness : std::uint8_t {
  // Layer info was extractable from upstream and is reflected in
  // layer_filter (set or explicitly empty for "all layers").
  Explicit,
  // Layer info couldn't be extracted at translation time. The rule still
  // evaluates correctly because the oracle's per-pair predicate filters
  // by Shape::layer; only per-layer reporting granularity is reduced.
  Unknown,
};

struct NormalizedRule
{
  RuleFamily family;
  SupportTier tier = SupportTier::Unsupported;
  RuleParams params;          // valid iff coverage == Supported

  // Optional layer constraint. Combined with layer_knownness:
  //   (set,        Explicit) — applies to that specific layer
  //   (std::nullopt, Explicit) — explicitly applies to all layers
  //   (std::nullopt, Unknown)  — translation-time layer unknown
  //   (set,        Unknown)    — invalid combination, do not construct
  std::optional<std::int16_t> layer_filter;
  LayerKnownness layer_knownness = LayerKnownness::Unknown;

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
