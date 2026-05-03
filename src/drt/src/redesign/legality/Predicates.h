// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.b — Reference (naive O(N*M)) implementations of the four FastPass
// rule predicates: MetalShort, PrlSpacing, EolSpacing, CutSpacing.
//
// These reference functions serve TWO roles per the user's P2.2.b
// guidance:
//   1) Semantic ground truth for each rule type. P2.2.c sweep-line-aware
//      predicates must agree with these on the same inputs.
//   2) Fallback oracle for debugging mismatches. EvaluateReference()
//      below runs the references O(N*M) and is the truth witness when
//      the production sweep-line oracle disagrees.
//
// All four predicates follow conservative-checker semantics
// (drt_redesign_plan.md §4): true iff the (candidate, context) pair
// violates the rule under the predicate's idealized model. False
// positives versus the upstream FlexGCWorker exact rules are acceptable
// (P2.2.d will tighten); false negatives are bugs.

#pragma once

#include <cstddef>
#include <cstdint>

#include "CpuDrcOracle.h"

namespace drt::redesign::legality {

// Empty config; metal short has no parameters.
struct MetalShortConfig
{
};

struct PrlSpacingConfig
{
  std::int32_t min_spacing = 0;
  std::int32_t prl_threshold = 0;  // parallel-run-length below which rule
                                   // does not trigger
};

struct EolSpacingConfig
{
  std::int32_t eol_width_threshold = 0;  // edges shorter than this are EOL
  std::int32_t eol_spacing = 0;          // perpendicular clearance required
  std::int32_t eol_within = 0;           // y-/x-extension to check
};

struct CutSpacingConfig
{
  std::int32_t min_spacing = 0;  // L-infinity edge-to-edge spacing required
};

bool MetalShortReference(const Shape& cand, const Shape& ctx, void* opaque);
bool PrlSpacingReference(const Shape& cand, const Shape& ctx, void* opaque);
bool EolSpacingReference(const Shape& cand, const Shape& ctx, void* opaque);
bool CutSpacingReference(const Shape& cand, const Shape& ctx, void* opaque);

// Fallback oracle. Runs each rule's reference predicate O(cand_count *
// context_count) without sweep-line. Use this as the truth witness when
// the sweep-line CpuDrcOracle disagrees, and as the test oracle for
// P2.2.c equivalence checks.
//
// Verdicts default-init to legal=true; this overlays violations.
void EvaluateReference(const Shape* candidates,
                       std::size_t candidates_count,
                       const Shape* context,
                       std::size_t context_count,
                       const RuleEntry* rules,
                       std::size_t rule_count,
                       Verdict* verdicts);

}  // namespace drt::redesign::legality
