// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.d preflight — declared interface for the FlexConstraint→NormalizedRule
// translator. Real implementation lands in P2.2.d proper. The current .cpp
// is a stub that returns an empty deck (with zero-coverage), to keep the
// abstraction surface live without entangling the oracle with upstream.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "NormalizedRule.h"
#include "RuleDeck.h"

// Forward-declared upstream types. Including the real frConstraint.h /
// frLayer.h here would defeat the abstraction boundary.
namespace drt {
class frConstraint;
class frLayer;
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
  // Translate one upstream constraint.
  //
  // RETURN:
  //   nullopt — constraint is unrecognized (none of our 4 families).
  //             Caller should account for it via RuleDeck::AddUnsupported.
  //   set     — populated NormalizedRule with family / coverage / params
  //             derived from the frConstraint subclass + layer
  //             attribution per the rules below.
  //
  // LAYER ATTRIBUTION CONTRACT (P2.2.e.2.b.1):
  //   `discovered_layer` is the authoritative layer container through
  //   which the caller reached this constraint during traversal. When
  //   non-null:
  //     - layer_filter      <- discovered_layer->getLayerNum()
  //     - layer_knownness   <- Explicit
  //   When null:
  //     - if constraint->getLayer() is non-null, use that as a fallback
  //       (Explicit, with the object-stored layer's number).
  //     - else layer_filter <- nullopt; layer_knownness <- Unknown.
  //
  //   This ordering reflects the upstream reality that
  //   frConstraint::layer_ is set inconsistently for constraints stored
  //   in frLayer's per-family collections (setLayer is not always
  //   called by upstream parsers). The traversal-context layer is the
  //   defensible truth source; the object-layer accessor is fallback.
  //
  // CONFLICT DETECTION:
  //   When both discovered_layer and constraint->getLayer() are
  //   non-null AND disagree, we prefer discovered_layer per the
  //   contract above, but increment a static counter exposed via
  //   LayerConflictsSeen() so audit can flag the disagreement. A
  //   non-zero counter at end-of-process is a signal that traversal
  //   assumptions or upstream metadata may be inconsistent.
  //
  // The constraint's frConstraint subclass continues to determine rule
  // family + params; this method changes ONLY attribution.
  std::optional<NormalizedRule> TranslateOne(
      const drt::frConstraint* c,
      const drt::frLayer* discovered_layer = nullptr);

  // Bulk translate. Used when the caller has no traversal context
  // (object-stored layer is the only attribution source). Internally
  // dispatches to TranslateOne(c, nullptr) per element.
  RuleDeck Translate(const drt::frConstraint* const* upstream,
                     std::size_t upstream_count);

  // Process-wide diagnostic: number of times TranslateOne saw a
  // (discovered_layer, constraint->getLayer()) disagreement. Honest
  // reporting metric per the conflict-detection contract.
  static std::uint64_t LayerConflictsSeen();
};

}  // namespace drt::redesign::legality
