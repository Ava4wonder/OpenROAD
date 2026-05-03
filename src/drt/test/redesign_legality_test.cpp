// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.a + P2.2.b — Tests for CpuDrcOracle (sweep-line) and the four
// FastPass reference predicates. Built only when ENABLE_DRT_REDESIGN=ON.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "redesign/legality/CpuDrcOracle.h"
#include "redesign/legality/Predicates.h"

namespace lg = drt::redesign::legality;

namespace {

void SortByX1(std::vector<lg::Shape>& v)
{
  std::sort(v.begin(),
            v.end(),
            [](const lg::Shape& a, const lg::Shape& b) { return a.x1 < b.x1; });
}

// ---------- P2.2.a sweep-line vs synthetic-rule reference ----------

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
    return false;
  }
  const std::int32_t gap = std::max(cand.x1 - ctx.x2, ctx.x1 - cand.x2);
  return gap < th->threshold;
}

bool TestSweepLineMatchesReference()
{
  std::mt19937 rng(0xC4FE);
  std::uniform_int_distribution<std::int32_t> coord(0, 1000);
  std::uniform_int_distribution<std::int16_t> layer_dist(0, 3);

  GapThreshold th{20};
  lg::RuleEntry rule{lg::RuleType::PrlSpacing,
                     th.threshold,
                     SameLayerXGapPredicate,
                     &th};

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

    std::vector<lg::Verdict> ref(cc);
    std::vector<lg::Verdict> got(cc);
    lg::EvaluateReference(cands.data(), cc, context.data(), kc, &rule, 1, ref.data());

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
  lg::RuleEntry rule{lg::RuleType::PrlSpacing,
                     th.threshold,
                     SameLayerXGapPredicate,
                     &th};
  oracle.SetRules(&rule, 1);

  oracle.Evaluate(nullptr, 0, nullptr, 0, nullptr);

  std::vector<lg::Shape> cands(3);
  std::vector<lg::Verdict> verdicts(3);
  oracle.Evaluate(cands.data(), 3, nullptr, 0, verdicts.data());
  for (const auto& v : verdicts) {
    if (!v.legal || v.triggered_rules != 0) {
      std::fprintf(stderr, "FAIL TestEmptyAndDegenerate empty-context\n");
      return false;
    }
  }

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

// ---------- P2.2.b reference predicates: hand-built unit cases ----------

bool TestMetalShortReference()
{
  lg::Shape a{0, 0, 10, 10, 1, 100};
  lg::Shape b_overlap_other_net{5, 5, 15, 15, 1, 200};
  lg::Shape b_overlap_same_net{5, 5, 15, 15, 1, 100};
  lg::Shape b_disjoint_other_net{20, 20, 30, 30, 1, 200};
  lg::Shape b_overlap_other_layer{5, 5, 15, 15, 2, 200};
  lg::Shape b_overlap_blockage{5, 5, 15, 15, 1, 0};
  lg::MetalShortConfig cfg{};

  if (!lg::MetalShortReference(a, b_overlap_other_net, &cfg)) {
    std::fprintf(stderr, "FAIL MetalShort: same-layer different-net overlap\n");
    return false;
  }
  if (lg::MetalShortReference(a, b_overlap_same_net, &cfg)) {
    std::fprintf(stderr, "FAIL MetalShort: same-net should not violate\n");
    return false;
  }
  if (lg::MetalShortReference(a, b_disjoint_other_net, &cfg)) {
    std::fprintf(stderr, "FAIL MetalShort: disjoint should not violate\n");
    return false;
  }
  if (lg::MetalShortReference(a, b_overlap_other_layer, &cfg)) {
    std::fprintf(stderr,
                 "FAIL MetalShort: different-layer should not violate\n");
    return false;
  }
  if (!lg::MetalShortReference(a, b_overlap_blockage, &cfg)) {
    std::fprintf(stderr,
                 "FAIL MetalShort: blockage (net_id=0) overlap must violate\n");
    return false;
  }
  return true;
}

bool TestPrlSpacingReference()
{
  // cand sits at x in [0,10], y in [0,100]; ctx parallels at varying x.
  lg::Shape cand{0, 0, 10, 100, 1, 100};
  lg::PrlSpacingConfig cfg{20, 50};  // min_spacing=20, prl_threshold=50

  // y-overlap = 100 (>= 50), x_dist = 5 (< 20) → violation
  lg::Shape close{15, 0, 25, 100, 1, 200};
  if (!lg::PrlSpacingReference(cand, close, &cfg)) {
    std::fprintf(stderr, "FAIL PrlSpacing: close parallel run should violate\n");
    return false;
  }
  // y-overlap = 100 (>= 50), x_dist = 30 (>= 20) → no violation
  lg::Shape far_x{40, 0, 50, 100, 1, 200};
  if (lg::PrlSpacingReference(cand, far_x, &cfg)) {
    std::fprintf(stderr, "FAIL PrlSpacing: spacing >= min_spacing OK\n");
    return false;
  }
  // y-overlap = 30 (< 50 prl threshold) → no violation
  lg::Shape short_overlap{15, 70, 25, 100, 1, 200};
  if (lg::PrlSpacingReference(cand, short_overlap, &cfg)) {
    std::fprintf(stderr,
                 "FAIL PrlSpacing: y-overlap below prl_threshold should be OK\n");
    return false;
  }
  // Same net: skip
  lg::Shape close_same_net{15, 0, 25, 100, 1, 100};
  if (lg::PrlSpacingReference(cand, close_same_net, &cfg)) {
    std::fprintf(stderr, "FAIL PrlSpacing: same net should be OK\n");
    return false;
  }
  return true;
}

bool TestEolSpacingReference()
{
  lg::EolSpacingConfig cfg{30, 15, 5};
  // cand is a short vertical wire (10 wide x 20 tall); 20 < 30 → vertical
  // edges trigger EOL.
  lg::Shape cand{0, 0, 10, 20, 1, 100};
  // ctx within eol_spacing to the right and overlapping y → violation
  lg::Shape close_right{15, 5, 25, 15, 1, 200};
  if (!lg::EolSpacingReference(cand, close_right, &cfg)) {
    std::fprintf(stderr, "FAIL EolSpacing: close right ctx should violate\n");
    return false;
  }
  // ctx far right: x_dist = 50 - 10 = 40 (>= 15) → no violation
  lg::Shape far_right{50, 5, 60, 15, 1, 200};
  if (lg::EolSpacingReference(cand, far_right, &cfg)) {
    std::fprintf(stderr, "FAIL EolSpacing: far right ctx should be OK\n");
    return false;
  }
  // cand is a TALL wire (10 wide x 100 tall); 100 >= 30 → vertical edges
  // not EOL. No violation regardless of close ctx.
  lg::Shape tall{0, 0, 10, 100, 1, 100};
  lg::Shape close_to_tall{15, 50, 25, 60, 1, 200};
  if (lg::EolSpacingReference(tall, close_to_tall, &cfg)) {
    std::fprintf(stderr,
                 "FAIL EolSpacing: tall edge (above threshold) should not "
                 "trigger EOL\n");
    return false;
  }
  // Different layer
  lg::Shape close_other_layer{15, 5, 25, 15, 2, 200};
  if (lg::EolSpacingReference(cand, close_other_layer, &cfg)) {
    std::fprintf(stderr, "FAIL EolSpacing: different layer should be OK\n");
    return false;
  }
  return true;
}

bool TestCutSpacingReference()
{
  lg::CutSpacingConfig cfg{30};
  // Two cuts on layer 5 (cut layer), different nets
  lg::Shape cut_a{0, 0, 10, 10, 5, 100};
  lg::Shape cut_close{20, 0, 30, 10, 5, 200};  // x_gap = 10, y_gap = 0 → 10
  if (!lg::CutSpacingReference(cut_a, cut_close, &cfg)) {
    std::fprintf(stderr, "FAIL CutSpacing: 10-dbu spacing < 30 must violate\n");
    return false;
  }
  lg::Shape cut_far{40, 0, 50, 10, 5, 200};  // x_gap = 30 = min_spacing → OK
  if (lg::CutSpacingReference(cut_a, cut_far, &cfg)) {
    std::fprintf(stderr,
                 "FAIL CutSpacing: spacing == min_spacing must NOT violate\n");
    return false;
  }
  // Diagonal: x_gap=20, y_gap=20 → L_inf=20 < 30 → violation
  lg::Shape cut_diag{30, 30, 40, 40, 5, 200};
  if (!lg::CutSpacingReference(cut_a, cut_diag, &cfg)) {
    std::fprintf(stderr, "FAIL CutSpacing: diagonal L_inf=20 < 30 must violate\n");
    return false;
  }
  return true;
}

// ---------- Cross-check sweep-line oracle vs reference for ALL 4 rules ----------

bool TestSweepLineAllRulesMatchReference()
{
  std::mt19937 rng(0xD00D);
  std::uniform_int_distribution<std::int32_t> coord(0, 800);
  std::uniform_int_distribution<std::int16_t> layer_dist(1, 4);

  lg::MetalShortConfig short_cfg{};
  lg::PrlSpacingConfig prl_cfg{15, 30};
  lg::EolSpacingConfig eol_cfg{40, 20, 5};
  lg::CutSpacingConfig cut_cfg{25};
  lg::RuleEntry rules[4] = {
      {lg::RuleType::MetalShort, 0, lg::MetalShortReference, &short_cfg},
      {lg::RuleType::PrlSpacing,
       prl_cfg.min_spacing,
       lg::PrlSpacingReference,
       &prl_cfg},
      {lg::RuleType::EolSpacing,
       eol_cfg.eol_spacing,
       lg::EolSpacingReference,
       &eol_cfg},
      {lg::RuleType::CutSpacing,
       cut_cfg.min_spacing,
       lg::CutSpacingReference,
       &cut_cfg},
  };

  for (int trial = 0; trial < 80; ++trial) {
    const std::size_t cc = 1 + (rng() % 150);
    const std::size_t kc = 1 + (rng() % 150);
    std::vector<lg::Shape> cands(cc);
    std::vector<lg::Shape> context(kc);
    for (auto& s : cands) {
      s.x1 = coord(rng);
      s.y1 = coord(rng);
      s.x2 = s.x1 + 5 + (rng() % 40);
      s.y2 = s.y1 + 5 + (rng() % 40);
      s.layer = layer_dist(rng);
      s.net_id = 1 + (rng() % 30);
    }
    for (auto& s : context) {
      s.x1 = coord(rng);
      s.y1 = coord(rng);
      s.x2 = s.x1 + 5 + (rng() % 40);
      s.y2 = s.y1 + 5 + (rng() % 40);
      s.layer = layer_dist(rng);
      s.net_id = (rng() % 4 == 0) ? 0 : 1 + (rng() % 30);
    }
    SortByX1(cands);
    SortByX1(context);

    std::vector<lg::Verdict> ref(cc);
    std::vector<lg::Verdict> got(cc);
    lg::EvaluateReference(cands.data(), cc, context.data(), kc, rules, 4, ref.data());

    lg::CpuDrcOracle oracle;
    oracle.SetRules(rules, 4);
    oracle.Evaluate(cands.data(), cc, context.data(), kc, got.data());

    // Conservative-checker: oracle may have FALSE POSITIVES vs reference
    // (legal=false where ref says legal=true) but NEVER FALSE NEGATIVES.
    // The reference IS the ground truth; sweep-line should match exactly
    // for these reference predicates because they are the same predicates.
    for (std::size_t i = 0; i < cc; ++i) {
      if (ref[i].legal != got[i].legal
          || ref[i].triggered_rules != got[i].triggered_rules) {
        std::fprintf(
            stderr,
            "FAIL TestSweepLineAllRulesMatchReference trial=%d i=%zu cc=%zu "
            "kc=%zu ref(%d, %u) got(%d, %u)\n",
            trial,
            i,
            cc,
            kc,
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

// ---------- Pair-counted bench (P2 exit-criterion unit) ----------

struct BenchOut
{
  double cands_per_s;
  double pairs_per_s;
};

BenchOut BenchSweepLine(std::size_t cc, std::size_t kc, int iters)
{
  std::mt19937 rng(0xBEAD);
  std::uniform_int_distribution<std::int32_t> coord(0, 100000);
  std::uniform_int_distribution<std::int16_t> layer_dist(1, 4);

  std::vector<lg::Shape> cands(cc);
  std::vector<lg::Shape> context(kc);
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

  lg::MetalShortConfig short_cfg{};
  lg::PrlSpacingConfig prl_cfg{50, 20};
  lg::EolSpacingConfig eol_cfg{40, 25, 8};
  lg::CutSpacingConfig cut_cfg{30};
  lg::RuleEntry rules[4] = {
      {lg::RuleType::MetalShort, 0, lg::MetalShortReference, &short_cfg},
      {lg::RuleType::PrlSpacing,
       prl_cfg.min_spacing,
       lg::PrlSpacingReference,
       &prl_cfg},
      {lg::RuleType::EolSpacing,
       eol_cfg.eol_spacing,
       lg::EolSpacingReference,
       &eol_cfg},
      {lg::RuleType::CutSpacing,
       cut_cfg.min_spacing,
       lg::CutSpacingReference,
       &cut_cfg},
  };

  lg::CpuDrcOracle oracle;
  oracle.SetRules(rules, 4);
  std::vector<lg::Verdict> verdicts(cc);

  // Warm-up + measure
  oracle.Evaluate(cands.data(), cc, context.data(), kc, verdicts.data());
  std::size_t pairs_per_iter = oracle.LastPairsEvaluated();

  const auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    oracle.Evaluate(cands.data(), cc, context.data(), kc, verdicts.data());
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const double total_cands = static_cast<double>(iters) * cc;
  const double total_pairs = static_cast<double>(iters) * pairs_per_iter;
  return {total_cands / secs, total_pairs / secs};
}

}  // namespace

int main()
{
  if (!TestMetalShortReference()) {
    return 1;
  }
  std::printf("PASS TestMetalShortReference\n");
  if (!TestPrlSpacingReference()) {
    return 1;
  }
  std::printf("PASS TestPrlSpacingReference\n");
  if (!TestEolSpacingReference()) {
    return 1;
  }
  std::printf("PASS TestEolSpacingReference\n");
  if (!TestCutSpacingReference()) {
    return 1;
  }
  std::printf("PASS TestCutSpacingReference\n");

  if (!TestSweepLineMatchesReference()) {
    return 1;
  }
  std::printf(
      "PASS TestSweepLineMatchesReference (synthetic rule, 100 trials)\n");

  if (!TestEmptyAndDegenerate()) {
    return 1;
  }
  std::printf(
      "PASS TestEmptyAndDegenerate (zero counts, no-context, no-rules)\n");

  if (!TestSweepLineAllRulesMatchReference()) {
    return 1;
  }
  std::printf(
      "PASS TestSweepLineAllRulesMatchReference (4 rules, 80 trials)\n");

  const BenchOut b = BenchSweepLine(2048, 2048, 20);
  std::printf("Bench 2048 cands x 2048 ctx, 4 rules: %.3f Mcand/s, %.3f Mpair/s\n",
              b.cands_per_s / 1e6,
              b.pairs_per_s / 1e6);

  // P2 exit-criterion check (single-thread CPU oracle): >= 1M pair/s.
  if (b.pairs_per_s < 1.0e6) {
    std::fprintf(stderr,
                 "FAIL P2 single-thread CPU exit criterion: %.3f Mpair/s "
                 "< 1.000 Mpair/s target\n",
                 b.pairs_per_s / 1e6);
    return 1;
  }
  std::printf("PASS P2 single-thread CPU oracle bench (>=1 Mpair/s)\n");
  return 0;
}
