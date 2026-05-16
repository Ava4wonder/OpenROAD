// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Structure-of-Arrays per-node state for the maze search.
// See plan.md §4.1, §4.2 and §5 Phase 2.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "MazeNodeIndex.h"
#include "frBaseTypes.h"

namespace drt {

enum class MazeNodeState : std::uint8_t
{
  Unseen = 0,
  Open = 1,
  Closed = 2,
};

// Allocated once per FlexGridGraph; resized when grid dims change. Per
// search, callers invoke beginNewSearch() to bump the epoch; any node
// whose stored epoch is stale is logically Unseen with g/f = INF.
class MazeSearchStateSoA
{
 public:
  static constexpr frCost kInfCost = std::numeric_limits<frCost>::max();

  MazeSearchStateSoA() = default;

  void resize(std::size_t n);
  std::size_t size() const { return state_.size(); }
  bool empty() const { return state_.empty(); }

  // Bump epoch; next touch() of any node will re-initialize it.
  void beginNewSearch();
  std::uint32_t currentEpoch() const { return current_; }

  // Lazy "touch": idempotent. Materializes default values if stale.
  // Must be called before reading any field if the caller is not sure
  // the node has been touched this epoch.
  void touch(MazeNodeId id);

  // Returns true iff node has been touched this epoch.
  bool isLive(MazeNodeId id) const { return epoch_[id] == current_; }

  // Field getters. Callers must touch() first if id may be stale.
  frCost g(MazeNodeId id) const { return g_cost_[id]; }
  frCost f(MazeNodeId id) const { return f_cost_[id]; }
  MazeNodeState state(MazeNodeId id) const
  {
    return static_cast<MazeNodeState>(state_[id]);
  }
  std::uint8_t parentDir(MazeNodeId id) const { return parent_dir_[id]; }
  std::uint8_t lastDir(MazeNodeId id) const { return last_dir_[id]; }
  frCoord vlengthX(MazeNodeId id) const { return vlength_x_[id]; }
  frCoord vlengthY(MazeNodeId id) const { return vlength_y_[id]; }
  frCoord tlength(MazeNodeId id) const { return tlength_[id]; }
  std::uint8_t prevViaUp(MazeNodeId id) const { return prev_via_up_[id]; }

  // Field setters.
  void setG(MazeNodeId id, frCost v) { g_cost_[id] = v; }
  void setF(MazeNodeId id, frCost v) { f_cost_[id] = v; }
  void setState(MazeNodeId id, MazeNodeState s)
  {
    state_[id] = static_cast<std::uint8_t>(s);
  }
  void setParentDir(MazeNodeId id, std::uint8_t d) { parent_dir_[id] = d; }
  void setLastDir(MazeNodeId id, std::uint8_t d) { last_dir_[id] = d; }
  void setVlengthX(MazeNodeId id, frCoord v) { vlength_x_[id] = v; }
  void setVlengthY(MazeNodeId id, frCoord v) { vlength_y_[id] = v; }
  void setTlength(MazeNodeId id, frCoord v) { tlength_[id] = v; }
  void setPrevViaUp(MazeNodeId id, std::uint8_t v) { prev_via_up_[id] = v; }

 private:
  std::vector<frCost> g_cost_;
  std::vector<frCost> f_cost_;
  std::vector<std::uint8_t> state_;
  std::vector<std::uint8_t> parent_dir_;
  std::vector<std::uint8_t> last_dir_;
  std::vector<frCoord> vlength_x_;
  std::vector<frCoord> vlength_y_;
  std::vector<frCoord> tlength_;
  std::vector<std::uint8_t> prev_via_up_;
  std::vector<std::uint32_t> epoch_;
  std::uint32_t current_ = 0;
};

}  // namespace drt
