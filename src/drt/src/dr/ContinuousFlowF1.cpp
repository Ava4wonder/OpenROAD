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
#include <unordered_set>

#include "FlexDR.h"
#include "db/obj/frNet.h"
#include "db/obj/frShape.h"
#include "db/obj/frVia.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "frRegionQuery.h"
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
// NetLockTable (M6)
// ============================================================

void NetLockTable::initialise(frDesign* design)
{
  auto* topBlock = design->getTopBlock();
  for (auto& net : topBlock->getNets()) {
    net_locks_[net.get()] = std::make_unique<std::mutex>();
  }
}

std::vector<frNet*> NetLockTable::netsInRegion(
    frDesign* design,
    const odb::Rect& region) const
{
  std::vector<frBlockObject*> objs;
  design->getRegionQuery()->queryDRObj(region, objs);
  std::unordered_set<frNet*> seen;
  for (auto* obj : objs) {
    frNet* net = nullptr;
    if (auto* p = dynamic_cast<frPathSeg*>(obj)) {
      net = p->getNet();
    } else if (auto* v = dynamic_cast<frVia*>(obj)) {
      net = v->getNet();
    } else if (auto* w = dynamic_cast<frPatchWire*>(obj)) {
      net = w->getNet();
    }
    if (net != nullptr) {
      seen.insert(net);
    }
  }
  std::vector<frNet*> sorted(seen.begin(), seen.end());
  // Sort by pointer address for deadlock-free acquisition.
  std::sort(sorted.begin(), sorted.end());
  return sorted;
}

std::vector<std::mutex*> NetLockTable::acquireNets(
    const std::vector<frNet*>& sorted)
{
  std::vector<std::mutex*> held;
  held.reserve(sorted.size());
  for (frNet* net : sorted) {
    auto it = net_locks_.find(net);
    if (it == net_locks_.end()) {
      continue;  // net not registered (created post-init); skip
    }
    it->second->lock();
    held.push_back(it->second.get());
  }
  return held;
}

void NetLockTable::releaseNets(const std::vector<std::mutex*>& held)
{
  // Release in reverse acquisition order.
  for (auto it = held.rbegin(); it != held.rend(); ++it) {
    (*it)->unlock();
  }
}

// ============================================================
// SpatialLockGrid (M5.0)
// ============================================================

SpatialLockGrid::SpatialLockGrid(const odb::Rect& die_area,
                                  int cells_per_axis)
    : die_(die_area), n_(cells_per_axis)
{
  if (n_ < 1) {
    n_ = 1;
  }
  cell_w_ = std::max<frCoord>(1, (die_.xMax() - die_.xMin()) / n_);
  cell_h_ = std::max<frCoord>(1, (die_.yMax() - die_.yMin()) / n_);
  locks_.reserve(n_ * n_);
  for (int i = 0; i < n_ * n_; ++i) {
    locks_.emplace_back(std::make_unique<std::mutex>());
  }
}

std::vector<int> SpatialLockGrid::cellsCovering(
    const odb::Rect& region) const
{
  // Clamp to die; cells outside the design need no lock.
  const frCoord rx_lo = std::max(region.xMin(), die_.xMin());
  const frCoord ry_lo = std::max(region.yMin(), die_.yMin());
  const frCoord rx_hi = std::min(region.xMax(), die_.xMax());
  const frCoord ry_hi = std::min(region.yMax(), die_.yMax());
  if (rx_hi <= rx_lo || ry_hi <= ry_lo) {
    return {};
  }
  const int x_lo = std::max(0, (int) ((rx_lo - die_.xMin()) / cell_w_));
  const int y_lo = std::max(0, (int) ((ry_lo - die_.yMin()) / cell_h_));
  const int x_hi
      = std::min(n_ - 1, (int) ((rx_hi - die_.xMin() - 1) / cell_w_));
  const int y_hi
      = std::min(n_ - 1, (int) ((ry_hi - die_.yMin() - 1) / cell_h_));
  std::vector<int> indices;
  indices.reserve((x_hi - x_lo + 1) * (y_hi - y_lo + 1));
  for (int y = y_lo; y <= y_hi; ++y) {
    for (int x = x_lo; x <= x_hi; ++x) {
      indices.push_back(y * n_ + x);
    }
  }
  return indices;
}

std::vector<int> SpatialLockGrid::acquireRegion(const odb::Rect& region)
{
  auto indices = cellsCovering(region);
  std::sort(indices.begin(), indices.end());
  for (int idx : indices) {
    locks_[idx]->lock();
  }
  return indices;
}

void SpatialLockGrid::releaseRegion(const std::vector<int>& held)
{
  // Release in reverse order (mirror of acquire).
  for (auto it = held.rbegin(); it != held.rend(); ++it) {
    locks_[*it]->unlock();
  }
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
  // M5.0 — kept for diagnostic comparison; unused on the M6 path.
  const odb::Rect die = parent_->getDesign()->getTopBlock()->getBBox();
  lock_grid_ = std::make_unique<SpatialLockGrid>(die, 32);
  // M6 — per-net mutex table. One mutex per design net, registered
  // once at dispatcher startup. Worker enumeration via queryDRObj +
  // dynamic_cast to frShape / frVia variants.
  net_locks_ = std::make_unique<NetLockTable>();
  net_locks_->initialise(parent_->getDesign());
  parent_->getLogger()->report(
      "[F.1 cont] M6 NetLockTable registered {} nets",
      net_locks_->numNetsRegistered());
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

  // M4.1: snap repair-region routeBoxes to the GCell grid. createWorker
  // accepts arbitrary boxes but pin-access initialization requires the
  // box to coincide with whole GCells (DRT-1231 fires otherwise). Use
  // the same getGCellBox(idx) pattern as seedInitialTiles.
  auto gCellPatterns = topBlock->getGCellPatterns();
  const int xgp_cnt = static_cast<int>(gCellPatterns.at(0).getCount());
  const int ygp_cnt = static_cast<int>(gCellPatterns.at(1).getCount());

  // Halo expressed in GCells rather than DBU — guarantees the snapped
  // routeBox is a whole-GCell-multiple super-region of the cluster.
  const int wide_halo_gcells = 2;    // ~CSR-style wide halo
  const int narrow_halo_gcells = 1;  // tight interior halo

  // M7 throttling — heartbeat trace shows queue avalanche
  // (~4 repair regions pushed per region processed → unbounded
  // growth). Two throttles applied here:
  //   (a) MIN_MARKERS_PER_CLUSTER — drop low-impact clusters that
  //       waste worker dispatches; isolated single-marker clusters
  //       almost never benefit from a wide-halo repair.
  //   (b) MAX_REPAIRS_PER_REGION — cap how many new regions a single
  //       parent worker can spawn. Caller can still re-emit on next
  //       iter; this just prevents one source from saturating queue.
  // M7.1 aggressive throttle — M7 (3, 4) still avalanche-grew on
  // ibex 8t (queue 360→600 in 6min). Tighten to (10, 1) so each
  // region pushes at most ONE repair, only when the cluster is
  // dense. Expected outcome: queue strictly shrinks; F.1 actually
  // terminates. Trade-off: misses small repair opportunities, will
  // converge to more residual markers than UNSET's iter loop.
  constexpr int kMinMarkers = 10;
  constexpr int kMaxRepairsPerRegion = 1;
  // Sort buckets by marker count desc so when we hit the cap we keep
  // the highest-impact clusters.
  std::vector<std::pair<std::tuple<frLayerNum, int, int>,
                         std::vector<odb::Rect>>>
      sorted_buckets(buckets.begin(), buckets.end());
  std::sort(sorted_buckets.begin(), sorted_buckets.end(),
            [](const auto& a, const auto& b) {
              return a.second.size() > b.second.size();
            });
  int pushed_here = 0;
  for (auto& [key, group] : sorted_buckets) {
    if (pushed_here >= kMaxRepairsPerRegion) {
      break;
    }
    if (static_cast<int>(group.size()) < kMinMarkers) {
      continue;
    }
    odb::Rect cluster_bb = group.front();
    for (const auto& bb : group) {
      cluster_bb.merge(bb);
    }
    const bool touches_boundary
        = cluster_bb.xMin() <= region.route_box.xMin() + kBoundaryTol
          || cluster_bb.xMax() >= region.route_box.xMax() - kBoundaryTol
          || cluster_bb.yMin() <= region.route_box.yMin() + kBoundaryTol
          || cluster_bb.yMax() >= region.route_box.yMax() - kBoundaryTol;
    const int halo_gcells
        = touches_boundary ? wide_halo_gcells : narrow_halo_gcells;

    // Snap cluster_bb corners to GCell indices, then expand by halo.
    const odb::Point lo_idx = topBlock->getGCellIdx(
        odb::Point(cluster_bb.xMin(), cluster_bb.yMin()));
    const odb::Point hi_idx = topBlock->getGCellIdx(
        odb::Point(cluster_bb.xMax(), cluster_bb.yMax()));
    const int i_lo = std::max(0, lo_idx.x() - halo_gcells);
    const int j_lo = std::max(0, lo_idx.y() - halo_gcells);
    const int i_hi = std::min(xgp_cnt - 1, hi_idx.x() + halo_gcells);
    const int j_hi = std::min(ygp_cnt - 1, hi_idx.y() + halo_gcells);
    const odb::Rect rb_lo
        = topBlock->getGCellBox(odb::Point(i_lo, j_lo));
    const odb::Rect rb_hi
        = topBlock->getGCellBox(odb::Point(i_hi, j_hi));

    Region r;
    r.route_box = odb::Rect(rb_lo.xMin(),
                            rb_lo.yMin(),
                            rb_hi.xMax(),
                            rb_hi.yMax());
    r.halo_dbu = cfg->DRCSAFEDIST;  // worker's own extBox margin
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

    // M7.2 — bypass InFlightConflictIndex routeBox MIS. Per-net locks
    // (acquired below) are now the correctness primitive; routeBox
    // overlap by itself is safe as long as the touched nets don't
    // race. The bbox claim was diagnosed as the source of the
    // 4232-popped-vs-311-processed spin loop in M7.1.
    in_flight_count_.fetch_add(1);
    auto worker = pool_->acquire(region.route_box, args_);
    // M6 — per-net mutex acquisition. Enumerate nets via queryDRObj
    // over the worker's READ region (routeBox + extBox + DRCSAFEDIST
    // margin). Acquire sorted by frNet pointer for deadlock safety.
    // Hold across main() AND end() so reads cannot race against
    // another worker's concurrent end() commit on the same net.
    auto* cfg = parent_->getRouterCfg();
    const odb::Rect read_region(
        region.route_box.xMin() - cfg->MTSAFEDIST - cfg->DRCSAFEDIST,
        region.route_box.yMin() - cfg->MTSAFEDIST - cfg->DRCSAFEDIST,
        region.route_box.xMax() + cfg->MTSAFEDIST + cfg->DRCSAFEDIST,
        region.route_box.yMax() + cfg->MTSAFEDIST + cfg->DRCSAFEDIST);
    const auto nets
        = net_locks_->netsInRegion(parent_->getDesign(), read_region);
    auto held = net_locks_->acquireNets(nets);
    int rc = 0;
    bool commit = false;
    rc = worker->main(parent_->getDesign());
    commit = (rc == 0
              && worker->getNumMarkers() < worker->getInitNumMarkers());
    if (commit) {
      worker->end(parent_->getDesign());
    }
    net_locks_->releaseNets(held);
    pool_->returnToPool(std::move(worker));
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

  // M7 DIAGNOSTIC — heartbeat every 10s so we can SEE the dispatcher
  // state during the 25-min hangs. If queue grows unbounded:
  // repair-avalanche. If queue is small but in_flight=0: deadlock.
  // If processed is steady but slow: lock contention.
  int hb_ticks = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    ++hb_ticks;
    const int proc = regions_processed_.load();
    const int rep = regions_pushed_repairs_.load();
    const int inflt = in_flight_count_.load();
    const int qsz = static_cast<int>(queue_.size());
    parent_->getLogger()->report(
        "[F.1 cont HB] t={}s processed={} repair_pushes={} "
        "queue={} in_flight={}",
        hb_ticks * 10, proc, rep, qsz, inflt);
    if (qsz == 0 && inflt == 0) {
      queue_.markDone();
      break;
    }
    if (hb_ticks > 60) {  // 10-minute safety bail (no progress -> stop)
      parent_->getLogger()->report(
          "[F.1 cont HB] 10-min cap reached; forcing shutdown");
      shutdown_.store(true);
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
