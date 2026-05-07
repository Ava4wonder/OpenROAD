// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.b — wrapper return type for PhysicalState::eval. Replaces the
// pre-V2 EvaluationResult struct that blended hard legality with soft
// scoring; see v2_drt_redesign_plan.md §4.3 for the hard/soft split.
//
// `score` is `optional` because soft scoring is skipped when the
// proposal is illegal — unless the caller explicitly requests
// speculative scoring via EvalOptions::score_even_if_illegal. A
// proposal with `legality.legal == false` is NEVER commit-eligible
// regardless of any speculative score it carries; ConflictPolicy
// assumes inputs already passed legality.

#pragma once

#include <cstdint>
#include <optional>

#include "LegalityVerdict.h"
#include "Score.h"

namespace drt::redesign {

// V2.2.c.bridge — selectable legality backend. The PoC path can
// advance with SyntheticOracle without waiting on real-PDK RuleDeck
// coverage. eval picks its source from this option.
enum class LegalityMode : std::uint8_t {
  StubAssumeLegal,        // V2.2.c.proj default — placeholder, not
                          // committable. Wirelength/score still
                          // computed for ranking experiments.
  SyntheticOracle,        // V2.2.c.legality.synthetic — synthetic
                          // RuleDeck (same-layer overlap = illegal,
                          // spacing < threshold = illegal, etc.) so
                          // the architectural seam can be exercised
                          // without rule-deck archaeology.
  CpuDrcOracleRealDeck,   // V2.2.c.legality.realpdk — full
                          // CpuDrcOracle backed by real ASAP7-style
                          // RuleDeck. Final form.
};

struct EvalOptions
{
  // Compute Score even if LegalityVerdict::legal == false. Used for
  // search heuristics that learn from illegal proposals (e.g. to
  // measure how close a candidate is to legal). Such proposals are
  // marked speculative-only; they cannot be committed.
  bool score_even_if_illegal = false;

  // V2.2.c.bridge — legality backend selection. Defaults to the
  // stub source so V2.2.c.proj behaviour is preserved when eval is
  // called without explicit options.
  LegalityMode legality_mode = LegalityMode::StubAssumeLegal;
};

struct EvalOutcome
{
  LegalityVerdict legality;
  // Present iff legality.legal == true OR
  //         opts.score_even_if_illegal was set on the eval call.
  std::optional<Score> score;
};

}  // namespace drt::redesign
