// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// F.1 Phase 2 — Continuous-flow router implementation.
//
// Status:
//   M1 WorkerPool                    — implemented (thin wrapper for now)
//   M2 RegionQueue                   — implemented (thread-safe priority queue)
//   M2 InFlightConflictIndex         — implemented (rtree-backed claim/release)
//   M3 F1Dispatcher                  — SKELETON; seeding + worker loop wired,
//                                       but per-region DRC scan + repair
//                                       region push is a TODO body.
//   M4 Boundary-cluster auto-queue   — not yet (TODO in scanAndPushRepairs).
//
// Activation: OPENROAD_DRT_F1_CONTINUOUS_QUEUE=1.

#include "ContinuousFlowF1.h"

#include <chrono>
#include <thread>

#include "FlexDR.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "global.h"  // RouterConfiguration
#include "utl/Logger.h"

namespace drt::f1 {

// ============================================================
// WorkerPool (M1)
// ============================================================

WorkerPool::WorkerPool(FlexDR* parent) : parent_(parent) {}

WorkerPool::~WorkerPool() = default;

std::unique_ptr<FlexDRWorker> WorkerPool::acquire(
    const odb::Rect& route_box,
    const FlexDR::SearchRepairArgs& args)
{
  acquired_.fetch_add(1);
  // M1: just call createWorker each time. Phase 2.1 will add a free
  // list + reconfigure fast-path. The pool interface is what M3 needs
  // — the perf win is incremental and can land later.
  return parent_->createWorker(0, 0, args, route_box);
}

void WorkerPool::returnToPool(std::unique_ptr<FlexDRWorker> worker)
{
  returned_.fetch_add(1);
  // M1: destruct. Phase 2.1: stash for reuse.
  worker.reset();
}

// ============================================================
// InFlightConflictIndex (M2)
// ============================================================

InFlightConflictIndex::BgBox InFlightConflictIndex::toBgBox(
    const odb::Rect& r)
{
  return BgBox(BgPoint(r.xMin(), r.yMin()),
               BgPoint(r.xMax(), r.yMax()));
}

bool InFlightConflictIndex::overlapsPositive(const odb::Rect& a,
                                              const odb::Rect& b)
{
  // Positive-area overlap (touching edges OK). Matches the F.1 v5
  // predicate that exposed the boundary-write race — that's the
  // intended Phase 2 predicate. Boundary races become deferred repair
  // regions via scanAndPushRepairs (M4 wiring).
  if (a.xMax() <= b.xMin() || b.xMax() <= a.xMin()) {
    return false;
  }
  if (a.yMax() <= b.yMin() || b.yMax() <= a.yMin()) {
    return false;
  }
  return true;
}

bool InFlightConflictIndex::tryClaim(int claim_id, const odb::Rect& route_box)
{
  std::lock_guard<std::mutex> g(mu_);
  // Query rtree for any candidate intersecting box; verify true positive-
  // area overlap (rtree uses inclusive box intersect; we want strict).
  const BgBox q = toBgBox(route_box);
  std::vector<BgValue> hits;
  rtree_.query(boost::geometry::index::intersects(q),
               std::back_inserter(hits));
  for (const auto& [other_box, other_id] : hits) {
    if (other_id == claim_id) {
      continue;  // own previous claim — shouldn't happen, defensive
    }
    auto it = claims_.find(other_id);
    if (it == claims_.end()) {
      continue;
    }
    if (overlapsPositive(route_box, it->second)) {
      return false;
    }
  }
  rtree_.insert(std::make_pair(q, claim_id));
  claims_.emplace(claim_id, route_box);
  return true;
}

void InFlightConflictIndex::release(int claim_id)
{
  std::lock_guard<std::mutex> g(mu_);
  auto it = claims_.find(claim_id);
  if (it == claims_.end()) {
    return;
  }
  rtree_.remove(std::make_pair(toBgBox(it->second), claim_id));
  claims_.erase(it);
}

int InFlightConflictIndex::size() const
{
  std::lock_guard<std::mutex> g(mu_);
  return static_cast<int>(claims_.size());
}

// ============================================================
// RegionQueue (M2)
// ============================================================

void RegionQueue::pushLocked(Region& r)
{
  r.seq = next_seq_.fetch_add(1);
  q_.push(std::move(r));
  pushed_.fetch_add(1);
}

void RegionQueue::push(Region&& r)
{
  {
    std::lock_guard<std::mutex> g(mu_);
    pushLocked(r);
  }
  cv_.notify_one();
}

std::optional<Region> RegionQueue::popBlocking()
{
  std::unique_lock<std::mutex> g(mu_);
  cv_.wait(g, [this] { return !q_.empty() || done_.load(); });
  if (q_.empty()) {
    return std::nullopt;
  }
  Region r = q_.top();
  q_.pop();
  popped_.fetch_add(1);
  return r;
}

void RegionQueue::markDone()
{
  done_.store(true);
  cv_.notify_all();
}

std::size_t RegionQueue::size() const
{
  std::lock_guard<std::mutex> g(mu_);
  return q_.size();
}

// ============================================================
// F1Dispatcher (M3 SKELETON)
// ============================================================

F1Dispatcher::F1Dispatcher(FlexDR* parent, const FlexDR::SearchRepairArgs& args)
    : parent_(parent), args_(args)
{
  pool_ = std::make_unique<WorkerPool>(parent_);
}

F1Dispatcher::~F1Dispatcher() = default;

void F1Dispatcher::seedInitialTiles()
{
  // Replicate the existing tiling: one region per (x, y) tile,
  // priority -1 (so any future marker-cluster region preempts).
  // Boxes are computed by FlexDR::createWorker semantics — we
  // delegate to createWorker via the WorkerPool acquire path at
  // dispatch time, so here we just enumerate (x_offset, y_offset)
  // pairs as placeholders. The actual routeBox is computed lazily.
  //
  // M3 SKELETON: emit one Region per tile carrying offset coords as
  // a workaround until we plumb route-box pre-computation.
  // M3 final: pre-compute every routeBox via the same logic as
  // createWorker so we can use it in the InFlightConflictIndex.
  auto* design = parent_->getDesign();
  auto gCellPatterns = design->getTopBlock()->getGCellPatterns();
  auto& xgp = gCellPatterns.at(0);
  auto& ygp = gCellPatterns.at(1);
  const int size = args_.size;
  const int offset = args_.offset;
  int count = 0;
  for (int i = offset; i < static_cast<int>(xgp.getCount()); i += size) {
    for (int j = offset; j < static_cast<int>(ygp.getCount()); j += size) {
      // Compute routeBox the same way createWorker does so the
      // InFlightConflictIndex query is accurate at dispatch time.
      odb::Rect rb1 = design->getTopBlock()->getGCellBox(odb::Point(i, j));
      const int max_i = std::min(static_cast<int>(xgp.getCount()) - 1,
                                  i + size - 1);
      const int max_j = std::min(static_cast<int>(ygp.getCount()),
                                  j + size - 1);
      odb::Rect rb2
          = design->getTopBlock()->getGCellBox(odb::Point(max_i, max_j));
      Region r;
      r.route_box
          = odb::Rect(rb1.xMin(), rb1.yMin(), rb2.xMax(), rb2.yMax());
      r.halo_dbu = parent_->getRouterCfg()->MTSAFEDIST;
      r.priority = -1;
      r.marker_count = 0;
      r.wide_halo_for_repair = false;
      queue_.push(std::move(r));
      ++count;
    }
  }
  parent_->getLogger()->report(
      "[F.1 cont] seeded {} initial tiles", count);
}

void F1Dispatcher::scanAndPushRepairs(int /*thread_id*/,
                                      const Region& /*region*/)
{
  // M3 SKELETON — body intentionally empty. M4 will:
  //
  //   1. Scan the just-committed region's routeBox + 1-GCell halo
  //      for new markers via getRegionQuery()->query(...) on each
  //      layer's marker rtree.
  //
  //   2. Cluster markers using CrossSeamRepair::buildRepairJobs
  //      (already exists and works on iter-0's 92k+ markers).
  //
  //   3. For each cluster decide:
  //        - touches region boundary edge → wide-halo CSR-style region
  //          (halo = DRCSAFEDIST + MTSAFEDIST + 4000, priority high)
  //        - interior → normal repair region
  //          (halo = DRCSAFEDIST, priority = marker count)
  //
  //   4. Push to queue_.
  //
  // The plumbing in M3 is already in place to call this from the
  // worker loop after end(); empty body just means "no new regions
  // queued", so the dispatcher will drain the initial tiles and
  // terminate — equivalent to iter-0-only routing. That's the M3
  // smoke gate: did it route all initial tiles correctly?
}

void F1Dispatcher::workerThreadMain(int thread_id)
{
  while (!shutdown_.load()) {
    auto region_opt = queue_.popBlocking();
    if (!region_opt) {
      return;  // queue done + empty
    }
    Region region = std::move(*region_opt);

    // Attempt to claim. If conflicts with in-flight, re-queue with
    // slightly-decremented priority (FIFO within same tier).
    if (!inflight_.tryClaim(thread_id, region.route_box)) {
      Region rq = region;
      rq.priority = std::max(-1000, rq.priority - 1);
      queue_.push(std::move(rq));
      // Brief back-off to avoid hot spin on contention.
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      continue;
    }

    in_flight_count_.fetch_add(1);
    auto worker = pool_->acquire(region.route_box, args_);
    const int rc = worker->main(parent_->getDesign());
    if (rc == 0
        && worker->getNumMarkers() < worker->getInitNumMarkers()) {
      worker->end(parent_->getDesign());
    }
    pool_->returnToPool(std::move(worker));
    inflight_.release(thread_id);
    regions_processed_.fetch_add(1);
    in_flight_count_.fetch_sub(1);

    // M4: scan + push repair regions.
    scanAndPushRepairs(thread_id, region);
  }
}

void F1Dispatcher::run()
{
  const int num_threads = parent_->getRouterCfg()->MAX_THREADS;
  parent_->getLogger()->report(
      "[F.1 cont] starting dispatcher with {} threads", num_threads);

  seedInitialTiles();

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([this, t] { workerThreadMain(t); });
  }

  // Termination: when queue is empty AND no workers in flight, no
  // more regions can be pushed (scanAndPushRepairs is the only push
  // path and it runs synchronously inside the worker loop).
  //
  // M3 SKELETON termination: poll every 100 ms; mark queue done when
  // both conditions hold. M4 will refine with a condition variable on
  // in-flight count drop.
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (queue_.size() == 0 && in_flight_count_.load() == 0) {
      queue_.markDone();
      break;
    }
  }

  for (auto& t : threads) {
    t.join();
  }

  parent_->getLogger()->report(
      "[F.1 cont] dispatcher done: processed {} regions "
      "(pushed {}, popped {})",
      regions_processed_.load(), queue_.pushed(), queue_.popped());
}

// ============================================================
// Public entry point
// ============================================================
void runContinuousFlowF1(FlexDR* parent, const FlexDR::SearchRepairArgs& args)
{
  F1Dispatcher dispatcher(parent, args);
  dispatcher.run();
}

}  // namespace drt::f1
