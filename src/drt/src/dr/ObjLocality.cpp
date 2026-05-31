// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P4.0 BoundaryDiag — implementation of ObjLocality classifier.
// See ObjLocality.h for the algorithm description.

#include "ObjLocality.h"

#include "db/drObj/drFig.h"

namespace drt {

ObjLocality classifyObj(const drConnFig* cf,
                        const odb::Rect& routeBox,
                        const odb::Rect& /* extBox */,
                        const odb::Rect& /* drcBox */,
                        const bool is_ext_owner)
{
  if (cf == nullptr) {
    return ObjLocality::FIXED_CONTEXT;
  }
  // Ext-owned shapes are EXT_CONTEXT regardless of geometry — they are the
  // worker's read-only view of a neighbor's route. Even if the ext bbox
  // happens to land inside routeBox (rare edge case post-merge), it is
  // still treated as ext context.
  if (is_ext_owner) {
    return ObjLocality::EXT_CONTEXT;
  }
  const odb::Rect bb = cf->getBBox();
  const bool strictly_inside = (bb.xMin() > routeBox.xMin())
                               && (bb.yMin() > routeBox.yMin())
                               && (bb.xMax() < routeBox.xMax())
                               && (bb.yMax() < routeBox.yMax());
  if (strictly_inside) {
    return ObjLocality::LOCAL_ROUTE;
  }
  // Local-owned but touches/crosses the routeBox edge.
  return ObjLocality::BOUNDARY_TOUCHING;
}

const char* objLocalityToken(const ObjLocality loc)
{
  switch (loc) {
    case ObjLocality::LOCAL_ROUTE:
      return "local";
    case ObjLocality::BOUNDARY_TOUCHING:
      return "boundary";
    case ObjLocality::EXT_CONTEXT:
      return "ext";
    case ObjLocality::FIXED_CONTEXT:
      return "fixed";
  }
  return "fixed";  // unreachable
}

}  // namespace drt
