// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Flat 3D node-id helpers for the SoA maze-search backend
// (grid_state_access topic). See plan.md §4.1 and §5 Phase 1.

#pragma once

#include <cstddef>
#include <cstdint>

#include "FlexMazeTypes.h"
#include "frBaseTypes.h"

namespace drt {

// 32-bit flat node index. Sufficient for grids up to ~4G nodes; typical
// drt worker grids are well under 100M nodes.
using MazeNodeId = std::uint32_t;
constexpr MazeNodeId kInvalidMazeNodeId = static_cast<MazeNodeId>(-1);

// Pure POD helper. Owns the dimensions of the grid being indexed.
// Does NOT own per-node arrays - those live in MazeSearchStateSoA.
class MazeNodeIndex
{
 public:
  MazeNodeIndex() = default;
  MazeNodeIndex(frMIdx xDim, frMIdx yDim, frMIdx zDim)
      : xDim_(xDim), yDim_(yDim), zDim_(zDim)
  {
  }

  // Flat id: (z * yDim + y) * xDim + x. No bounds checking.
  MazeNodeId getNodeId(frMIdx x, frMIdx y, frMIdx z) const
  {
    return static_cast<MazeNodeId>((z * yDim_ + y) * xDim_ + x);
  }
  MazeNodeId getNodeId(const FlexMazeIdx& mi) const
  {
    return getNodeId(mi.x(), mi.y(), mi.z());
  }

  // Inverse: split id back into 3D coords.
  void getXYZ(MazeNodeId id, frMIdx& x, frMIdx& y, frMIdx& z) const
  {
    const frMIdx layerStride = xDim_ * yDim_;
    z = static_cast<frMIdx>(id) / layerStride;
    const frMIdx within_layer
        = static_cast<frMIdx>(id) - z * layerStride;
    y = within_layer / xDim_;
    x = within_layer - y * xDim_;
  }
  FlexMazeIdx toMazeIdx(MazeNodeId id) const
  {
    frMIdx x = 0;
    frMIdx y = 0;
    frMIdx z = 0;
    getXYZ(id, x, y, z);
    return FlexMazeIdx(x, y, z);
  }

  // Neighbor lookup. Returns kInvalidMazeNodeId if the step would leave
  // the bounding box.
  MazeNodeId getNeighbor(MazeNodeId id, frDirEnum dir) const
  {
    frMIdx x = 0;
    frMIdx y = 0;
    frMIdx z = 0;
    getXYZ(id, x, y, z);
    switch (dir) {
      case frDirEnum::E:
        x += 1;
        break;
      case frDirEnum::W:
        x -= 1;
        break;
      case frDirEnum::N:
        y += 1;
        break;
      case frDirEnum::S:
        y -= 1;
        break;
      case frDirEnum::U:
        z += 1;
        break;
      case frDirEnum::D:
        z -= 1;
        break;
      default:
        return kInvalidMazeNodeId;
    }
    if (x < 0 || x >= xDim_ || y < 0 || y >= yDim_ || z < 0
        || z >= zDim_) {
      return kInvalidMazeNodeId;
    }
    return getNodeId(x, y, z);
  }

  frMIdx xDim() const { return xDim_; }
  frMIdx yDim() const { return yDim_; }
  frMIdx zDim() const { return zDim_; }
  std::size_t numNodes() const
  {
    return static_cast<std::size_t>(xDim_) * yDim_ * zDim_;
  }

 private:
  frMIdx xDim_ = 0;
  frMIdx yDim_ = 0;
  frMIdx zDim_ = 0;
};

}  // namespace drt
