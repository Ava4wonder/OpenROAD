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

#include <optional>

#include "LegalityVerdict.h"
#include "Score.h"

namespace drt::redesign {

struct EvalOptions
{
  // Compute Score even if LegalityVerdict::legal == false. Used for
  // search heuristics that learn from illegal proposals (e.g. to
  // measure how close a candidate is to legal). Such proposals are
  // marked speculative-only; they cannot be committed.
  bool score_even_if_illegal = false;
};

struct EvalOutcome
{
  LegalityVerdict legality;
  // Present iff legality.legal == true OR
  //         opts.score_even_if_illegal was set on the eval call.
  std::optional<Score> score;
};

}  // namespace drt::redesign
