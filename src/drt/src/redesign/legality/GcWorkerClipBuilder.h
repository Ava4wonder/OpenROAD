// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.b — Bridge from FlexGCWorker::Impl state to a ClipRecord.
// This is the only redesign/legality file allowed to depend on gcNet
// and frMarker upstream types — by the same logic as
// FlexConstraintTranslator.cpp being the only file allowed to depend on
// frConstraint.h. Everything downstream consumes ClipRecord.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ClipDump.h"
#include "frBaseTypes.h"
#include "odb/geom.h"

namespace drt {
class gcNet;
class frMarker;
}  // namespace drt

namespace drt::redesign::legality {

// Build a ClipRecord from worker state. The caller (inside
// FlexGCWorker::Impl::main) has access to these references through
// friend; passing by const-ref keeps this builder out of
// FlexGCWorker::Impl's friend list.
//
// Layer A scope for v1 of the hook:
//   candidates[] = ALL gcNet rectangles (fixed + routed) that intersect
//                  drcBox, on every layer in [minLayerNum, maxLayerNum].
//                  There is no native "candidate" concept in
//                  FlexGCWorker; v1 treats every shape as a candidate
//                  and lets P2.2.e.2/3 reinterpret if a finer split
//                  becomes useful.
//   context[]    = empty.
//
// Layer B (per candidate):
//   projected_marker_count    = #markers whose bbox intersects this
//                               candidate's bbox.
//   projected_legal           = (projected_marker_count == 0).
//   projection_ambiguous      = set on every candidate that shares a
//                               marker with another candidate.
//
// clip_box = extBox (read region); route_box = drcBox (action region).
ClipRecord BuildClipRecord(
    const std::vector<std::unique_ptr<drt::gcNet>>& nets,
    const std::vector<std::unique_ptr<drt::frMarker>>& markers,
    const odb::Rect& drc_box,
    const odb::Rect& ext_box,
    drt::frLayerNum min_layer_num,
    drt::frLayerNum max_layer_num,
    std::uint64_t clip_id,
    const std::string& design,
    const std::string& pdk);

}  // namespace drt::redesign::legality
