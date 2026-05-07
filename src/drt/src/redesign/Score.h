// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.b — soft scoring, separate from hard legality per
// v2_drt_redesign_plan.md §4.3.
//
// Score is used only after legality filtering, or for explicitly marked
// speculative/repair-mode proposals that are NOT directly
// commit-eligible (set EvalOptions::score_even_if_illegal). Score has
// no `legal` field, no DRC term, no marker-count field by design —
// hard legality lives in LegalityVerdict.
//
// `aggregate` is computed by ConflictPolicy with phase-dependent
// weights (early/middle/late from drt_redesign_plan.md §8). The
// individual delta_* fields are populated by the scorer; the policy
// applies the weighted reduction.

#pragma once

namespace drt::redesign {

struct Score
{
  int    delta_marker_reduction = 0;  // legal-marker reduction only
  // V2.2.c.proj — Manhattan-length proxy (sum of max(dx, dy) over
  // route-shape bboxes), NOT bbox perimeter. Documented as a proxy
  // because route-shape semantics are richer than rectangle bbox in
  // OpenROAD (tracks, widths, vias). Refined when V2.2.e wires a
  // real frPathSeg path-length accessor.
  double delta_wirelength_proxy = 0.0;
  int    delta_via_count = 0;
  double delta_congestion = 0.0;
  double delta_timing = 0.0;
  double delta_history_cost = 0.0;
  double aggregate = 0.0;
};

}  // namespace drt::redesign
