// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.a — Sweep-line correctness test for CpuDrcOracle, plus a microbench
// against an O(N*M) brute-force reference. Built only when
// ENABLE_DRT_REDESIGN=ON.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "redesign/legality/CpuDrcOracle.h"

namespace lg = drt::redesign::legality;

namespace {

// Synthetic rule: "two shapes on the same layer with x-axis gap < threshold
// are illegal." Conservative — y-axis ignored, so it over-flags compared
// to a real spacing rule but is sufficient for sweep-line correctness.
struct GapThreshold
{
  std::int32_t threshold;
};

bool SameLayerXGapPredicate(const lg::Shape& cand,
                            const lg::Shape& ctx,
                            void* opaque)
{
  const auto* th = static_cast<const GapThreshold*>(opaque);
  if (cand.layer != ctx.layer) {
    return false;
  }
  if (cand.net_id != 0 && cand.net_id == ctx.net_id) {
    return false;  // same net, ignored
  }
  // Distance between the closest x-edges
  const std::int32_t gap = std::max(cand.x1 - ctx.x2, ctx.x1 - cand.x2);
  return gap < th->threshold;
}

// Brute-force reference: for each candidate, check all context shapes.
void ReferenceEvaluate(const std::vector<lg::Shape>& cands,
                       const std::vector<lg::Shape>& context,
                       const lg::RuleEntry* rules,
                       std::size_t rule_count,
                       std::vector<lg::Verdict>& out)
{
  out.assign(cands.size(), lg::Verdict{});
  for (std::size_t c = 0; c < cands.size(); ++c) {
    for (const auto& ctx : context) {
      for (std::size_t r = 0; r < rule_count; ++r) {
        if (rules[r].predicate(cands[c], ctx, rules[r].opaque)) {
          out[c].legal = false;
          out[c].triggered_rules |= static_cast<std::uint16_t>(
              1u << static_cast<std::uint8_t>(rules[r].type));
        }
      }
    }
  }
}

void SortByX1(std::vector<lg::Shape>& v)
{
  std::sort(v.begin(),
            v.end(),
            [](const lg::Shape& a, const lg::Shape& b) { return a.x1 < b.x1; });
}

bool TestSweepLineMatchesReference()
{
  std::mt19937 rng(0xC4FE);
  std::uniform_int_distribution<std::int32_t> coord(0, 1000);
  std::uniform_int_distribution<std::int16_t> layer_dist(0, 3);

  GapThreshold th{20};
  lg::RuleEntry rule{lg::RuleType::PrlSpacing, th.threshold,
                     SameLayerXGapPredicate, &th};

  for (int trial = 0; trial < 100; ++trial) {
    const std::size_t cc = 1 + (rng() % 200);
    const std::size_t kc = 1 + (rng() % 200);

    std::vector<lg::Shape> cands(cc);
    std::vector<lg::Shape> context(kc);
    for (auto& s : cands) {
      s.x1 = coord(rng);
      s.y1 = coord(rng);
      s.x2 = s.x1 + 1 + (rng() % 30);
      s.y2 = s.y1 + 1 + (rng() % 30);
      s.layer = layer_dist(rng);
      s.net_id = 1 + (rng() % 50);
    }
    for (auto& s : context) {
      s.x1 = coord(rng);
      s.y1 = coord(rng);
      s.x2 = s.x1 + 1 + (rng() % 30);
      s.y2 = s.y1 + 1 + (rng() % 30);
      s.layer = layer_dist(rng);
      s.net_id = (rng() % 4 == 0) ? 0 : 1 + (rng() % 50);
    }
    SortByX1(cands);
    SortByX1(context);

    std::vector<lg::Verdict> ref;
    std::vector<lg::Verdict> got(cc);
    ReferenceEvaluate(cands, context, &rule, 1, ref);

    lg::CpuDrcOracle oracle;
    oracle.SetRules(&rule, 1);
    oracle.Evaluate(cands.data(), cc, context.data(), kc, got.data());

    for (std::size_t i = 0; i < cc; ++i) {
      if (ref[i].legal != got[i].legal
          || ref[i].triggered_rules != got[i].triggered_rules) {
        std::fprintf(stderr,
                     "FAIL TestSweepLineMatchesReference trial=%d i=%zu "
                     "ref(legal=%d, mask=%u) got(legal=%d, mask=%u)\n",
                     trial,
                     i,
                     ref[i].legal,
                     ref[i].triggered_rules,
                     got[i].legal,
                     got[i].triggered_rules);
        return false;
      }
    }
  }
  return true;
}

bool TestEmptyAndDegenerate()
{
  lg::CpuDrcOracle oracle;
  GapThreshold th{10};
  lg::RuleEntry rule{lg::RuleType::PrlSpacing, th.threshold,
                     SameLayerXGapPredicate, &th};
  oracle.SetRules(&rule, 1);

  // No candidates: no-op, no crash.
  oracle.Evaluate(nullptr, 0, nullptr, 0, nullptr);

  // No context: all legal.
  std::vector<lg::Shape> cands(3);
  std::vector<lg::Verdict> verdicts(3);
  oracle.Evaluate(cands.data(), 3, nullptr, 0, verdicts.data());
  for (const auto& v : verdicts) {
    if (!v.legal || v.triggered_rules != 0) {
      std::fprintf(stderr, "FAIL TestEmptyAndDegenerate empty-context\n");
      return false;
    }
  }

  // No rules loaded: even with overlapping shapes, all legal.
  oracle.SetRules(nullptr, 0);
  std::vector<lg::Shape> ctx(3);
  for (auto& s : cands) {
    s.x1 = 0;
    s.y1 = 0;
    s.x2 = 100;
    s.y2 = 100;
    s.layer = 0;
  }
  for (auto& s : ctx) {
    s.x1 = 0;
    s.y1 = 0;
    s.x2 = 100;
    s.y2 = 100;
    s.layer = 0;
  }
  oracle.Evaluate(cands.data(), 3, ctx.data(), 3, verdicts.data());
  for (const auto& v : verdicts) {
    if (!v.legal) {
      std::fprintf(stderr, "FAIL TestEmptyAndDegenerate no-rules\n");
      return false;
    }
  }
  return true;
}

double BenchSweepLine(std::size_t cands_count, std::size_t ctx_count, int iters)
{
  std::mt19937 rng(0xBEAD);
  std::uniform_int_distribution<std::int32_t> coord(0, 100000);
  std::uniform_int_distribution<std::int16_t> layer_dist(0, 3);

  std::vector<lg::Shape> cands(cands_count);
  std::vector<lg::Shape> context(ctx_count);
  for (auto& s : cands) {
    s.x1 = coord(rng);
    s.y1 = coord(rng);
    s.x2 = s.x1 + 1 + (rng() % 200);
    s.y2 = s.y1 + 1 + (rng() % 200);
    s.layer = layer_dist(rng);
    s.net_id = 1 + (rng() % 50);
  }
  for (auto& s : context) {
    s.x1 = coord(rng);
    s.y1 = coord(rng);
    s.x2 = s.x1 + 1 + (rng() % 200);
    s.y2 = s.y1 + 1 + (rng() % 200);
    s.layer = layer_dist(rng);
    s.net_id = 1 + (rng() % 50);
  }
  SortByX1(cands);
  SortByX1(context);

  GapThreshold th{50};
  lg::RuleEntry rule{lg::RuleType::PrlSpacing, th.threshold,
                     SameLayerXGapPredicate, &th};

  lg::CpuDrcOracle oracle;
  oracle.SetRules(&rule, 1);

  std::vector<lg::Verdict> verdicts(cands_count);
  const auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    oracle.Evaluate(cands.data(),
                    cands_count,
                    context.data(),
                    ctx_count,
                    verdicts.data());
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(iters) * cands_count / secs;
}

}  // namespace

int main()
{
  if (!TestSweepLineMatchesReference()) {
    return 1;
  }
  std::printf(
      "PASS TestSweepLineMatchesReference (100 trials, randomized "
      "candidates+context+rules)\n");

  if (!TestEmptyAndDegenerate()) {
    return 1;
  }
  std::printf(
      "PASS TestEmptyAndDegenerate (zero counts, no-context, no-rules)\n");

  const double rate = BenchSweepLine(2048, 2048, 20);
  std::printf("Bench 2048 cands x 2048 context, 1 rule: %.3f Mcand/s\n",
              rate / 1e6);
  return 0;
}
