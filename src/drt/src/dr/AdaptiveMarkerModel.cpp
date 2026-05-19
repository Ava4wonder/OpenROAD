// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 1 — disabled-shell implementation.
//
// All public methods are present but do no work — they return identity
// values appropriate for the no-op path. This lets FlexDR own a
// std::unique_ptr<AdaptiveMarkerModel> field unconditionally without
// any behaviour change when the feature is disabled (the unique_ptr is
// never constructed in the disabled path).
//
// Patch 2 fills in observeGlobalMarkers + decayHeat + updateHotspots +
// CSV logging.
// Patch 3 fills in getWorkerPolicy's real scalar multipliers.

#include "dr/AdaptiveMarkerModel.h"

#include "db/obj/frMarker.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "utl/Logger.h"

namespace drt {

AdaptiveMarkerModel::AdaptiveMarkerModel(const Options& options,
                                         frDesign* design,
                                         utl::Logger* logger)
    : options_(options), design_(design), logger_(logger)
{
  // Patch 1: no-op constructor. Patch 2 reserves rule_layer_heat_ to
  // its full capacity (kNumAdaptiveRuleClasses * num_layers * tiles)
  // once tile dimensions are known.
}

AdaptiveMarkerModel::~AdaptiveMarkerModel() = default;

void AdaptiveMarkerModel::beginOuterIter(int iter)
{
  iter_ = iter;
  // Patch 2: decayHeat() here.
}

void AdaptiveMarkerModel::endOuterIter()
{
  // Patch 2: updateHotspots() + writeCsvRow() here.
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::vector<std::unique_ptr<frMarker>>& /*markers*/)
{
  // Patch 2: classify + extract nets + accumulate heat.
}

void AdaptiveMarkerModel::observeGlobalMarkers(
    const std::vector<frMarker>& /*markers*/)
{
  // Patch 2: same logic, by-value overload for callers that hold
  // markers by value rather than unique_ptr.
}

void AdaptiveMarkerModel::observeWorkerStats(
    const AdaptiveWorkerStats& /*stats*/)
{
  // Patch 2: merge rule_counts into a per-iter aggregate; flag congested
  // workers; update region heat from the worker's drc_box footprint.
}

AdaptiveWorkerPolicy AdaptiveMarkerModel::getWorkerPolicy(
    const odb::Rect& /*route_box*/,
    const odb::Rect& /*drc_box*/,
    int /*iter*/,
    frUInt4 /*base_drc_cost*/,
    frUInt4 /*base_marker_cost*/,
    frUInt4 /*base_fixed_shape_cost*/,
    float /*base_decay*/) const
{
  // Patch 1: identity policy. The default-constructed policy has
  // enabled=false + all multipliers=1.0, so any worker that receives
  // it behaves identically to the disabled-model case.
  AdaptiveWorkerPolicy policy;
  policy.enabled = options_.enabled;
  return policy;
}

int AdaptiveMarkerModel::getMarkerIncrement(const frMarker& /*marker*/,
                                            bool /*is_via*/,
                                            frLayerNum /*layer*/) const
{
  // Patch 1: identity. FlexGridGraph's existing addMarkerCostPlanar /
  // addMarkerCostVia use a hardcoded +10 internally, so returning 10
  // here matches that contract for callers that route through this
  // helper.
  return 10;
}

int AdaptiveMarkerModel::getNetPriority(frNet* /*net*/) const
{
  return 0;
}

bool AdaptiveMarkerModel::shouldIncreaseClipSize(
    const odb::Rect& /*region*/) const
{
  return false;
}

// --- private ---

void AdaptiveMarkerModel::decayHeat()
{
  // Patch 2.
}

void AdaptiveMarkerModel::addMarkerObservation(
    const AdaptiveMarkerObs& /*obs*/)
{
  // Patch 2.
}

void AdaptiveMarkerModel::updateHotspots()
{
  // Patch 2.
}

}  // namespace drt
