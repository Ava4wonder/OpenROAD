// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "MazeSearchStateSoA.h"

#include <algorithm>

namespace drt {

void MazeSearchStateSoA::resize(std::size_t n)
{
  g_cost_.assign(n, kInfCost);
  f_cost_.assign(n, kInfCost);
  state_.assign(n, static_cast<std::uint8_t>(MazeNodeState::Unseen));
  parent_dir_.assign(n, 0);
  last_dir_.assign(n, 0);
  vlength_x_.assign(n, 0);
  vlength_y_.assign(n, 0);
  tlength_.assign(n, 0);
  prev_via_up_.assign(n, 0);
  epoch_.assign(n, 0);
  current_ = 0;
}

void MazeSearchStateSoA::beginNewSearch()
{
  ++current_;
  if (current_ == 0) {
    // Epoch wrapped. Zero out so the next touch correctly fires.
    std::fill(epoch_.begin(), epoch_.end(), 0);
    current_ = 1;
  }
}

void MazeSearchStateSoA::touch(MazeNodeId id)
{
  if (epoch_[id] != current_) {
    epoch_[id] = current_;
    g_cost_[id] = kInfCost;
    f_cost_[id] = kInfCost;
    state_[id] = static_cast<std::uint8_t>(MazeNodeState::Unseen);
    parent_dir_[id] = 0;
    last_dir_[id] = 0;
    vlength_x_[id] = 0;
    vlength_y_[id] = 0;
    tlength_[id] = 0;
    prev_via_up_[id] = 0;
  }
}

}  // namespace drt
