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

F1Dispatcher::F1Dispatcher(FlexDR* parent,
                           const FlexDR::SearchRepairArgs& args,
                           FlexDR::IterationProgress& iter_prog)
    : parent_(parent), args_(args), iter_prog_(iter_prog)
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
                                      const Region& region)
{
  // M4 — after a worker commits, scan its routeBox + 1-GCell halo
  // for markers and push new Regions:
  //   - boundary cluster (touches edge of routeBox): wide CSR-style halo,
  //     high priority (marker_count × 10) — repairs the cross-worker
  //     boundary race that routeBox-only MIS deliberately allows
  //   - interior cluster: narrow halo, priority = marker count
  auto* topBlock = parent_->getDesign()->getTopBlock();
  auto* cfg = parent_->getRouterCfg();
  const frCoord kHaloPad = cfg->MTSAFEDIST;
  const frCoord kBoundaryTol = cfg->MTSAFEDIST;
  const frCoord kClusterT = 8000;  // ~2 GCells per cluster bucket

  // Scan markers whose bbox intersects region.route_box + kHaloPad.
  // Linear over all markers; M4.1 will replace with frRegionQuery for
  // designs > 50k markers where this dominates.
  const odb::Rect scan_box(region.route_box.xMin() - kHaloPad,
                           region.route_box.yMin() - kHaloPad,
                           region.route_box.xMax() + kHaloPad,
                           region.route_box.yMax() + kHaloPad);

  std::map<std::tuple<frLayerNum, int, int>, std::vector<odb::Rect>> buckets;
  for (const auto& mu : topBlock->getMarkers()) {
    const odb::Rect bb = mu->getBBox();
    if (!scan_box.intersects(bb)) {
      continue;
    }
    const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
    const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
    const auto key
        = std::make_tuple(mu->getLayerNum(),
                          static_cast<int>(cx / kClusterT),
                          static_cast<int>(cy / kClusterT));
    buckets[key].push_back(bb);
  }
  if (buckets.empty()) {
    return;
  }

  const frCoord wide_halo
      = cfg->DRCSAFEDIST + cfg->MTSAFEDIST + 4000;
  const frCoord narrow_halo = cfg->DRCSAFEDIST;

  int pushed_here = 0;
  for (auto& [key, group] : buckets) {
    odb::Rect cluster_bb = group.front();
    for (const auto& bb : group) {
      cluster_bb.merge(bb);
    }
    const bool touches_boundary
        = cluster_bb.xMin() <= region.route_box.xMin() + kBoundaryTol
          || cluster_bb.xMax() >= region.route_box.xMax() - kBoundaryTol
          || cluster_bb.yMin() <= region.route_box.yMin() + kBoundaryTol
          || cluster_bb.yMax() >= region.route_box.yMax() - kBoundaryTol;
    const frCoord halo = touches_boundary ? wide_halo : narrow_halo;
    Region r;
    r.route_box = odb::Rect(cluster_bb.xMin() - halo,
                            cluster_bb.yMin() - halo,
                            cluster_bb.xMax() + halo,
                            cluster_bb.yMax() + halo);
    r.halo_dbu = halo;
    r.marker_count = static_cast<int>(group.size());
    r.priority = r.marker_count * (touches_boundary ? 10 : 1);
    r.wide_halo_for_repair = touches_boundary;
    queue_.push(std::move(r));
    ++pushed_here;
  }
  regions_pushed_repairs_.fetch_add(pushed_here);
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

    // M4 — scan + push BEFORE decrementing in_flight, so the
    // termination check can't false-trigger between "this region is
    // done" and "the repair regions I want to push are visible to the
    // queue". Termination is: queue.size()==0 AND in_flight_count==0.
    scanAndPushRepairs(thread_id, region);
    in_flight_count_.fetch_sub(1);
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

  // iter_prog must be populated so downstream progress / div-by-zero
  // code in searchRepair sees consistent counters. We model the
  // entire F.1 continuous-flow phase as one logical iter where
  // total = cnt = processed regions.
  const int processed = regions_processed_.load();
  iter_prog_.total_num_workers = std::max(1, processed);
  iter_prog_.cnt_done_workers = std::max(1, processed);

  parent_->getLogger()->report(
      "[F.1 cont] dispatcher done: processed {} regions "
      "(initial-tile + repair pushes: {}; popped {})",
      processed, queue_.pushed(), queue_.popped());
  parent_->getLogger()->report(
      "[F.1 cont] M4 repair regions pushed: {}",
      regions_pushed_repairs_.load());
}

// ============================================================
// Public entry point
// ============================================================
void runContinuousFlowF1(FlexDR* parent,
                         const FlexDR::SearchRepairArgs& args,
                         FlexDR::IterationProgress& iter_prog)
{
  F1Dispatcher dispatcher(parent, args, iter_prog);
  dispatcher.run();
}

}  // namespace drt::f1
