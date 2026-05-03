// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.a — CpuDrcOracle implementation: rule-agnostic sweep-line over
// x-coordinates, per-rule predicate dispatch, conservative-checker
// semantics. Rule-specific predicates land in P2.2.b/c.

#include "CpuDrcOracle.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace drt::redesign::legality {

struct CpuDrcOracle::Impl
{
  const RuleEntry* rules = nullptr;
  std::size_t rule_count = 0;
  std::int32_t max_halo = 0;
  std::size_t last_pairs = 0;

  // Active set: indices into `context` whose x2 + max_halo >= sweep_x.
  // Maintained as an unsorted vector; entries removed lazily during
  // forward advance.
  std::vector<std::size_t> active;

  void RecomputeMaxHalo()
  {
    max_halo = 0;
    for (std::size_t i = 0; i < rule_count; ++i) {
      if (rules[i].halo > max_halo) {
        max_halo = rules[i].halo;
      }
    }
  }
};

CpuDrcOracle::CpuDrcOracle() : impl_(std::make_unique<Impl>())
{
}

CpuDrcOracle::~CpuDrcOracle() = default;

void CpuDrcOracle::SetRules(const RuleEntry* rules, std::size_t count)
{
  impl_->rules = rules;
  impl_->rule_count = count;
  impl_->RecomputeMaxHalo();
  impl_->active.clear();
}

void CpuDrcOracle::Evaluate(const Shape* candidates,
                            std::size_t candidates_count,
                            const Shape* context,
                            std::size_t context_count,
                            Verdict* verdicts)
{
  // Default: legal until proven otherwise.
  for (std::size_t i = 0; i < candidates_count; ++i) {
    verdicts[i] = Verdict{};
  }
  impl_->last_pairs = 0;
  if (candidates_count == 0) {
    return;
  }
  if (context_count == 0 || impl_->rule_count == 0) {
    return;
  }

  const std::int32_t halo = impl_->max_halo;

  // Advance pointers into `context`: `next_add` is the next context idx to
  // consider for activation. The active set is filtered lazily on each
  // candidate (entries whose x2 + halo < candidate.x1 are dropped).
  std::size_t next_add = 0;
  impl_->active.clear();

  for (std::size_t c = 0; c < candidates_count; ++c) {
    const Shape& cand = candidates[c];
    const std::int32_t window_lo = cand.x1 - halo;
    const std::int32_t window_hi = cand.x2 + halo;

    // 1) Add any new context shapes whose x1 <= window_hi.
    while (next_add < context_count && context[next_add].x1 <= window_hi) {
      impl_->active.push_back(next_add);
      ++next_add;
    }

    // 2) Drop active entries whose x2 + halo < cand.x1 (no longer reachable).
    auto new_end = std::remove_if(
        impl_->active.begin(),
        impl_->active.end(),
        [&](std::size_t idx) { return context[idx].x2 + halo < window_lo; });
    impl_->active.erase(new_end, impl_->active.end());

    // 3) For each active context shape, dispatch all rules.
    for (const std::size_t idx : impl_->active) {
      const Shape& ctx = context[idx];
      for (std::size_t r = 0; r < impl_->rule_count; ++r) {
        const RuleEntry& rule = impl_->rules[r];
        ++impl_->last_pairs;
        if (rule.predicate(cand, ctx, rule.opaque)) {
          verdicts[c].legal = false;
          verdicts[c].triggered_rules
              |= static_cast<std::uint16_t>(1u
                                            << static_cast<std::uint8_t>(
                                                rule.type));
        }
      }
    }
  }
}

std::size_t CpuDrcOracle::LastPairsEvaluated() const noexcept
{
  return impl_->last_pairs;
}

}  // namespace drt::redesign::legality
