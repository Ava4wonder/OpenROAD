// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// TS.2.a — per-worker seam-crossing ledger (cross-worker contract
// foundation; THREAD_SCALING_PLAN.md).
//
// FlexDRWorker::initNet_boundary() already computes, for every net whose
// route crosses the worker's routeBox edge, the exact crossing points
// (Point, layerNum) — the `extBounds` map — and then flattens them into
// anonymous drPins, discarding the net/seam identity. This class captures
// that identity as queryable runtime state, populated unconditionally in
// the same loop (cheap: one push_back + map bump per crossing, mirroring
// the BoundaryDiagStats "no env branching in the hot loop" pattern).
//
// Consumers: TS.2.b per-iter boundary re-merge (crossing alignment check
// between adjacent workers) and TS.2.c portal assignment (seam crossing
// slots). Until then the only reader is the env-gated validation dump in
// FlexDR::endWorkersBatch() (OPENROAD_DRT_SEAM_STATE_DIR), whose gate is
// exact agreement with BoundaryDiagStats::num_boundary_pins /
// num_boundary_nets on every worker.

#pragma once

#include <map>
#include <vector>

#include "frBaseTypes.h"
#include "odb/geom.h"

namespace drt {

class frNet;

struct SeamCrossing
{
  odb::Point pt;            // on the routeBox edge
  frLayerNum layer = 0;
  frNet* net = nullptr;
  int side = -1;            // 0=W 1=E 2=S 3=N, -1 = corner/unresolved
};

class WorkerSeamState
{
 public:
  void clear()
  {
    crossings_.clear();
    net_counts_.clear();
    num_subnets_ = 0;
  }
  // One boundary-crossing drNet fragment (a net split into k disjoint
  // pieces inside a worker calls this k times). Mirrors
  // BoundaryDiagStats::num_boundary_nets, which counts fragments.
  void noteSubnet() { ++num_subnets_; }
  int numSubnets() const { return num_subnets_; }
  void addCrossing(const odb::Point& pt,
                   frLayerNum layer,
                   frNet* net,
                   const odb::Rect& route_box)
  {
    SeamCrossing c;
    c.pt = pt;
    c.layer = layer;
    c.net = net;
    if (pt.x() == route_box.xMin()) {
      c.side = 0;
    } else if (pt.x() == route_box.xMax()) {
      c.side = 1;
    } else if (pt.y() == route_box.yMin()) {
      c.side = 2;
    } else if (pt.y() == route_box.yMax()) {
      c.side = 3;
    }
    crossings_.push_back(c);
    net_counts_[net] += 1;
  }
  const std::vector<SeamCrossing>& crossings() const { return crossings_; }
  int numCrossings() const { return static_cast<int>(crossings_.size()); }
  int numNets() const { return static_cast<int>(net_counts_.size()); }
  const std::map<frNet*, int>& netCounts() const { return net_counts_; }

 private:
  std::vector<SeamCrossing> crossings_;
  std::map<frNet*, int> net_counts_;
  int num_subnets_ = 0;
};

}  // namespace drt
