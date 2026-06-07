// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// F.1 Phase 2 — Continuous-flow router with routeBox-only conflict +
// CSR-style boundary repair. See F1_architecture.md for the full design.
//
// PUBLIC SURFACE — three concerns:
//   1. WorkerPool      (M1) — persistent FlexDRWorkers, acquire/release
//   2. RegionQueue     (M2) — thread-safe priority queue of Regions
//   3. F1Dispatcher    (M3) — pops regions, claims via InFlightIndex,
//                             submits to workers, ingests new regions
//                             from per-worker DRC scans
//
// Activation: env OPENROAD_DRT_F1_CONTINUOUS_QUEUE=1 (distinct from
// Phase 1's OPENROAD_DRT_F1_CONTINUOUS which still selects the old
// bbox-MIS variant).

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/index/rtree.hpp>

#include "FlexDR.h"  // needed for FlexDR::SearchRepairArgs nested type
#include "frBaseTypes.h"
#include "odb/db.h"

namespace drt {

class FlexDRWorker;
class frDesign;
class RouterConfiguration;

namespace f1 {

// ============================================================
// Region — unit of work on the queue
// ============================================================
struct Region
{
  odb::Rect route_box;       // worker's write region
  // Halo applied at FlexDRWorker construction: extBox = routeBox +
  // halo_dbu. Default = MTSAFEDIST.
  frCoord halo_dbu;
  // Priority — higher = dispatched first. Marker repair regions have
  // priority ∝ marker count; initial-seed regions have priority -1.
  int priority;
  // Origin marker count (informational; used for priority + decay).
  int marker_count;
  // Wider halo flag — set on boundary-cluster repair regions so the
  // worker constructed for this region uses the CSR-style wide halo
  // even if the cluster geometry is small.
  bool wide_halo_for_repair;
  // Sequence number for deterministic tie-breaking.
  uint64_t seq;

  bool operator<(const Region& other) const noexcept
  {
    // priority_queue is max-heap on operator<; we want higher priority
    // popped first, with ties broken by lower seq (FIFO within tier).
    if (priority != other.priority) {
      return priority < other.priority;
    }
    return seq > other.seq;
  }
};

// ============================================================
// WorkerPool (M1) — persistent FlexDRWorker storage per thread
// ============================================================
//
// M1 strategy: maintain a pool of free FlexDRWorker objects. Acquire
// returns a worker pre-configured for the requested region; the pool
// internally calls FlexDR::createWorker for a fresh allocation, OR
// reuses + reconfigure() a previously-released worker (Phase 2.1 will
// add the reconfigure fast-path; M1 just builds the pool primitive).
class WorkerPool
{
 public:
  explicit WorkerPool(FlexDR* parent);
  ~WorkerPool();

  // Acquire a worker configured for `route_box` with `args`. Thread-safe.
  // Returns ownership of a unique_ptr; release via returnToPool when
  // done.
  std::unique_ptr<FlexDRWorker> acquire(const odb::Rect& route_box,
                                        const FlexDR::SearchRepairArgs& args);

  // Return a worker to the pool. Thread-safe. M1: just destructs.
  // Phase 2.1: stash for reconfigure-fast-path reuse.
  void returnToPool(std::unique_ptr<FlexDRWorker> worker);

  // Stats.
  int acquired() const { return acquired_.load(); }
  int returned() const { return returned_.load(); }

 private:
  FlexDR* parent_;
  std::atomic<int> acquired_{0};
  std::atomic<int> returned_{0};
  // Phase 2.1: std::mutex pool_mu_; std::vector<...> free_workers_;
};

// ============================================================
// InFlightConflictIndex (M2) — spatial index of in-flight routeBoxes
// ============================================================
//
// Dispatcher invariant: at any moment, all in-flight workers'
// routeBoxes are pairwise non-overlapping (touching is OK). The index
// is queried atomically with tryClaim/release.
class InFlightConflictIndex
{
 public:
  // Try to claim `route_box` for `claim_id`. Returns true if no
  // currently-claimed routeBox has positive-area overlap with
  // `route_box`. On success the box is registered; release with
  // release(claim_id).
  bool tryClaim(int claim_id, const odb::Rect& route_box);

  // Release a previously-claimed box. No-op if not present.
  void release(int claim_id);

  // Number of currently-in-flight claims.
  int size() const;

 private:
  using BgPoint = boost::geometry::model::point<
      frCoord,
      2,
      boost::geometry::cs::cartesian>;
  using BgBox = boost::geometry::model::box<BgPoint>;
  using BgValue = std::pair<BgBox, int>;
  using BgRtree = boost::geometry::index::rtree<
      BgValue,
      boost::geometry::index::quadratic<16>>;

  static BgBox toBgBox(const odb::Rect& r);
  static bool overlapsPositive(const odb::Rect& a, const odb::Rect& b);

  mutable std::mutex mu_;
  BgRtree rtree_;
  std::unordered_map<int, odb::Rect> claims_;
};

// ============================================================
// RegionQueue (M2) — thread-safe priority queue of Regions
// ============================================================
class RegionQueue
{
 public:
  // Push a region. Thread-safe. Wakes one waiting dispatcher.
  void push(Region&& r);

  // Pop the highest-priority region. Blocks if queue is empty and
  // !done(). Returns empty optional if done() and queue empty.
  std::optional<Region> popBlocking();

  // Signal that no more regions will ever be pushed. All waiters
  // wake; subsequent popBlocking returns empty optional.
  void markDone();

  // Statistics.
  int pushed() const { return pushed_.load(); }
  int popped() const { return popped_.load(); }
  std::size_t size() const;
  bool done() const { return done_.load(); }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<Region> q_;
  std::atomic<bool> done_{false};
  std::atomic<uint64_t> next_seq_{0};
  std::atomic<int> pushed_{0};
  std::atomic<int> popped_{0};

  // Internal: called under lock; assigns seq + pushes.
  void pushLocked(Region& r);
};

// ============================================================
// F1Dispatcher (M3 SKELETON) — top-level continuous-flow loop
// ============================================================
//
// M3 entry point. Owns a WorkerPool + RegionQueue +
// InFlightConflictIndex, runs N worker threads that loop:
//   pop region → claim → reconfigure-or-acquire → main → DRC →
//   end → push repair regions → release → return to pool
//
// SKELETON ONLY — termination + DRC marker emission left as
// // TODO comments. M4 wires the boundary-repair queue path.
class F1Dispatcher
{
 public:
  F1Dispatcher(FlexDR* parent,
               const FlexDR::SearchRepairArgs& args,
               FlexDR::IterationProgress& iter_prog);
  ~F1Dispatcher();

  // Run the continuous flow until termination. Blocks the calling
  // thread until done. Updates iter_prog at end so downstream
  // progress-report code sees consistent counters.
  void run();

  // Stats / debugging.
  int regionsProcessed() const { return regions_processed_.load(); }

 private:
  // Seed the queue with one Region per tile (replicates iter 0 layout).
  void seedInitialTiles();

  // Per-thread worker loop. Pops regions, dispatches, ingests repairs.
  void workerThreadMain(int thread_id);

  // After a worker commits, scan its routeBox + halo for markers and
  // push repair regions to the queue. M3 SKELETON: empty body.
  void scanAndPushRepairs(int thread_id, const Region& region);

  FlexDR* parent_;
  const FlexDR::SearchRepairArgs& args_;
  FlexDR::IterationProgress& iter_prog_;
  std::unique_ptr<WorkerPool> pool_;
  RegionQueue queue_;
  InFlightConflictIndex inflight_;
  std::atomic<int> regions_processed_{0};
  std::atomic<int> regions_pushed_repairs_{0};
  std::atomic<int> in_flight_count_{0};
  std::atomic<bool> shutdown_{false};
  // M4.2 design-DB R/W lock — main() takes shared (read) lock, end()
  // takes unique (write) lock. Prevents the SIGSEGV in initNetObjs
  // when worker A.main() reads frNet X while worker B.end() mutates
  // it. Phase 3 will replace with per-net versioning.
  std::shared_mutex design_mu_;
};

// ============================================================
// Public entry point — called from FlexDR::optimizationFlow when
// OPENROAD_DRT_F1_CONTINUOUS_QUEUE=1 is set.
// ============================================================
void runContinuousFlowF1(FlexDR* parent,
                         const FlexDR::SearchRepairArgs& args,
                         FlexDR::IterationProgress& iter_prog);

}  // namespace f1
}  // namespace drt
