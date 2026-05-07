// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.c.legality.synthetic — tiny PoC legality oracle.
//
// SyntheticOracle is a PoC legality source for overlay/eval/commit
// plumbing. It is not a physical DRC model and must not be used for
// QoR claims. Three rules, deliberately tiny:
//
//   1. same-layer overlap on candidate vs context shape → illegal
//      (Short violation)
//   2. same-layer spacing below kSyntheticSpacingThresholdDbu →
//      illegal (PrlSpacing violation)
//   3. different-layer → no check, legal
//
// The oracle exists to prove the architectural seam:
//
//   Delta → DeltaToOracleInput → OracleCandidateBatch
//        → SyntheticOracle::Evaluate → LegalityVerdict (source =
//          SyntheticOracle, commit_eligible = legal)
//
// without dragging in real-PDK rule-deck integration. Once the V2
// overlay/eval/commit loop is proven end-to-end, V2.2.c.legality.
// realpdk swaps SyntheticOracle out for the full CpuDrcOracle path.
//
// Activation: only via EvalOptions::legality_mode ==
// LegalityMode::SyntheticOracle. The default mode stays
// StubAssumeLegal (non-committable). Synthetic commit eligibility
// only exists when explicitly requested by tests / PoC paths.

#pragma once

#include <cstdint>

#include "../LegalityVerdict.h"
#include "GeometryView.h"
#include "OracleCandidate.h"

namespace drt::redesign::overlay {

class SyntheticOracle
{
 public:
  // Spacing threshold (DBU). Same-layer shapes whose nearest-edge
  // distance is below this are flagged as PrlSpacing violations.
  // Hardcoded for PoC — V2.2.c.legality.realpdk replaces with
  // per-layer rule-deck thresholds.
  static constexpr std::int32_t kSyntheticSpacingThresholdDbu = 50;

  // Evaluate a candidate batch against a base view. Returns a
  // LegalityVerdict with source = SyntheticOracle. The eval impl
  // applies the trusted-source rule:
  //   commit_eligible = legal AND IsTrustedLegalitySource(source)
  // SyntheticOracle is trusted under that rule, so a clean verdict
  // here makes the proposal commit-eligible — but only when the
  // caller explicitly opted into LegalityMode::SyntheticOracle.
  static LegalityVerdict Evaluate(const OracleCandidateBatch& batch,
                                  const GeometryView& base);
};

}  // namespace drt::redesign::overlay
