// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.g — staged-batch MIS over real K-bias candidates.
//
// V2.6.f.10's cross-net check is greedy-per-net: when net B is about
// to commit its v1 variant, query worker_rq for any drConnFig from a
// different net intersecting v1's footprint; if any, downgrade B to
// v0. This catches conflicts but is one-sided — it can only downgrade
// the *later* net. It cannot reconsider net A which was committed
// earlier with v1.
//
// V2.6.g flips the protocol: at the V2.6.f.10 hook, drNet is always
// rebuilt with v0 (upstream-equivalent baseline), and the v1
// alternative is STASHED into a thread-local pending buffer instead
// of committed. At the end of FlexDRWorker::main(), V26gFlush below
// runs a greedy MIS over the worker's pending alternatives:
//
//   1. Build a pairwise conflict graph over alternatives. Edge =
//      footprints overlap on same layer (bbox shrunk 1 DBU to filter
//      edge-touch false positives, same as V2.6.f.10).
//   2. Compute priority per alternative: v0_drv - v1_drv (larger
//      DRV improvement = higher priority).
//   3. Greedy MIS: process priority-sorted; for each alternative,
//      skip if it conflicts with any already-selected alternative
//      or with the (updated) worker_rq baseline.
//   4. For each selected alternative: teardown drNet's v0 routes,
//      apply v1 clones, rebuild via subPathCost/addPathCost cycle
//      (the V2.6.f.5.c teardown/rebuild pattern).
//
// The MIS scope is WORKER-LOCAL by design. The cross-worker
// ProposalStaging singleton (V2.4.e/f) exists but adds OMP barrier
// overhead that this MVP avoids; cross-worker MIS is a follow-up.
//
// Gated by env: OPENROAD_DRT_REDESIGN_V26G_ENABLE=1. When unset,
// V2.6.f.10 behavior is unchanged (the buffer stays empty + flush is
// a no-op).

#pragma once

namespace drt {
class FlexDRWorker;
}

namespace dr_re {

// Called by FlexDRWorker::main() after route_queue() and before
// cleanup(). Drains the thread-local pending buffer of v1
// alternatives staged during this worker's routeNet calls, runs
// greedy MIS, and applies the selected subset by mutating drNet.
//
// No-op (and no overhead beyond an empty-vector check) when
// V2.6.g is not enabled OR when no alternatives were staged.
void V26gFlushPendingAlternatives(drt::FlexDRWorker* worker);

}  // namespace dr_re
