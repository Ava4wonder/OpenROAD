// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.a — CPU DRC pair-evaluator API.
//
// CpuDrcOracle decides legality for a batch of candidate rectangles against
// a set of fixed context rectangles. The inner algorithm is a sweep-line
// over x-coordinates that maintains an active region of nearby context
// shapes, then dispatches per-rule predicates only on the candidate ↔
// active-context pairs. Adapted from FastPass (ISPD 2023, Algorithm 1)
// per topics/.../Related_work.md and drt_redesign_execution_plan.md P2.2.
//
// **Conservative-checker semantics** (per drt_redesign_plan.md §4 and the
// P2 goal block of the execution plan):
//   - Verdict::legal == false  → caller MUST treat as a violation candidate
//     (push to upstream exact checker for disambiguation).
//   - Verdict::legal == true   → caller MAY treat as legal without further
//     check, ONLY for the rule types loaded in the rule deck. Rules not
//     loaded are not checked here.
// The oracle is allowed to over-flag (false positives) but must NEVER
// under-flag (false negatives) on the loaded rules.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace drt::redesign::legality {

struct Shape
{
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  std::int32_t x2 = 0;
  std::int32_t y2 = 0;
  std::int16_t layer = 0;
  std::uint64_t net_id = 0;
};

enum class RuleType : std::uint8_t {
  MetalShort = 0,
  PrlSpacing = 1,
  EolSpacing = 2,
  CutSpacing = 3,
};

// Rule-predicate plug-in. Returns true if the (candidate, context) pair
// violates the rule (with the conservative-checker semantics from the
// header comment). `opaque` is whatever rule-deck context the predicate
// needs (e.g. a spacing-table pointer); the oracle never inspects it.
using RulePredicate = bool (*)(const Shape& candidate,
                               const Shape& context,
                               void* opaque);

struct RuleEntry
{
  RuleType type;
  std::int32_t halo;       // x-axis active-region halo for this rule
  RulePredicate predicate;
  void* opaque;            // pointer-to-rule-deck context; owned by caller
};

struct Verdict
{
  bool legal = true;
  std::uint16_t triggered_rules = 0;  // bitmask; bit (1u << RuleType)
};

class CpuDrcOracle
{
 public:
  CpuDrcOracle();
  ~CpuDrcOracle();

  CpuDrcOracle(const CpuDrcOracle&) = delete;
  CpuDrcOracle& operator=(const CpuDrcOracle&) = delete;

  // Install the active rule deck. References to `rules` and to each
  // entry's `opaque` pointer must outlive the next Evaluate() call.
  // No copy is performed.
  void SetRules(const RuleEntry* rules, std::size_t count);

  // Evaluate all `candidates_count` candidates against `context_count`
  // fixed shapes. Both arrays MUST be pre-sorted by x1 ascending; the
  // oracle does not sort. Writes one Verdict per candidate.
  //
  // The active region used in sweep-line is the per-rule-deck max halo;
  // a candidate at x1 = X has context shapes with x2 in [X - max_halo,
  // X + (candidate.x2 - candidate.x1) + max_halo] in its active region.
  void Evaluate(const Shape* candidates,
                std::size_t candidates_count,
                const Shape* context,
                std::size_t context_count,
                Verdict* verdicts);

  // Total number of (candidate, active-context) predicate dispatches in
  // the most recent Evaluate() call. Used for benchmark normalization in
  // pair/sec, the unit drt_redesign_execution_plan.md P2.2 exit
  // criterion is phrased in. Reset to 0 by each Evaluate().
  std::size_t LastPairsEvaluated() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drt::redesign::legality
