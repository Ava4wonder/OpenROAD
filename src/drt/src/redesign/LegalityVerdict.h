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

struct LegalityVerdict
{
  bool legal = true;
  std::vector<MarkerKind> violations;  // empty iff legal == true
  uint64_t marker_count_after = 0;
};

}  // namespace drt::redesign
