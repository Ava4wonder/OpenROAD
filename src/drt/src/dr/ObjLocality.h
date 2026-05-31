// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P4.0 BoundaryDiag — cross-cutting ObjLocality classification.
//
// classifyObj() partitions a drConnFig (queried during the per-marker region
// query inside FlexDRWorker::main()) into one of four locality classes
// against the worker's (routeBox, extBox, drcBox) triplet. The per-marker
// CSV emits per-(type, locality) shape counts; the locality is also a token
// in the canonical `nearby_pair_type` string.
//
// Default-off: this header is included only by code paths that are
// themselves gated by OPENROAD_DRT_BOUNDARY_DIAG_DIR. With the env var
// unset, classifyObj() is never called and contributes zero overhead.

#pragma once

#include "odb/geom.h"

namespace drt {

class drConnFig;

enum class ObjLocality
{
  LOCAL_ROUTE,        // drNet::routeConnFigs, bbox strictly inside routeBox
  BOUNDARY_TOUCHING,  // local-owned shape that crosses/touches routeBox edge
  EXT_CONTEXT,        // drNet::extConnFigs (read-only neighbor route)
  FIXED_CONTEXT,      // pin / blockage / null cf
};

// Classify a drConnFig against the worker's three nested boxes. The caller
// supplies is_ext_owner: true iff `cf` is in its parent drNet's
// extConnFigs vector (BoundaryDiag computes this once per encountered net
// during the per-marker region query and caches it).
//
// Algorithm:
//   1. cf == nullptr  -> FIXED_CONTEXT
//   2. is_ext_owner   -> EXT_CONTEXT (regardless of geometry)
//   3. Strictly inside routeBox (all 4 corners > / <) -> LOCAL_ROUTE
//   4. Otherwise (local-owned but on/over the edge) -> BOUNDARY_TOUCHING
//
// `extBox` and `drcBox` are reserved for future use (e.g. P4.2 repair
// decisions that distinguish ext_inside_drcBox vs outside).
ObjLocality classifyObj(const drConnFig* cf,
                        const odb::Rect& routeBox,
                        const odb::Rect& extBox,
                        const odb::Rect& drcBox,
                        bool is_ext_owner);

// String token used in the per-marker CSV's nearby_pair_type column.
// Returns one of "local" / "boundary" / "ext" / "fixed".
const char* objLocalityToken(ObjLocality loc);

}  // namespace drt
