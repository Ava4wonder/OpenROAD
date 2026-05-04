// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight — declared interface for the FlexConstraint→NormalizedRule
// translator. Real implementation lands in P2.2.d proper. The current .cpp
// is a stub that returns an empty deck (with zero-coverage), to keep the
// abstraction surface live without entangling the oracle with upstream.

#pragma once

#include <cstddef>

#include "RuleDeck.h"

// Forward-declared upstream type. Including the real frConstraint.h here
// would defeat the abstraction boundary.
namespace drt {
class frConstraint;
}  // namespace drt

namespace drt::redesign::legality {

// IN-SCOPE SUBSET FOR FIRST INTEGRATION (P2.2.d). Anything outside this
// list is marked Fallback (recognized rule shape, params out of scope) or
// Unsupported (rule shape unrecognized) in the returned RuleDeck.
//
//   MetalShort
//     Supported: same-layer different-net AABB overlap with no
//                same-net exception, no cut-aware overlap, no width-
//                conditional behavior.
//     Fallback:  any constraint with NDR/SDP/cut-stack interactions.
//
//   PrlSpacing
//     Supported: single (min_spacing, prl_threshold) pair per metal
//                layer.
//     Fallback:  multi-row spacing tables, width-conditional spacing,
//                run-length conditional spacing.
//
//   EolSpacing
//     Supported: single (eol_width_threshold, eol_spacing, eol_within)
//                tuple per metal layer.
//     Fallback:  cut-aware EOL, EOL with parallel-edge condition,
//                EOL with notch condition.
//
//   CutSpacing
//     Supported: single min_spacing per cut layer (L-infinity edge-to-
//                edge), no cut-class interaction.
//     Fallback:  cut-class tables, layer-stack interactions, parallel
//                overlap exceptions.

class FlexConstraintTranslator
{
 public:
  // Translate the supplied upstream constraint pointers. The translator
  // never modifies upstream state. The returned RuleDeck owns its
  // NormalizedRule storage; param pointers handed to the oracle remain
  // valid until the next call into this RuleDeck.
  //
  // P2.2.d preflight stub: this implementation returns an empty deck,
  // marks every input as Unsupported (so coverage accounting still
  // reflects the input population), and never inspects the constraint
  // pointers. The real wiring lands in P2.2.d proper.
  RuleDeck Translate(const drt::frConstraint* const* upstream,
                     std::size_t upstream_count);
};

}  // namespace drt::redesign::legality
