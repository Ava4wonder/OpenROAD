// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.b — hard-legality verdict, separate from soft scoring per
// v2_drt_redesign_plan.md §4.3.
//
// LegalityVerdict is authoritative for hard constraints. A Delta whose
// verdict is `legal == false` is NEVER commit-eligible regardless of
// any soft Score it might also have. The compiler enforces this via the
// type system: Score has no `legal` field, no DRC term, no marker
// fields, so there is no way to compose a "score that overrides
// legality." See Score.h.

#pragma once

#include <cstdint>
#include <vector>

namespace drt::redesign {

enum class MarkerKind : uint8_t {
  Short,
  PrlSpacing,
  EolSpacing,
  CutSpacing,
  MinWidth,
  MinArea,
  Other,
};

// V2.2.c.proj — provenance of a LegalityVerdict. The hard-gate
// discipline from V2.1.b says only verdicts that have actually been
// validated may make a proposal commit-eligible. Different sources
// have different evidentiary weight; the source is recorded on the
// verdict so consumers can refuse to commit on stub evidence.
enum class LegalitySource : uint8_t {
  // V2.2.c.proj default — eval was called but no real legality
  // oracle was wired. legal=true is a *placeholder* value; the
  // proposal MUST NOT be commit-eligible on this verdict.
  StubAssumeLegal,
  // The Delta's identity is incomplete (e.g., DeleteWire without
  // resolved_net_id, or unknown WriteFootprint). Eval cannot validate
  // it. legal=false; non-committable.
  UnresolvedFootprint,
  // V2.2.c.legality.synthetic — verdict came from the small synthetic
  // oracle (same-layer overlap / spacing-below-threshold). PoC-grade
  // legality, NOT a physical DRC model. Commit-eligible iff legal,
  // but consumers should not interpret these verdicts as real-PDK
  // signoff signals. Provenance kept distinct from CpuDrcOracle so
  // logs are honest about what ran.
  SyntheticOracle,
  // V2.2.c.legality.realpdk — verdict came from running the L2
  // legality oracle (CpuDrcOracle / RuleDeck path) against a real
  // PDK rule deck. Commit-eligible iff legal.
  CpuDrcOracle,
  // Future — verdict produced by the upstream FlexGCWorker exact
  // checker. The strictest source.
  UpstreamExact,
};

// V2.2.c.legality.synthetic — commit-eligibility rule.
// commit_eligible = legal ONLY for trusted legality sources:
//   SyntheticOracle (PoC)
//   CpuDrcOracle (real)
//   UpstreamExact (real)
// Stub and unresolved sources can never be commit-eligible regardless
// of `legal`. The eval impl applies this rule when constructing a
// LegalityVerdict — it must not be re-derived at commit time.
inline bool IsTrustedLegalitySource(LegalitySource s) noexcept
{
  switch (s) {
    case LegalitySource::SyntheticOracle:
    case LegalitySource::CpuDrcOracle:
    case LegalitySource::UpstreamExact:
      return true;
    case LegalitySource::StubAssumeLegal:
    case LegalitySource::UnresolvedFootprint:
      return false;
  }
  return false;
}

struct LegalityVerdict
{
  bool legal = true;
  std::vector<MarkerKind> violations;  // empty iff legal == true
  uint64_t marker_count_after = 0;
  // V2.2.c.proj — provenance + commit-eligibility gate.
  LegalitySource source = LegalitySource::StubAssumeLegal;
  // V2.1.b hard-gate, V2.2.c.proj surface: a proposal is
  // commit-eligible only when (legal == true AND source has been
  // validated). Stub legality always sets this false.
  bool commit_eligible = false;
};

}  // namespace drt::redesign
