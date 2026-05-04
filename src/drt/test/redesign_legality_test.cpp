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

#include <sstream>
#include <string>

#include "db/tech/frConstraint.h"
#include "db/tech/frLayer.h"
#include "frBaseTypes.h"
#include "redesign/legality/ClipDump.h"
#include "redesign/legality/CpuDrcOracle.h"
#include "redesign/legality/FlexConstraintTranslator.h"
#include "redesign/legality/Predicates.h"
#include "redesign/legality/RuleDeck.h"

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

// ---------- P2.2.c: Mode A equivalence under halo relaxation ----------
//
// Mode A = oracle-internal equivalence: a tight per-rule halo set MUST
// produce the same verdicts as a loose halo set (since the per-rule halo
// pre-filter only skips pair-rule tuples where the predicate would have
// returned false anyway, given the rule's geometric precondition).
//
// Mode B (oracle vs upstream FlexGCWorker conservative correctness) lives
// in P2.2.d/e and is intentionally not exercised here.

bool TestModeAEquivalenceUnderHaloRelaxation()
{
  std::mt19937 rng(0xE17E);
  std::uniform_int_distribution<std::int32_t> coord(0, 800);
  std::uniform_int_distribution<std::int16_t> layer_dist(1, 4);

  lg::MetalShortConfig short_cfg{};
  lg::PrlSpacingConfig prl_cfg{15, 30};
  lg::EolSpacingConfig eol_cfg{40, 20, 5};
  lg::CutSpacingConfig cut_cfg{25};

  // Tight halos: the actual x-axis precondition of each rule.
  lg::RuleEntry tight[4] = {
      {lg::RuleType::MetalShort, 0, lg::MetalShortReference, &short_cfg},
      {lg::RuleType::PrlSpacing,
       prl_cfg.min_spacing,
       lg::PrlSpacingReference,
       &prl_cfg},
      {lg::RuleType::EolSpacing,
       std::max(eol_cfg.eol_spacing, eol_cfg.eol_within),
       lg::EolSpacingReference,
       &eol_cfg},
      {lg::RuleType::CutSpacing,
       cut_cfg.min_spacing,
       lg::CutSpacingReference,
       &cut_cfg},
  };
  // Loose halos: all rules set to the max tight halo, so the per-rule
  // pre-filter never trips (LastPairsAvoided will be 0).
  std::int32_t max_halo = 0;
  for (const auto& r : tight) {
    max_halo = std::max(max_halo, r.halo);
  }
  lg::RuleEntry loose[4] = {tight[0], tight[1], tight[2], tight[3]};
  for (auto& r : loose) {
    r.halo = max_halo;
  }

  for (int trial = 0; trial < 50; ++trial) {
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

    std::vector<lg::Verdict> v_tight(cc);
    std::vector<lg::Verdict> v_loose(cc);

    lg::CpuDrcOracle o_tight;
    o_tight.SetRules(tight, 4);
    o_tight.Evaluate(cands.data(), cc, context.data(), kc, v_tight.data());
    const std::size_t pairs_eval_tight = o_tight.LastPairsEvaluated();
    const std::size_t pairs_avoid_tight = o_tight.LastPairsAvoided();

    lg::CpuDrcOracle o_loose;
    o_loose.SetRules(loose, 4);
    o_loose.Evaluate(cands.data(), cc, context.data(), kc, v_loose.data());
    const std::size_t pairs_eval_loose = o_loose.LastPairsEvaluated();
    const std::size_t pairs_avoid_loose = o_loose.LastPairsAvoided();

    for (std::size_t i = 0; i < cc; ++i) {
      if (v_tight[i].legal != v_loose[i].legal
          || v_tight[i].triggered_rules != v_loose[i].triggered_rules) {
        std::fprintf(
            stderr,
            "FAIL TestModeAEquivalence trial=%d i=%zu tight(%d, %u) loose(%d, "
            "%u)\n",
            trial,
            i,
            v_tight[i].legal,
            v_tight[i].triggered_rules,
            v_loose[i].legal,
            v_loose[i].triggered_rules);
        return false;
      }
    }
    // Both setups admit the same total pairs through the outer sweep-line
    // (same max-halo). Per-rule pre-filter then partitions admitted into
    // (evaluated, avoided). Tight setup evaluates strictly fewer or equal,
    // and avoids at least as many.
    const std::size_t admitted_tight
        = pairs_eval_tight + pairs_avoid_tight;
    const std::size_t admitted_loose
        = pairs_eval_loose + pairs_avoid_loose;
    if (admitted_tight != admitted_loose) {
      std::fprintf(stderr,
                   "FAIL TestModeAEquivalence trial=%d: outer admitted "
                   "differs (tight=%zu vs loose=%zu); same max-halo should "
                   "produce same active set\n",
                   trial,
                   admitted_tight,
                   admitted_loose);
      return false;
    }
    if (pairs_eval_tight > pairs_eval_loose) {
      std::fprintf(stderr,
                   "FAIL TestModeAEquivalence trial=%d: tight evaluated "
                   "(%zu) exceeds loose evaluated (%zu); tighter halos "
                   "must skip at least as much\n",
                   trial,
                   pairs_eval_tight,
                   pairs_eval_loose);
      return false;
    }
  }
  return true;
}

// ---------- P2.2.d preflight: RuleDeck + Translator stub ----------

bool TestRuleDeckCoverageAccounting()
{
  lg::RuleDeck deck;
  if (deck.GetCoverage().total_input != 0
      || deck.GetCoverage().supported != 0) {
    std::fprintf(stderr, "FAIL TestRuleDeckCoverageAccounting empty deck\n");
    return false;
  }

  // One Supported with explicit layer info.
  lg::NormalizedRule supported_explicit{};
  supported_explicit.family = lg::RuleFamily::PrlSpacing;
  supported_explicit.coverage = lg::RuleCoverage::Supported;
  supported_explicit.params = lg::PrlSpacingConfig{15, 30};
  supported_explicit.layer_filter = std::int16_t{1};
  supported_explicit.layer_knownness = lg::LayerKnownness::Explicit;
  supported_explicit.tag = "M2:test";
  supported_explicit.halo = 15;
  deck.Add(supported_explicit);

  // One Supported with unknown layer info (the current translator default).
  lg::NormalizedRule supported_unknown{};
  supported_unknown.family = lg::RuleFamily::MetalShort;
  supported_unknown.coverage = lg::RuleCoverage::Supported;
  supported_unknown.params = lg::MetalShortConfig{};
  supported_unknown.layer_knownness = lg::LayerKnownness::Unknown;
  supported_unknown.tag = "Mx:short";
  supported_unknown.halo = 0;
  deck.Add(supported_unknown);

  lg::NormalizedRule fallback{};
  fallback.family = lg::RuleFamily::PrlSpacing;
  fallback.coverage = lg::RuleCoverage::Fallback;
  fallback.layer_filter = std::int16_t{2};
  fallback.layer_knownness = lg::LayerKnownness::Explicit;
  fallback.tag = "M2:spacing_table";
  deck.Add(fallback);

  deck.AddUnsupported();
  deck.AddUnsupported();

  const auto cov = deck.GetCoverage();
  if (cov.total_input != 5 || cov.supported != 2 || cov.fallback != 1
      || cov.unsupported != 2) {
    std::fprintf(stderr,
                 "FAIL TestRuleDeckCoverageAccounting: got "
                 "(total=%zu, sup=%zu, fb=%zu, unsup=%zu)\n",
                 cov.total_input,
                 cov.supported,
                 cov.fallback,
                 cov.unsupported);
    return false;
  }
  if (cov.supported_explicit != 1 || cov.supported_unknown != 1) {
    std::fprintf(stderr,
                 "FAIL TestRuleDeckCoverageAccounting layer-knownness split: "
                 "(sup_explicit=%zu, sup_unknown=%zu)\n",
                 cov.supported_explicit,
                 cov.supported_unknown);
    return false;
  }
  if (deck.Size() != 3) {
    // Note: AddUnsupported() does NOT add a NormalizedRule, only
    // increments counters; deck size reflects only Add() calls.
    std::fprintf(stderr,
                 "FAIL TestRuleDeckCoverageAccounting Size=%zu (expected 3)\n",
                 deck.Size());
    return false;
  }
  return true;
}

bool TestRuleDeckToRuleEntriesRoundTrip()
{
  lg::RuleDeck deck;

  lg::NormalizedRule short_rule{};
  short_rule.family = lg::RuleFamily::MetalShort;
  short_rule.coverage = lg::RuleCoverage::Supported;
  short_rule.params = lg::MetalShortConfig{};
  short_rule.halo = 0;
  deck.Add(short_rule);

  lg::NormalizedRule prl{};
  prl.family = lg::RuleFamily::PrlSpacing;
  prl.coverage = lg::RuleCoverage::Supported;
  prl.params = lg::PrlSpacingConfig{20, 50};
  prl.halo = 20;
  deck.Add(prl);

  lg::NormalizedRule fallback{};
  fallback.family = lg::RuleFamily::EolSpacing;
  fallback.coverage = lg::RuleCoverage::Fallback;
  deck.Add(fallback);

  auto entries = deck.ToRuleEntries();
  if (entries.size() != 2) {
    std::fprintf(stderr,
                 "FAIL TestRuleDeckToRuleEntriesRoundTrip size=%zu (expected "
                 "2 supported)\n",
                 entries.size());
    return false;
  }

  // Drive the oracle with the materialized entries on a tiny case to
  // confirm the param-pointer hand-off works.
  lg::CpuDrcOracle oracle;
  oracle.SetRules(entries.data(), entries.size());
  std::vector<lg::Shape> cands{
      {0, 0, 10, 10, 1, 100},     // candidate 0
      {200, 0, 210, 10, 1, 100},  // candidate 1, far from any context
  };
  std::vector<lg::Shape> context{
      {5, 5, 15, 15, 1, 200},  // overlaps cand 0 different net -> short
  };
  std::vector<lg::Verdict> verdicts(2);
  oracle.Evaluate(cands.data(), 2, context.data(), 1, verdicts.data());
  if (verdicts[0].legal) {
    std::fprintf(stderr,
                 "FAIL TestRuleDeckToRuleEntriesRoundTrip: cand 0 should "
                 "violate MetalShort\n");
    return false;
  }
  if (!verdicts[1].legal) {
    std::fprintf(stderr,
                 "FAIL TestRuleDeckToRuleEntriesRoundTrip: cand 1 (far) "
                 "should be legal\n");
    return false;
  }
  return true;
}

bool TestFlexConstraintTranslatorNullsAreUnsupported()
{
  // Null pointers count as Unsupported (no rule produced).
  lg::FlexConstraintTranslator t;
  const drt::frConstraint* fake_inputs[3] = {nullptr, nullptr, nullptr};
  auto deck = t.Translate(fake_inputs, 3);
  const auto cov = deck.GetCoverage();
  if (cov.total_input != 3 || cov.supported != 0 || cov.fallback != 0
      || cov.unsupported != 3) {
    std::fprintf(stderr,
                 "FAIL TestFlexConstraintTranslatorNullsAreUnsupported: "
                 "(total=%zu, sup=%zu, fb=%zu, unsup=%zu)\n",
                 cov.total_input,
                 cov.supported,
                 cov.fallback,
                 cov.unsupported);
    return false;
  }
  return true;
}

// ---------- P2.2.d translation tests against real upstream constraints ----------

bool TestTranslateShort()
{
  // No layer attached -> translator stays Unknown.
  drt::frShortConstraint sc;
  const drt::frConstraint* in[1] = {&sc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().supported != 1 || deck.Size() != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateShort: expected 1 supported rule\n");
    return false;
  }
  const auto& r = deck.At(0);
  if (r.family != lg::RuleFamily::MetalShort
      || r.coverage != lg::RuleCoverage::Supported || r.halo != 0) {
    std::fprintf(stderr, "FAIL TestTranslateShort: unexpected rule shape\n");
    return false;
  }
  if (r.layer_knownness != lg::LayerKnownness::Unknown
      || r.layer_filter.has_value()) {
    std::fprintf(stderr,
                 "FAIL TestTranslateShort: layer model expected "
                 "(no_value, Unknown) when no upstream layer attached\n");
    return false;
  }
  if (deck.GetCoverage().supported_unknown != 1
      || deck.GetCoverage().supported_explicit != 0) {
    std::fprintf(stderr,
                 "FAIL TestTranslateShort: coverage layer-split expected "
                 "(explicit=0, unknown=1) got (explicit=%zu, unknown=%zu)\n",
                 deck.GetCoverage().supported_explicit,
                 deck.GetCoverage().supported_unknown);
    return false;
  }
  return true;
}

bool TestTranslateShortWithExplicitLayer()
{
  // Attach an upstream frLayer with layerNum=4. Translator should now
  // emit (layer_filter=4, Explicit) and increment supported_explicit.
  drt::frLayer m4;
  m4.setLayerNum(4);
  drt::frShortConstraint sc;
  sc.setLayer(&m4);

  const drt::frConstraint* in[1] = {&sc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);

  if (deck.GetCoverage().supported_explicit != 1
      || deck.GetCoverage().supported_unknown != 0) {
    std::fprintf(stderr,
                 "FAIL TestTranslateShortWithExplicitLayer: coverage split "
                 "expected (explicit=1, unknown=0) got (explicit=%zu, "
                 "unknown=%zu)\n",
                 deck.GetCoverage().supported_explicit,
                 deck.GetCoverage().supported_unknown);
    return false;
  }
  const auto& r = deck.At(0);
  if (r.layer_knownness != lg::LayerKnownness::Explicit
      || !r.layer_filter.has_value() || r.layer_filter.value() != 4) {
    std::fprintf(stderr,
                 "FAIL TestTranslateShortWithExplicitLayer: layer model "
                 "expected (4, Explicit)\n");
    return false;
  }
  return true;
}

bool TestTranslateSpacing()
{
  drt::frSpacingConstraint sc(75);
  const drt::frConstraint* in[1] = {&sc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().supported != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateSpacing: expected supported=1, got %zu\n",
                 deck.GetCoverage().supported);
    return false;
  }
  const auto& r = deck.At(0);
  if (r.family != lg::RuleFamily::PrlSpacing
      || r.coverage != lg::RuleCoverage::Supported || r.halo != 75) {
    std::fprintf(stderr, "FAIL TestTranslateSpacing: shape\n");
    return false;
  }
  const auto* cfg = std::get_if<lg::PrlSpacingConfig>(&r.params);
  if (cfg == nullptr || cfg->min_spacing != 75 || cfg->prl_threshold != 0) {
    std::fprintf(stderr,
                 "FAIL TestTranslateSpacing: params mismatch\n");
    return false;
  }
  return true;
}

bool TestTranslateSpacingSamenetIsFallback()
{
  drt::frSpacingSamenetConstraint sc(50, false);
  const drt::frConstraint* in[1] = {&sc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().fallback != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateSpacingSamenetIsFallback: expected "
                 "fallback=1, got %zu\n",
                 deck.GetCoverage().fallback);
    return false;
  }
  const auto& r = deck.At(0);
  if (r.family != lg::RuleFamily::PrlSpacing
      || r.coverage != lg::RuleCoverage::Fallback) {
    std::fprintf(stderr, "FAIL TestTranslateSpacingSamenetIsFallback: shape\n");
    return false;
  }
  return true;
}

bool TestTranslateEolSupported()
{
  drt::frSpacingEndOfLineConstraint ec;
  ec.setMinSpacing(20);
  ec.setEolWidth(40);
  ec.setEolWithin(8);
  // No setParSpace -> hasParallelEdge() == false; no setTwoEdges
  const drt::frConstraint* in[1] = {&ec};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().supported != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateEolSupported: expected supported=1\n");
    return false;
  }
  const auto& r = deck.At(0);
  if (r.family != lg::RuleFamily::EolSpacing
      || r.coverage != lg::RuleCoverage::Supported) {
    std::fprintf(stderr, "FAIL TestTranslateEolSupported: shape\n");
    return false;
  }
  const auto* cfg = std::get_if<lg::EolSpacingConfig>(&r.params);
  if (cfg == nullptr || cfg->eol_width_threshold != 40 || cfg->eol_spacing != 20
      || cfg->eol_within != 8) {
    std::fprintf(stderr, "FAIL TestTranslateEolSupported: params\n");
    return false;
  }
  if (r.halo != std::max(20, 8)) {
    std::fprintf(stderr,
                 "FAIL TestTranslateEolSupported: halo expected 20 got %d\n",
                 r.halo);
    return false;
  }
  return true;
}

bool TestTranslateEolWithParallelEdgeIsFallback()
{
  drt::frSpacingEndOfLineConstraint ec;
  ec.setMinSpacing(20);
  ec.setEolWidth(40);
  ec.setEolWithin(8);
  ec.setParSpace(10);   // activates parallel-edge condition
  ec.setParWithin(5);
  const drt::frConstraint* in[1] = {&ec};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().fallback != 1) {
    std::fprintf(
        stderr,
        "FAIL TestTranslateEolWithParallelEdgeIsFallback: expected fallback=1, "
        "got %zu\n",
        deck.GetCoverage().fallback);
    return false;
  }
  return true;
}

bool TestTranslateCutSpacingMinimal()
{
  drt::frCutSpacingConstraint cc(/*cutSpacing*/ 30,
                                 /*centerToCenter*/ false,
                                 /*sameNet*/ false,
                                 /*secondLayerName*/ {},
                                 /*stack*/ false,
                                 /*adjacentCuts*/ -1,
                                 /*cutWithin*/ -1,
                                 /*isExceptSamePGNet*/ false,
                                 /*isParallelOverlap*/ false,
                                 /*cutArea*/ -1);
  const drt::frConstraint* in[1] = {&cc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().supported != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateCutSpacingMinimal: expected supported=1\n");
    return false;
  }
  const auto& r = deck.At(0);
  const auto* cfg = std::get_if<lg::CutSpacingConfig>(&r.params);
  if (cfg == nullptr || cfg->min_spacing != 30 || r.halo != 30) {
    std::fprintf(stderr, "FAIL TestTranslateCutSpacingMinimal: params/halo\n");
    return false;
  }
  return true;
}

bool TestTranslateCutSpacingExtendedIsFallback()
{
  // adjacentCuts > -1 activates the extended ADJACENTCUTS path.
  drt::frCutSpacingConstraint cc(/*cutSpacing*/ 30,
                                 /*centerToCenter*/ false,
                                 /*sameNet*/ false,
                                 /*secondLayerName*/ {},
                                 /*stack*/ false,
                                 /*adjacentCuts*/ 3,
                                 /*cutWithin*/ 50,
                                 /*isExceptSamePGNet*/ false,
                                 /*isParallelOverlap*/ false,
                                 /*cutArea*/ -1);
  const drt::frConstraint* in[1] = {&cc};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().fallback != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateCutSpacingExtendedIsFallback: expected "
                 "fallback=1, got %zu\n",
                 deck.GetCoverage().fallback);
    return false;
  }
  return true;
}

bool TestTranslateUnknownIsUnsupported()
{
  // frMinWidthConstraint isn't in any of our families.
  drt::frMinWidthConstraint mw(20);
  const drt::frConstraint* in[1] = {&mw};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 1);
  if (deck.GetCoverage().unsupported != 1 || deck.Size() != 0) {
    std::fprintf(stderr,
                 "FAIL TestTranslateUnknownIsUnsupported: (unsup=%zu, "
                 "deck.Size=%zu)\n",
                 deck.GetCoverage().unsupported,
                 deck.Size());
    return false;
  }
  return true;
}

bool TestTranslateBatchCoverageMix()
{
  drt::frShortConstraint sc;
  drt::frSpacingConstraint sp(40);
  drt::frSpacingSamenetConstraint sn(50, false);  // Fallback
  drt::frMinWidthConstraint mw(20);                   // Unsupported
  const drt::frConstraint* in[4] = {&sc, &sp, &sn, &mw};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 4);
  const auto cov = deck.GetCoverage();
  if (cov.total_input != 4 || cov.supported != 2 || cov.fallback != 1
      || cov.unsupported != 1) {
    std::fprintf(stderr,
                 "FAIL TestTranslateBatchCoverageMix: (total=%zu, sup=%zu, "
                 "fb=%zu, unsup=%zu)\n",
                 cov.total_input,
                 cov.supported,
                 cov.fallback,
                 cov.unsupported);
    return false;
  }
  // Materialize: only the 2 Supported should appear.
  if (deck.ToRuleEntries().size() != 2) {
    std::fprintf(stderr,
                 "FAIL TestTranslateBatchCoverageMix: ToRuleEntries=%zu "
                 "(expected 2)\n",
                 deck.ToRuleEntries().size());
    return false;
  }
  return true;
}

bool TestTranslateThenOracleEvaluate()
{
  // End-to-end: translate real upstream constraints, hand them to the
  // oracle, verify the resulting verdict on a tiny synthetic clip.
  drt::frShortConstraint sc;
  drt::frSpacingConstraint sp(50);
  const drt::frConstraint* in[2] = {&sc, &sp};
  lg::FlexConstraintTranslator t;
  auto deck = t.Translate(in, 2);
  auto entries = deck.ToRuleEntries();
  if (entries.size() != 2) {
    std::fprintf(
        stderr,
        "FAIL TestTranslateThenOracleEvaluate: expected 2 entries, got %zu\n",
        entries.size());
    return false;
  }

  lg::CpuDrcOracle oracle;
  oracle.SetRules(entries.data(), entries.size());

  // Two shapes on layer 1 different nets, 30 dbu apart in x: should
  // violate the spacing rule (50 > 30) but not the short rule.
  std::vector<lg::Shape> cands{{0, 0, 10, 10, 1, 100}};
  std::vector<lg::Shape> context{{40, 0, 50, 10, 1, 200}};
  std::vector<lg::Verdict> verdicts(1);
  oracle.Evaluate(cands.data(), 1, context.data(), 1, verdicts.data());
  if (verdicts[0].legal) {
    std::fprintf(stderr,
                 "FAIL TestTranslateThenOracleEvaluate: spacing 30<50 should "
                 "violate\n");
    return false;
  }
  const auto prl_bit = static_cast<std::uint16_t>(
      1u << static_cast<std::uint8_t>(lg::RuleType::PrlSpacing));
  const auto short_bit = static_cast<std::uint16_t>(
      1u << static_cast<std::uint8_t>(lg::RuleType::MetalShort));
  if ((verdicts[0].triggered_rules & prl_bit) == 0) {
    std::fprintf(stderr,
                 "FAIL TestTranslateThenOracleEvaluate: PrlSpacing bit not "
                 "set, mask=%u\n",
                 verdicts[0].triggered_rules);
    return false;
  }
  if (verdicts[0].triggered_rules & short_bit) {
    std::fprintf(stderr,
                 "FAIL TestTranslateThenOracleEvaluate: MetalShort bit "
                 "spuriously set on non-overlapping pair\n");
    return false;
  }
  return true;
}

// ---------- P2.2.e.1.a: ClipDump round-trip ----------

bool ShapesEqual(const lg::Shape& a, const lg::Shape& b)
{
  return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2
         && a.layer == b.layer && a.net_id == b.net_id;
}

bool LabelsEqual(const lg::ProjectedUpstreamLabel& a,
                 const lg::ProjectedUpstreamLabel& b)
{
  return a.projected_marker_count == b.projected_marker_count
         && a.projected_legal == b.projected_legal
         && a.projection_ambiguous == b.projection_ambiguous;
}

bool MetaEqual(const lg::ClipMeta& a, const lg::ClipMeta& b)
{
  return a.clip_id == b.clip_id && a.design == b.design && a.pdk == b.pdk
         && a.tech_hash == b.tech_hash
         && a.rule_deck_fingerprint == b.rule_deck_fingerprint
         && a.clip_x1 == b.clip_x1 && a.clip_y1 == b.clip_y1
         && a.clip_x2 == b.clip_x2 && a.clip_y2 == b.clip_y2
         && a.route_x1 == b.route_x1 && a.route_y1 == b.route_y1
         && a.route_x2 == b.route_x2 && a.route_y2 == b.route_y2;
}

lg::ClipRecord MakeSyntheticRecord()
{
  lg::ClipRecord r;
  r.meta.clip_id = 0xDEADBEEFCAFEBABEull;
  r.meta.design = "asap7_gcd";
  r.meta.pdk = "asap7";
  r.meta.tech_hash = 0x0102030405060708ull;
  r.meta.rule_deck_fingerprint = 0xAABBCCDDEEFF0011ull;
  r.meta.clip_x1 = -100;
  r.meta.clip_y1 = -200;
  r.meta.clip_x2 = 1000;
  r.meta.clip_y2 = 1500;
  r.meta.route_x1 = 0;
  r.meta.route_y1 = 0;
  r.meta.route_x2 = 800;
  r.meta.route_y2 = 1200;

  r.candidates = {
      {0, 0, 10, 10, 1, 100},
      {50, 50, 60, 60, 1, 200},
      {200, 200, 220, 240, 2, 300},
  };
  r.context = {
      {-50, -50, -10, -10, 1, 0},   // blockage (net 0)
      {500, 500, 510, 510, 2, 400},
  };
  r.labels = {
      {0u, 1u, 0u},  // legal, no markers
      {3u, 0u, 1u},  // 3 markers, illegal, ambiguous projection
      {1u, 0u, 0u},  // 1 marker, illegal, unambiguous
  };
  return r;
}

bool TestClipDumpRoundTrip()
{
  std::stringstream s;
  lg::WriteHeader(s);

  const lg::ClipRecord original = MakeSyntheticRecord();
  lg::WriteRecord(s, original);

  // Write a second record so we exercise sequence handling too.
  lg::ClipRecord empty_clip;
  empty_clip.meta.clip_id = 42;
  empty_clip.meta.design = "asap7_ibex";
  empty_clip.meta.pdk = "asap7";
  // Empty candidates / context / labels — must round-trip cleanly.
  lg::WriteRecord(s, empty_clip);

  std::uint32_t version = 0;
  if (!lg::ReadHeader(s, &version)) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: ReadHeader\n");
    return false;
  }
  if (version != lg::kClipDumpVersion) {
    std::fprintf(stderr,
                 "FAIL TestClipDumpRoundTrip: version mismatch (%08x vs %08x)\n",
                 version,
                 lg::kClipDumpVersion);
    return false;
  }

  lg::ClipRecord rt;
  if (!lg::ReadRecord(s, &rt)) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: ReadRecord(0)\n");
    return false;
  }
  if (!MetaEqual(rt.meta, original.meta)) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: meta mismatch\n");
    return false;
  }
  if (rt.candidates.size() != original.candidates.size()
      || rt.context.size() != original.context.size()
      || rt.labels.size() != original.labels.size()) {
    std::fprintf(stderr,
                 "FAIL TestClipDumpRoundTrip: count mismatch "
                 "(cand %zu/%zu, ctx %zu/%zu, lbl %zu/%zu)\n",
                 rt.candidates.size(), original.candidates.size(),
                 rt.context.size(), original.context.size(),
                 rt.labels.size(), original.labels.size());
    return false;
  }
  for (std::size_t i = 0; i < original.candidates.size(); ++i) {
    if (!ShapesEqual(rt.candidates[i], original.candidates[i])) {
      std::fprintf(stderr,
                   "FAIL TestClipDumpRoundTrip: candidate %zu mismatch\n",
                   i);
      return false;
    }
  }
  for (std::size_t i = 0; i < original.context.size(); ++i) {
    if (!ShapesEqual(rt.context[i], original.context[i])) {
      std::fprintf(stderr,
                   "FAIL TestClipDumpRoundTrip: context %zu mismatch\n",
                   i);
      return false;
    }
  }
  for (std::size_t i = 0; i < original.labels.size(); ++i) {
    if (!LabelsEqual(rt.labels[i], original.labels[i])) {
      std::fprintf(stderr,
                   "FAIL TestClipDumpRoundTrip: label %zu mismatch\n",
                   i);
      return false;
    }
  }

  // Second record (empty payload).
  lg::ClipRecord rt2;
  if (!lg::ReadRecord(s, &rt2)) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: ReadRecord(1)\n");
    return false;
  }
  if (rt2.meta.clip_id != 42 || rt2.meta.design != "asap7_ibex"
      || !rt2.candidates.empty() || !rt2.context.empty()
      || !rt2.labels.empty()) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: empty-record mismatch\n");
    return false;
  }

  // EOF: third ReadRecord call returns false cleanly.
  lg::ClipRecord rt_eof;
  if (lg::ReadRecord(s, &rt_eof)) {
    std::fprintf(stderr, "FAIL TestClipDumpRoundTrip: expected EOF\n");
    return false;
  }
  return true;
}

bool TestClipDumpBadMagicRejected()
{
  std::stringstream s;
  // Write something that is NOT the magic prefix.
  const char garbage[8] = {'X', 'X', 'X', 'X', 0, 0, 0, 0};
  s.write(garbage, 8);

  std::uint32_t v = 0;
  if (lg::ReadHeader(s, &v)) {
    std::fprintf(stderr,
                 "FAIL TestClipDumpBadMagicRejected: ReadHeader should fail\n");
    return false;
  }
  return true;
}

// ---------- Pair-counted bench (P2 exit-criterion unit) ----------

struct BenchOut
{
  double cands_per_s;
  double pairs_per_s;
  std::size_t pairs_admitted;  // by outer max-halo sweep-line
  std::size_t pairs_evaluated; // predicate actually called
  std::size_t pairs_avoided;   // skipped by per-rule halo pre-filter
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
       std::max(eol_cfg.eol_spacing, eol_cfg.eol_within),
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

  // Warm-up + capture pair counts (constant across iters for fixed input).
  oracle.Evaluate(cands.data(), cc, context.data(), kc, verdicts.data());
  const std::size_t pairs_eval_per_iter = oracle.LastPairsEvaluated();
  const std::size_t pairs_avoid_per_iter = oracle.LastPairsAvoided();
  const std::size_t pairs_admitted_per_iter
      = pairs_eval_per_iter + pairs_avoid_per_iter;

  const auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    oracle.Evaluate(cands.data(), cc, context.data(), kc, verdicts.data());
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const double total_cands = static_cast<double>(iters) * cc;
  const double total_pairs = static_cast<double>(iters) * pairs_eval_per_iter;
  return {total_cands / secs,
          total_pairs / secs,
          pairs_admitted_per_iter,
          pairs_eval_per_iter,
          pairs_avoid_per_iter};
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

  if (!TestModeAEquivalenceUnderHaloRelaxation()) {
    return 1;
  }
  std::printf(
      "PASS TestModeAEquivalenceUnderHaloRelaxation (50 trials, "
      "tight vs loose halo, exact verdict equivalence + pair accounting)\n");

  if (!TestRuleDeckCoverageAccounting()) {
    return 1;
  }
  std::printf("PASS TestRuleDeckCoverageAccounting\n");
  if (!TestRuleDeckToRuleEntriesRoundTrip()) {
    return 1;
  }
  std::printf(
      "PASS TestRuleDeckToRuleEntriesRoundTrip (oracle drives "
      "materialized entries)\n");
  if (!TestFlexConstraintTranslatorNullsAreUnsupported()) {
    return 1;
  }
  std::printf("PASS TestFlexConstraintTranslatorNullsAreUnsupported\n");
  if (!TestTranslateShort()) {
    return 1;
  }
  std::printf("PASS TestTranslateShort\n");
  if (!TestTranslateShortWithExplicitLayer()) {
    return 1;
  }
  std::printf("PASS TestTranslateShortWithExplicitLayer\n");
  if (!TestTranslateSpacing()) {
    return 1;
  }
  std::printf("PASS TestTranslateSpacing\n");
  if (!TestTranslateSpacingSamenetIsFallback()) {
    return 1;
  }
  std::printf("PASS TestTranslateSpacingSamenetIsFallback\n");
  if (!TestTranslateEolSupported()) {
    return 1;
  }
  std::printf("PASS TestTranslateEolSupported\n");
  if (!TestTranslateEolWithParallelEdgeIsFallback()) {
    return 1;
  }
  std::printf("PASS TestTranslateEolWithParallelEdgeIsFallback\n");
  if (!TestTranslateCutSpacingMinimal()) {
    return 1;
  }
  std::printf("PASS TestTranslateCutSpacingMinimal\n");
  if (!TestTranslateCutSpacingExtendedIsFallback()) {
    return 1;
  }
  std::printf("PASS TestTranslateCutSpacingExtendedIsFallback\n");
  if (!TestTranslateUnknownIsUnsupported()) {
    return 1;
  }
  std::printf("PASS TestTranslateUnknownIsUnsupported\n");
  if (!TestTranslateBatchCoverageMix()) {
    return 1;
  }
  std::printf("PASS TestTranslateBatchCoverageMix (4 inputs across all 3 coverages)\n");
  if (!TestTranslateThenOracleEvaluate()) {
    return 1;
  }
  std::printf(
      "PASS TestTranslateThenOracleEvaluate (real upstream -> normalized "
      "-> oracle verdict)\n");

  if (!TestClipDumpRoundTrip()) {
    return 1;
  }
  std::printf(
      "PASS TestClipDumpRoundTrip (header + 2 records, byte-equal "
      "round-trip + clean EOF)\n");
  if (!TestClipDumpBadMagicRejected()) {
    return 1;
  }
  std::printf("PASS TestClipDumpBadMagicRejected\n");

  const BenchOut b = BenchSweepLine(2048, 2048, 20);
  const double avoid_ratio = b.pairs_admitted == 0
                                 ? 0.0
                                 : 100.0 * static_cast<double>(b.pairs_avoided)
                                       / static_cast<double>(b.pairs_admitted);
  std::printf("Bench 2048 cands x 2048 ctx, 4 rules:\n"
              "  throughput          : %.3f Mcand/s, %.3f Mpair/s\n"
              "  pairs admitted/iter : %zu (by outer max-halo sweep-line)\n"
              "  pairs evaluated/iter: %zu (predicate actually called)\n"
              "  pairs avoided/iter  : %zu (per-rule halo pre-filter)\n"
              "  pre-filter savings  : %.1f%% of admitted pairs\n",
              b.cands_per_s / 1e6,
              b.pairs_per_s / 1e6,
              b.pairs_admitted,
              b.pairs_evaluated,
              b.pairs_avoided,
              avoid_ratio);

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
