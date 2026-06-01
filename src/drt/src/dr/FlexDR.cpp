// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "dr/FlexDR.h"

#include "dr/AdaptiveMarkerModel.h"
#include "dr/ObjLocality.h"  // P4.0 BoundaryDiag — locality classifier

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "boost/archive/text_iarchive.hpp"
#include "boost/archive/text_oarchive.hpp"
#include "boost/io/ios_state.hpp"
#include "boost/polygon/polygon.hpp"
#include "db/drObj/drFig.h"
#include "db/drObj/drShape.h"
#include "db/drObj/drVia.h"
#include "db/infra/KDTree.hpp"
#include "db/infra/frTime.h"
#include "db/obj/frBlockObject.h"
#include "db/obj/frBTerm.h"
#include "db/obj/frMarker.h"
#include "db/obj/frShape.h"
#include "db/obj/frVia.h"
#include "distributed/RoutingJobDescription.h"
#include "distributed/drUpdate.h"
#include "distributed/frArchive.h"
#include "dr/AbstractDRGraphics.h"
#include "dr/FlexDR_conn.h"
#include "drt/TritonRoute.h"
#include "dst/BalancerJobDescription.h"
#include "dst/Distributed.h"
#include "dst/JobMessage.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "frProfileTask.h"
#include "gc/FlexGC.h"
#include "io/io.h"
#include "odb/dbTypes.h"
#include "odb/geom.h"
#include "omp.h"
#include "serialization.h"
#include "utl/Logger.h"
#include "utl/Progress.h"
#include "utl/ScopedTemporaryFile.h"
#include "utl/exception.h"

using odb::dbTechLayerType;

BOOST_CLASS_EXPORT(drt::RoutingJobDescription)

namespace drt {

namespace {

// P4.0 BoundaryDiag — per-worker boundary-diagnostics CSV writer.
// Gated by OPENROAD_DRT_BOUNDARY_DIAG_DIR=<dir>; emits one CSV per
// iteration (boundary_diag_iter<N>.csv) with one row per
// FlexDRWorker::main() call. Row is written single-threaded in
// FlexDR::endWorkersBatch() AFTER FlexDRWorker::end() runs, so the
// merge / boundary-points counters (populated by endRemoveNets and
// endAddNets_merge) are populated by the time we emit. Mutex-locked
// append is conservative; with single-threaded emit only the file-open
// path strictly needs it.
class BoundaryDiagDump
{
 public:
  static BoundaryDiagDump& instance()
  {
    static BoundaryDiagDump inst;
    return inst;
  }
  void initFromEnv()
  {
    if (initialized_) {
      return;
    }
    initialized_ = true;
    const char* v = std::getenv("OPENROAD_DRT_BOUNDARY_DIAG_DIR");
    if (v == nullptr || v[0] == '\0') {
      return;
    }
    dir_ = v;
    enabled_ = true;
    mkdir(dir_.c_str(), 0777);  // best-effort
  }
  bool enabled() const { return enabled_; }
  void setIterBatch(int iter, int batch_id)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    current_iter_ = iter;
    current_batch_ = batch_id;
    if (last_open_iter_ != iter) {
      if (out_.is_open()) {
        out_.flush();
        out_.close();
      }
      const std::string path
          = dir_ + "/boundary_diag_iter" + std::to_string(iter) + ".csv";
      out_.open(path);
      out_ << "iter,batch_id,thread,worker_id,route_xmin,route_ymin,"
              "route_xmax,route_ymax,num_nets,num_true_pins,"
              "num_boundary_pins,num_boundary_nets,num_ext_connfigs,"
              "num_ext_pathsegs,num_ext_vias,num_ext_patchwires,"
              "markers_in,markers_out,markers_out_near_boundary,"
              "boundary_points_removed,boundary_merge_attempts,"
              "boundary_merge_success_h,boundary_merge_success_v,"
              "boundary_pin_layer_histogram,ext_connfig_layer_histogram\n";
      last_open_iter_ = iter;
    }
  }
  int currentIter() const { return current_iter_; }
  int currentBatch() const { return current_batch_; }
  void appendRow(const std::string& row)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (out_.is_open()) {
      out_ << row;
    }
  }
  void closeOnEnd()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (out_.is_open()) {
      out_.flush();
      out_.close();
    }
  }

 private:
  std::mutex mutex_;
  bool initialized_ = false;
  bool enabled_ = false;
  std::string dir_;
  int current_iter_ = -1;
  int current_batch_ = 0;
  int last_open_iter_ = -1;
  std::ofstream out_;
};

// P4.0 BoundaryDiag — per-marker CSV writer. Shares
// OPENROAD_DRT_BOUNDARY_DIAG_DIR with BoundaryDiagDump. Emits one row
// per marker per worker per iter into <dir>/marker_diag_iter<N>.csv.
// Written from inside the OMP-parallel FlexDRWorker::main() — the
// mutex-locked appendRows() guards the write.
class MarkerDiagDump
{
 public:
  static MarkerDiagDump& instance()
  {
    static MarkerDiagDump inst;
    return inst;
  }
  void initFromEnv()
  {
    if (initialized_) {
      return;
    }
    initialized_ = true;
    const char* v = std::getenv("OPENROAD_DRT_BOUNDARY_DIAG_DIR");
    if (v == nullptr || v[0] == '\0') {
      return;
    }
    dir_ = v;
    enabled_ = true;
    mkdir(dir_.c_str(), 0777);  // best-effort
  }
  bool enabled() const { return enabled_; }
  void setIterBatch(int iter, int batch_id)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    current_iter_ = iter;
    current_batch_ = batch_id;
    if (last_open_iter_ != iter) {
      if (out_.is_open()) {
        out_.flush();
        out_.close();
      }
      const std::string path
          = dir_ + "/marker_diag_iter" + std::to_string(iter) + ".csv";
      out_.open(path);
      // P4.0 schema: 34 columns. 3-way local/boundary/ext per-type bucket
      // matching the ObjLocality classifier.
      out_ << "iter,batch_id,thread,worker_id,"
              "marker_idx,marker_layer,marker_rule_class,"
              "bbox_xmin,bbox_ymin,bbox_xmax,bbox_ymax,"
              "distance_to_boundary,"
              "routebox_side,"
              "touches_boundary_crossing_net,"
              "src_has_ext_connfig,src_has_boundary_pin,"
              "nearby_total,"
              "nearby_pathseg_local,nearby_pathseg_boundary,nearby_pathseg_ext,"
              "nearby_via_local,nearby_via_boundary,nearby_via_ext,"
              "nearby_patch_local,nearby_patch_boundary,nearby_patch_ext,"
              "nearby_pair_type,"
              "nearby_crosses_ownership,"
              "nearby_distinct_nets,"
              "first_net_id,"
              "same_net_self_violation,"
              "neighbor_worker_id,"
              "owner_local_movable,owner_ext_movable_by_neighbor,"
              // SeamClassifier v5 — F10 root-cause type + ownership class
              "nearby_min_width_dbu,nearby_max_width_dbu,"
              "both_vias_crossing,"
              "dist_to_prev_seam_dbu,"
              "root_cause_type,ownership_class\n";
      last_open_iter_ = iter;
    }
  }
  int currentIter() const { return current_iter_; }
  int currentBatch() const { return current_batch_; }
  void appendRows(const std::string& rows)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (out_.is_open()) {
      out_ << rows;
    }
  }
  void closeOnEnd()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (out_.is_open()) {
      out_.flush();
      out_.close();
    }
  }

 private:
  std::mutex mutex_;
  bool initialized_ = false;
  bool enabled_ = false;
  std::string dir_;
  int current_iter_ = -1;
  int current_batch_ = 0;
  int last_open_iter_ = -1;
  std::ofstream out_;
};

// P4.0 BoundaryDiag — global registry mapping routeBox 4-tuple to its
// worker's emit_worker_id. Used to resolve neighbor_worker_id in the
// per-marker CSV: at marker emit time we compute the routeBox of the
// would-be neighbor across the marker's routebox_side and look it up
// here. Returns -1 when no worker exists at that position (chip edge,
// non-uniform tiling, or worker absent from this batch). Populated
// single-threaded in FlexDR::processWorkersBatch BEFORE the OMP loop
// runs; read read-only from inside the parallel marker emit. Mutex is
// kept for safety but in practice the writes are complete before any
// read fires.
class WorkerBoxRegistry
{
 public:
  static WorkerBoxRegistry& instance()
  {
    static WorkerBoxRegistry inst;
    return inst;
  }
  void clear()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    map_.clear();
  }
  void registerWorker(const odb::Rect& rb, int worker_id)
  {
    const auto key = makeKey(rb);
    std::lock_guard<std::mutex> lk(mutex_);
    map_[key] = worker_id;
  }
  int lookup(const odb::Rect& rb) const
  {
    const auto key = makeKey(rb);
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = map_.find(key);
    return (it == map_.end()) ? -1 : it->second;
  }

 private:
  using Key = std::tuple<frCoord, frCoord, frCoord, frCoord>;
  static Key makeKey(const odb::Rect& rb)
  {
    return std::make_tuple(rb.xMin(), rb.yMin(), rb.xMax(), rb.yMax());
  }
  mutable std::mutex mutex_;
  std::map<Key, int> map_;
};

// SeamClassifier v5 (F10 panel D/F) — per-iter snapshot of worker-grid
// seam coordinates. addRouteBox() is called once per worker (during
// processWorkersBatch::registerWorker). snapshotAsPrev() is called at
// the start of each new iter in optimizationFlow — it folds the
// just-finished iter's seams into prev_xs_/prev_ys_ and clears curr_.
// distToPrev(cx, cy) returns the L-inf distance from a point to the
// nearest previous-iter routeBox boundary line (used to flag
// reroute-jog interior markers in the per-marker CSV).
class PerIterSeams
{
 public:
  static PerIterSeams& instance()
  {
    static PerIterSeams inst;
    return inst;
  }
  void clearAll()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    curr_xs_.clear();
    curr_ys_.clear();
    prev_xs_.clear();
    prev_ys_.clear();
  }
  void addRouteBox(const odb::Rect& rb)
  {
    std::lock_guard<std::mutex> lk(mutex_);
    curr_xs_.push_back(rb.xMin());
    curr_xs_.push_back(rb.xMax());
    curr_ys_.push_back(rb.yMin());
    curr_ys_.push_back(rb.yMax());
  }
  void snapshotAsPrev()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    prev_xs_ = curr_xs_;
    prev_ys_ = curr_ys_;
    std::sort(prev_xs_.begin(), prev_xs_.end());
    std::sort(prev_ys_.begin(), prev_ys_.end());
    prev_xs_.erase(std::unique(prev_xs_.begin(), prev_xs_.end()),
                   prev_xs_.end());
    prev_ys_.erase(std::unique(prev_ys_.begin(), prev_ys_.end()),
                   prev_ys_.end());
    curr_xs_.clear();
    curr_ys_.clear();
  }
  frCoord distToPrev(frCoord cx, frCoord cy) const
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (prev_xs_.empty() && prev_ys_.empty()) {
      return std::numeric_limits<frCoord>::max();
    }
    auto nearest = [](const std::vector<frCoord>& v, frCoord c) -> frCoord {
      if (v.empty()) {
        return std::numeric_limits<frCoord>::max();
      }
      auto it = std::lower_bound(v.begin(), v.end(), c);
      frCoord best = std::numeric_limits<frCoord>::max();
      if (it != v.end()) {
        best = std::min(best, *it - c);
      }
      if (it != v.begin()) {
        best = std::min(best, c - *(it - 1));
      }
      return best;
    };
    return std::min(nearest(prev_xs_, cx), nearest(prev_ys_, cy));
  }

 private:
  mutable std::mutex mutex_;
  std::vector<frCoord> curr_xs_, curr_ys_;
  std::vector<frCoord> prev_xs_, prev_ys_;

 public:
  // E1 — accessors so CrossSeamRepair can read this iter's accumulated
  // seam coords WITHOUT swapping them to prev (snapshotAsPrev would
  // empty curr_).
  std::vector<frCoord> getCurrXsSorted() const
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto v = curr_xs_;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  }
  std::vector<frCoord> getCurrYsSorted() const
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto v = curr_ys_;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  }
};

// Forward decl — boundaryDiagRuleClassName is defined below; needed
// here for CrossSeamRepair::endIterAnalysis() rule classification.
const char* boundaryDiagRuleClassName(frConstraintTypeEnum t);

// CrossSeamRepair (CSR) E1 — selection-only stage. Gated by env
// OPENROAD_DRT_CSR_DIR. Called at end of each optimizationFlow iter:
// walks topBlock markers, applies the cross-worker different-net
// seam-spacing predicate, emits per-marker selection CSV. NO repair
// workers in E1; this is the candidate-coverage gate experiment.
//
// Predicate (plan §6 stage A):
//   1. marker has >=2 distinct frNet srcs (different-net)
//   2. marker rule_class is spacing-like / short-like / eol-like / cut-like
//   3. marker bbox center is within seam_band_dbu of some iter's worker
//      seam (PerIterSeams::curr_xs_/curr_ys_)
class CrossSeamRepair
{
 public:
  static CrossSeamRepair& instance()
  {
    static CrossSeamRepair inst;
    return inst;
  }
  void initFromEnv()
  {
    if (initialized_) {
      return;
    }
    initialized_ = true;
    const char* v = std::getenv("OPENROAD_DRT_CSR_DIR");
    if (v == nullptr || v[0] == '\0') {
      return;
    }
    dir_ = v;
    enabled_ = true;
    mkdir(dir_.c_str(), 0777);
    // E2 — separate gate: OPENROAD_DRT_CSR_REPAIR=1 enables actual
    // repair-worker spawn (in addition to the E1 selection-only CSV).
    // When unset (or set to '0'), CSR is read-only (E1 mode).
    const char* r = std::getenv("OPENROAD_DRT_CSR_REPAIR");
    if (r != nullptr && r[0] != '\0' && r[0] != '0') {
      repair_enabled_ = true;
    }
    // E2 — open per-job CSV writer.
    const std::string job_path = dir_ + "/csr_e2_jobs.csv";
    e2_jobs_out_.open(job_path);
    e2_jobs_out_ << "iter,cluster_id,layer,box_xmin,box_ymin,box_xmax,"
                    "box_ymax,num_markers_in_cluster,markers_pre,"
                    "markers_post,committed,crosses_seam\n";
  }
  bool enabled() const { return enabled_; }
  bool repairEnabled() const { return enabled_ && repair_enabled_; }
  void closeOnEnd()
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (out_.is_open()) {
      out_.flush();
      out_.close();
    }
  }
  static bool isSpacingLikeRule(const char* rule_class)
  {
    if (rule_class == nullptr) {
      return false;
    }
    std::string s(rule_class);
    return s.find("short") != std::string::npos
           || s.find("spc") != std::string::npos
           || s.find("Spc") != std::string::npos
           || s.find("eol") != std::string::npos
           || s.find("Eol") != std::string::npos
           || s.find("cut") != std::string::npos
           || s.find("Cut") != std::string::npos;
  }
  void endIterAnalysis(int iter,
                       frBlock* topBlock,
                       const std::vector<frCoord>& seam_xs_sorted,
                       const std::vector<frCoord>& seam_ys_sorted,
                       frCoord seam_band_dbu)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (last_iter_ != iter) {
      if (out_.is_open()) {
        out_.flush();
        out_.close();
      }
      const std::string path
          = dir_ + "/csr_e1_iter" + std::to_string(iter) + ".csv";
      out_.open(path);
      out_ << "iter,marker_idx,layer,rule_class,bbox_xmin,bbox_ymin,"
              "bbox_xmax,bbox_ymax,num_distinct_nets,is_diff_net,"
              "rule_spacing_like,dist_to_nearest_seam_dbu,is_near_seam,"
              "is_csr_candidate\n";
      last_iter_ = iter;
    }
    auto distToSeam = [&](frCoord c,
                          const std::vector<frCoord>& v) -> frCoord {
      if (v.empty()) {
        return std::numeric_limits<frCoord>::max();
      }
      auto it = std::lower_bound(v.begin(), v.end(), c);
      frCoord best = std::numeric_limits<frCoord>::max();
      if (it != v.end()) {
        best = std::min(best, *it - c);
      }
      if (it != v.begin()) {
        best = std::min(best, c - *(it - 1));
      }
      return best;
    };
    int idx = -1;
    const auto& markers = topBlock->getMarkers();
    for (const auto& mu : markers) {
      ++idx;
      const auto& mk = *mu;
      const auto bb = mk.getBBox();
      const frLayerNum ln = mk.getLayerNum();
      const char* rc = "unknown";
      if (mk.getConstraint() != nullptr) {
        rc = boundaryDiagRuleClassName(mk.getConstraint()->typeId());
      }
      std::set<frNet*> nets;
      for (auto* src : mk.getSrcs()) {
        if (src == nullptr) {
          continue;
        }
        frNet* n = nullptr;
        switch (src->typeId()) {
          case frcNet:
            n = static_cast<frNet*>(src);
            break;
          case frcInstTerm:
            n = static_cast<frInstTerm*>(src)->getNet();
            break;
          case frcBTerm:
            n = static_cast<frBTerm*>(src)->getNet();
            break;
          default:
            break;
        }
        if (n != nullptr) {
          nets.insert(n);
        }
      }
      const int ndn = static_cast<int>(nets.size());
      const bool is_diff_net = ndn >= 2;
      const bool rule_ok = isSpacingLikeRule(rc);
      const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
      const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
      const frCoord dx = distToSeam(cx, seam_xs_sorted);
      const frCoord dy = distToSeam(cy, seam_ys_sorted);
      const frCoord d_seam = std::min(dx, dy);
      const bool is_near_seam = (d_seam <= seam_band_dbu);
      const bool is_candidate = is_diff_net && rule_ok && is_near_seam;
      out_ << iter << "," << idx << "," << ln << "," << rc << "," << bb.xMin()
           << "," << bb.yMin() << "," << bb.xMax() << "," << bb.yMax() << ","
           << ndn << "," << (is_diff_net ? 1 : 0) << ","
           << (rule_ok ? 1 : 0) << "," << d_seam << ","
           << (is_near_seam ? 1 : 0) << "," << (is_candidate ? 1 : 0) << "\n";
    }
    out_.flush();
  }

  // E2 — per-iter repair-worker spawn pipeline. Called by
  // FlexDR::optimizationFlow AFTER the batch loop, BEFORE the iter
  // completes. Builds clusters → repair boxes → returns jobs for
  // FlexDR to spawn via createWorker. Stateless (returns a fresh
  // vector each call).
  struct RepairJob
  {
    int cluster_id;
    odb::Rect box;
    frLayerNum layer;
    int num_markers_in_cluster;
    bool crosses_seam;
  };
  std::vector<RepairJob> buildRepairJobs(
      int iter,
      frBlock* topBlock,
      const std::vector<frCoord>& seam_xs_sorted,
      const std::vector<frCoord>& seam_ys_sorted,
      frCoord drc_safe_dist_dbu,
      frCoord merge_t_dbu,
      frCoord max_box_dbu);
  void recordJobResult(int iter,
                       int cluster_id,
                       frLayerNum layer,
                       const odb::Rect& box,
                       int num_markers_in_cluster,
                       int markers_pre,
                       int markers_post,
                       bool committed,
                       bool crosses_seam)
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (e2_jobs_out_.is_open()) {
      e2_jobs_out_ << iter << "," << cluster_id << "," << layer << ","
                   << box.xMin() << "," << box.yMin() << "," << box.xMax()
                   << "," << box.yMax() << "," << num_markers_in_cluster
                   << "," << markers_pre << "," << markers_post << ","
                   << (committed ? 1 : 0) << "," << (crosses_seam ? 1 : 0)
                   << "\n";
      e2_jobs_out_.flush();
    }
  }

 private:
  std::mutex mutex_;
  bool initialized_ = false;
  bool enabled_ = false;
  bool repair_enabled_ = false;
  std::string dir_;
  std::ofstream out_;
  std::ofstream e2_jobs_out_;
  int last_iter_ = -1;
};

// P4.0 — classify marker bbox centroid against routeBox edges. K=200 DBU
// band; corner band returns a diagonal token. Pure function.
const char* boundaryDiagRouteBoxSide(const odb::Rect& bb,
                                     const odb::Rect& routeBox,
                                     const frCoord k_band)
{
  const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
  const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
  const frCoord d_w = std::abs(cx - routeBox.xMin());
  const frCoord d_e = std::abs(cx - routeBox.xMax());
  const frCoord d_s = std::abs(cy - routeBox.yMin());
  const frCoord d_n = std::abs(cy - routeBox.yMax());
  const bool near_w = d_w <= k_band;
  const bool near_e = d_e <= k_band;
  const bool near_s = d_s <= k_band;
  const bool near_n = d_n <= k_band;
  if (near_n && near_e) {
    return "NE";
  }
  if (near_n && near_w) {
    return "NW";
  }
  if (near_s && near_e) {
    return "SE";
  }
  if (near_s && near_w) {
    return "SW";
  }
  if (near_n) {
    return "N";
  }
  if (near_s) {
    return "S";
  }
  if (near_e) {
    return "E";
  }
  if (near_w) {
    return "W";
  }
  return "INTERIOR";
}

// P4.0 — given a routeBox + side token, compute the routeBox of the
// hypothetical neighbor across that seam. Uniform-tiling assumption
// (stride = routeBox dimensions). Diagonal sides shift both axes.
odb::Rect boundaryDiagNeighborBox(const odb::Rect& routeBox, const char* side)
{
  const frCoord w = routeBox.xMax() - routeBox.xMin();
  const frCoord h = routeBox.yMax() - routeBox.yMin();
  frCoord dx = 0;
  frCoord dy = 0;
  if (side[0] == 'N') {
    dy = +h;
  } else if (side[0] == 'S') {
    dy = -h;
  } else if (side[0] == 'E') {
    dx = +w;
  } else if (side[0] == 'W') {
    dx = -w;
  }
  if (side[0] != '\0' && side[1] != '\0') {
    if (side[1] == 'E') {
      dx = +w;
    } else if (side[1] == 'W') {
      dx = -w;
    }
  }
  return odb::Rect(routeBox.xMin() + dx,
                   routeBox.yMin() + dy,
                   routeBox.xMax() + dx,
                   routeBox.yMax() + dy);
}

// P4.0 — short-name lookup for frConstraintTypeEnum (marker_rule_class
// column in the per-marker CSV).
const char* boundaryDiagRuleClassName(frConstraintTypeEnum t)
{
  switch (t) {
    case frConstraintTypeEnum::frcShortConstraint:
      return "short";
    case frConstraintTypeEnum::frcAreaConstraint:
      return "minArea";
    case frConstraintTypeEnum::frcMinWidthConstraint:
      return "minWidth";
    case frConstraintTypeEnum::frcSpacingConstraint:
      return "spc";
    case frConstraintTypeEnum::frcSpacingEndOfLineConstraint:
      return "eol";
    case frConstraintTypeEnum::frcSpacingEndOfLineParallelEdgeConstraint:
      return "eolParEdge";
    case frConstraintTypeEnum::frcSpacingTableConstraint:
      return "spcTable";
    case frConstraintTypeEnum::frcSpacingTablePrlConstraint:
      return "spcTablePrl";
    case frConstraintTypeEnum::frcSpacingTableTwConstraint:
      return "spcTableTw";
    case frConstraintTypeEnum::frcLef58SpacingTableConstraint:
      return "lef58SpcTable";
    case frConstraintTypeEnum::frcLef58CutSpacingTableConstraint:
      return "lef58CutSpcTable";
    case frConstraintTypeEnum::frcLef58CutSpacingTablePrlConstraint:
      return "lef58CutSpcTablePrl";
    case frConstraintTypeEnum::frcLef58CutSpacingTableLayerConstraint:
      return "lef58CutSpcTableLayer";
    case frConstraintTypeEnum::frcLef58CutSpacingConstraint:
      return "lef58CutSpc";
    case frConstraintTypeEnum::frcLef58CutSpacingParallelWithinConstraint:
      return "lef58CutSpcParWithin";
    case frConstraintTypeEnum::frcLef58CutSpacingAdjacentCutsConstraint:
      return "lef58CutSpcAdjCuts";
    case frConstraintTypeEnum::frcLef58CutSpacingLayerConstraint:
      return "lef58CutSpcLayer";
    case frConstraintTypeEnum::frcCutSpacingConstraint:
      return "cutSpc";
    case frConstraintTypeEnum::frcMinStepConstraint:
      return "minStep";
    case frConstraintTypeEnum::frcLef58MinStepConstraint:
      return "lef58MinStep";
    case frConstraintTypeEnum::frcMinimumcutConstraint:
      return "minCut";
    case frConstraintTypeEnum::frcOffGridConstraint:
      return "offGrid";
    case frConstraintTypeEnum::frcMinEnclosedAreaConstraint:
      return "minEnclArea";
    case frConstraintTypeEnum::frcLef58CornerSpacingConstraint:
      return "lef58CornerSpc";
    case frConstraintTypeEnum::frcLef58CornerSpacingConcaveCornerConstraint:
      return "lef58CornerSpcConcave";
    case frConstraintTypeEnum::frcLef58CornerSpacingConvexCornerConstraint:
      return "lef58CornerSpcConvex";
    case frConstraintTypeEnum::frcLef58CornerSpacingSpacingConstraint:
      return "lef58CornerSpcSpc";
    case frConstraintTypeEnum::frcLef58CornerSpacingSpacing1DConstraint:
      return "lef58CornerSpcSpc1D";
    case frConstraintTypeEnum::frcLef58CornerSpacingSpacing2DConstraint:
      return "lef58CornerSpcSpc2D";
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineConstraint:
      return "lef58Eol";
    case frConstraintTypeEnum::frcLef58SpacingEndOfLineWithinConstraint:
      return "lef58EolWithin";
    case frConstraintTypeEnum::
        frcLef58SpacingEndOfLineWithinEndToEndConstraint:
      return "lef58EolE2E";
    case frConstraintTypeEnum::
        frcLef58SpacingEndOfLineWithinEncloseCutConstraint:
      return "lef58EolEncloseCut";
    case frConstraintTypeEnum::
        frcLef58SpacingEndOfLineWithinParallelEdgeConstraint:
      return "lef58EolParEdge";
    case frConstraintTypeEnum::
        frcLef58SpacingEndOfLineWithinMaxMinLengthConstraint:
      return "lef58EolMaxMinLen";
    case frConstraintTypeEnum::frcLef58SpacingWrongDirConstraint:
      return "lef58SpcWrongDir";
    case frConstraintTypeEnum::frcLef58CutClassConstraint:
      return "lef58CutClass";
    case frConstraintTypeEnum::frcNonSufficientMetalConstraint:
      return "nsmetal";
    case frConstraintTypeEnum::frcSpacingSamenetConstraint:
      return "spcSamenet";
    case frConstraintTypeEnum::frcLef58RightWayOnGridOnlyConstraint:
      return "lef58RightWayOnGrid";
    case frConstraintTypeEnum::frcLef58RectOnlyConstraint:
      return "lef58RectOnly";
    case frConstraintTypeEnum::frcRecheckConstraint:
      return "recheck";
    case frConstraintTypeEnum::frcSpacingTableInfluenceConstraint:
      return "spcTableInf";
    case frConstraintTypeEnum::frcLef58EolExtensionConstraint:
      return "lef58EolExt";
    case frConstraintTypeEnum::frcLef58EolKeepOutConstraint:
      return "lef58EolKeepOut";
    case frConstraintTypeEnum::frcLef58MinimumCutConstraint:
      return "lef58MinCut";
    case frConstraintTypeEnum::frcMetalWidthViaConstraint:
      return "metalWidthVia";
    case frConstraintTypeEnum::frcLef58AreaConstraint:
      return "lef58Area";
    case frConstraintTypeEnum::frcLef58KeepOutZoneConstraint:
      return "lef58KeepOutZone";
    case frConstraintTypeEnum::frcLef58TwoWiresForbiddenSpcConstraint:
      return "lef58TwoWiresForbSpc";
    case frConstraintTypeEnum::frcLef58ForbiddenSpcConstraint:
      return "lef58ForbSpc";
    case frConstraintTypeEnum::frcLef58EnclosureConstraint:
      return "lef58Encl";
    case frConstraintTypeEnum::frcSpacingRangeConstraint:
      return "spcRange";
    case frConstraintTypeEnum::frcLef58MaxSpacingConstraint:
      return "lef58MaxSpc";
    case frConstraintTypeEnum::frcSpacingTableOrth:
      return "spcTableOrth";
    case frConstraintTypeEnum::frcLef58WidthTableOrth:
      return "lef58WidthTableOrth";
  }
  return "unknown";
}

// CSR E2 — build per-iter repair-job list. Walks topBlock markers,
// re-applies the E1 predicate (cross-worker different-net spacing-like
// near-seam), greedily buckets by (layer, cx/merge_t, cy/merge_t),
// builds bbox-union → bloat by MTSAFEDIST → cap to max_box_dbu →
// snap to box. Verifies cross-seam (box must contain at least one
// seam_x or seam_y line). Returns jobs in deterministic order
// (sorted by cluster_id which is itself ordered by first-marker idx).
std::vector<CrossSeamRepair::RepairJob>
CrossSeamRepair::buildRepairJobs(
    int iter,
    frBlock* topBlock,
    const std::vector<frCoord>& seam_xs_sorted,
    const std::vector<frCoord>& seam_ys_sorted,
    frCoord drc_safe_dist_dbu,
    frCoord merge_t_dbu,
    frCoord max_box_dbu)
{
  std::vector<RepairJob> jobs;
  // Step 1 — collect candidates (re-apply E1 predicate; topBlock markers
  // are the post-iter state).
  struct Cand
  {
    int idx;
    odb::Rect bb;
    frLayerNum ln;
  };
  std::vector<Cand> cands;
  auto distToSeam = [](frCoord c, const std::vector<frCoord>& v) -> frCoord {
    if (v.empty()) {
      return std::numeric_limits<frCoord>::max();
    }
    auto it = std::lower_bound(v.begin(), v.end(), c);
    frCoord best = std::numeric_limits<frCoord>::max();
    if (it != v.end()) {
      best = std::min(best, *it - c);
    }
    if (it != v.begin()) {
      best = std::min(best, c - *(it - 1));
    }
    return best;
  };
  int idx = -1;
  const auto& markers = topBlock->getMarkers();
  for (const auto& mu : markers) {
    ++idx;
    const auto& mk = *mu;
    const auto bb = mk.getBBox();
    const frLayerNum ln = mk.getLayerNum();
    const char* rc = "unknown";
    if (mk.getConstraint() != nullptr) {
      rc = boundaryDiagRuleClassName(mk.getConstraint()->typeId());
    }
    if (!isSpacingLikeRule(rc)) {
      continue;
    }
    std::set<frNet*> nets;
    for (auto* src : mk.getSrcs()) {
      if (src == nullptr) {
        continue;
      }
      frNet* n = nullptr;
      switch (src->typeId()) {
        case frcNet:
          n = static_cast<frNet*>(src);
          break;
        case frcInstTerm:
          n = static_cast<frInstTerm*>(src)->getNet();
          break;
        case frcBTerm:
          n = static_cast<frBTerm*>(src)->getNet();
          break;
        default:
          break;
      }
      if (n != nullptr) {
        nets.insert(n);
      }
    }
    if (nets.size() < 2) {
      continue;
    }
    const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
    const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
    const frCoord d_seam
        = std::min(distToSeam(cx, seam_xs_sorted),
                   distToSeam(cy, seam_ys_sorted));
    if (d_seam > drc_safe_dist_dbu) {
      continue;
    }
    cands.push_back({idx, bb, ln});
  }
  if (cands.empty()) {
    return jobs;
  }
  // Step 2 — cluster by (layer, bucket_x, bucket_y) using merge_t_dbu.
  std::map<std::tuple<frLayerNum, int, int>, std::vector<int>> bucket;
  for (size_t i = 0; i < cands.size(); ++i) {
    const auto& c = cands[i];
    const frCoord cx = (c.bb.xMin() + c.bb.xMax()) / 2;
    const frCoord cy = (c.bb.yMin() + c.bb.yMax()) / 2;
    const int bx = cx / merge_t_dbu;
    const int by = cy / merge_t_dbu;
    bucket[{c.ln, bx, by}].push_back(static_cast<int>(i));
  }
  // Step 3 — build repair box per cluster.
  int cid = -1;
  for (const auto& [key, idxs] : bucket) {
    ++cid;
    const frLayerNum ln = std::get<0>(key);
    odb::Rect box(cands[idxs[0]].bb);
    for (int ii : idxs) {
      box.merge(cands[ii].bb);
    }
    // Bloat by MTSAFEDIST (2 * drc_safe_dist_dbu approximation; we use
    // a separate parameter for cleanliness).
    constexpr frCoord kBloat = 2000;  // MTSAFEDIST
    box = odb::Rect(box.xMin() - kBloat,
                    box.yMin() - kBloat,
                    box.xMax() + kBloat,
                    box.yMax() + kBloat);
    // Cap box dimensions.
    if (box.dx() > max_box_dbu) {
      const frCoord cx = (box.xMin() + box.xMax()) / 2;
      box.set_xlo(cx - max_box_dbu / 2);
      box.set_xhi(cx + max_box_dbu / 2);
    }
    if (box.dy() > max_box_dbu) {
      const frCoord cy = (box.yMin() + box.yMax()) / 2;
      box.set_ylo(cy - max_box_dbu / 2);
      box.set_yhi(cy + max_box_dbu / 2);
    }
    // Cross-seam check — box must contain at least one seam_x OR
    // seam_y line. Otherwise the box is fully inside one prior worker
    // and CSR provides no joint-visibility benefit; skip.
    bool crosses_seam = false;
    {
      auto it = std::lower_bound(
          seam_xs_sorted.begin(), seam_xs_sorted.end(), box.xMin());
      if (it != seam_xs_sorted.end() && *it < box.xMax()) {
        crosses_seam = true;
      }
    }
    if (!crosses_seam) {
      auto it = std::lower_bound(
          seam_ys_sorted.begin(), seam_ys_sorted.end(), box.yMin());
      if (it != seam_ys_sorted.end() && *it < box.yMax()) {
        crosses_seam = true;
      }
    }
    if (!crosses_seam) {
      continue;  // skip non-cross-seam clusters
    }
    jobs.push_back({cid,
                    box,
                    ln,
                    static_cast<int>(idxs.size()),
                    crosses_seam});
  }
  return jobs;
}

// P4.0 — pack a layer-indexed histogram into a "L<n>:<c>|..." string,
// skipping zero entries. Empty string when no nonzero entries.
std::string packLayerHistogram(const std::vector<int>& hist)
{
  std::string out;
  for (size_t i = 0; i < hist.size(); ++i) {
    if (hist[i] == 0) {
      continue;
    }
    if (!out.empty()) {
      out += '|';
    }
    out += 'L';
    out += std::to_string(i);
    out += ':';
    out += std::to_string(hist[i]);
  }
  return out;
}

}  // namespace

using utl::ThreadException;

enum class SerializationType
{
  READ,
  WRITE
};

void serializeWorker(FlexDRWorker* worker, std::string& workerStr)
{
  std::stringstream stream(std::ios_base::binary | std::ios_base::in
                           | std::ios_base::out);
  frOArchive ar(stream);
  registerTypes(ar);
  ar << *worker;
  workerStr = stream.str();
}

void deserializeWorker(FlexDRWorker* worker,
                       frDesign* design,
                       const std::string& workerStr)
{
  std::stringstream stream(
      workerStr,
      std::ios_base::binary | std::ios_base::in | std::ios_base::out);
  frIArchive ar(stream);
  ar.setDesign(design);
  registerTypes(ar);
  ar >> *worker;
}

void serializeViaData(const FlexDRViaData& viaData, std::string& serializedStr)
{
  std::stringstream stream(std::ios_base::binary | std::ios_base::in
                           | std::ios_base::out);
  frOArchive ar(stream);
  registerTypes(ar);
  ar << viaData;
  serializedStr = stream.str();
}

FlexDR::FlexDR(TritonRoute* router,
               frDesign* designIn,
               utl::Logger* loggerIn,
               odb::dbDatabase* dbIn,
               RouterConfiguration* router_cfg)
    : flow_state_machine_(std::make_unique<FlexDRFlow>()),
      router_(router),
      design_(designIn),
      logger_(loggerIn),
      db_(dbIn),
      router_cfg_(router_cfg),
      numWorkUnits_(0),
      dist_(nullptr),
      dist_on_(false),
      dist_port_(0),
      increaseClipsize_(false),
      clipSizeInc_(0),
      iter_(0)
{
  // outer_loop_plus Patch 1 — opt-in construction of AdaptiveMarkerModel.
  // Default OFF: env var absent → unique_ptr stays nullptr and every
  // call site guards on `adaptive_marker_model_ != nullptr`, preserving
  // bit-identical upstream-master behaviour. Patch 2+ promotes this to
  // a Tcl option (detailed_route -adaptive_marker_model).
  if (const char* env = std::getenv("OPENROAD_DRT_ADAPTIVE_MARKER");
      env != nullptr && env[0] == '1') {
    AdaptiveMarkerModel::Options opts;
    opts.enabled = true;
    if (const char* log_path = std::getenv("OPENROAD_DRT_ADAPTIVE_MARKER_LOG");
        log_path != nullptr && log_path[0] != '\0') {
      opts.log_csv = true;
      opts.log_path = log_path;
    }
    // Patch 3.1a — per-getWorkerPolicy-call CSV, gated by separate env var.
    if (const char* policy_path
            = std::getenv("OPENROAD_DRT_ADAPTIVE_POLICY_LOG");
        policy_path != nullptr && policy_path[0] != '\0') {
      opts.log_policy_csv = true;
      opts.policy_log_path = policy_path;
    }
    // Patch 3.2 — tail-marker CSV, gated by separate env var. One row
    // per marker dumped when an iter's marker count is small (default
    // <= 20). Used to compare residual marker locations across
    // variants.
    if (const char* tail_path
            = std::getenv("OPENROAD_DRT_ADAPTIVE_TAIL_LOG");
        tail_path != nullptr && tail_path[0] != '\0') {
      opts.log_tail_csv = true;
      opts.tail_log_path = tail_path;
    }
    // Patch 3.1d — multiplier sweep config via env vars. Defaults are
    // now the Patch 3.3 "config H" values (hot drc=1.50, severe drc=
    // 2.00, marker mul=1.00, decay override disabled = -1). Override
    // any subset to sweep against H.
    auto read_float_env = [](const char* name, float fallback) {
      const char* v = std::getenv(name);
      if (v == nullptr || v[0] == '\0') {
        return fallback;
      }
      try {
        return std::stof(std::string(v));
      } catch (...) {
        return fallback;
      }
    };
    opts.hot_drc_mul
        = read_float_env("OPENROAD_DRT_ADAPTIVE_HOT_DRC_MUL", opts.hot_drc_mul);
    opts.hot_marker_mul
        = read_float_env("OPENROAD_DRT_ADAPTIVE_HOT_MARKER_MUL",
                         opts.hot_marker_mul);
    opts.severe_drc_mul
        = read_float_env("OPENROAD_DRT_ADAPTIVE_SEVERE_DRC_MUL",
                         opts.severe_drc_mul);
    opts.severe_marker_mul
        = read_float_env("OPENROAD_DRT_ADAPTIVE_SEVERE_MARKER_MUL",
                         opts.severe_marker_mul);
    opts.severe_decay_override
        = read_float_env("OPENROAD_DRT_ADAPTIVE_SEVERE_DECAY_OVERRIDE",
                         opts.severe_decay_override);
    opts.hot_percentile
        = read_float_env("OPENROAD_DRT_ADAPTIVE_HOT_PERCENTILE",
                         opts.hot_percentile);
    opts.severe_percentile
        = read_float_env("OPENROAD_DRT_ADAPTIVE_SEVERE_PERCENTILE",
                         opts.severe_percentile);
    // P4.A — Adaptive Boundary-Band Heat (BBH). Default OFF (so all
    // values absent → unique_ptr created with bbh_enabled = false →
    // bit-identical to P3.3 H). Activation requires
    // OPENROAD_DRT_BBH=1 alongside the umbrella
    // OPENROAD_DRT_ADAPTIVE_MARKER=1 gate; the inner options are then
    // overridable via env.
    if (const char* bbh_env = std::getenv("OPENROAD_DRT_BBH");
        bbh_env != nullptr && bbh_env[0] == '1') {
      opts.bbh_enabled = true;
      opts.bbh_band_width_dbu = read_float_env(
          "OPENROAD_DRT_BBH_BAND_WIDTH_DBU", opts.bbh_band_width_dbu);
      opts.bbh_hot_drc_mul = read_float_env("OPENROAD_DRT_BBH_HOT_DRC_MUL",
                                            opts.bbh_hot_drc_mul);
      opts.bbh_severe_drc_mul = read_float_env(
          "OPENROAD_DRT_BBH_SEVERE_DRC_MUL", opts.bbh_severe_drc_mul);
      opts.bbh_hot_percentile = read_float_env(
          "OPENROAD_DRT_BBH_HOT_PERCENTILE", opts.bbh_hot_percentile);
      opts.bbh_severe_percentile = read_float_env(
          "OPENROAD_DRT_BBH_SEVERE_PERCENTILE", opts.bbh_severe_percentile);
    }
    adaptive_marker_model_
        = std::make_unique<AdaptiveMarkerModel>(opts, design_, logger_);
  }
}

FlexDR::~FlexDR() = default;

void FlexDR::setDebug(std::unique_ptr<AbstractDRGraphics> dr_graphics)
{
  graphics_ = std::move(dr_graphics);
}

std::string FlexDRWorker::reloadedMain()
{
  init(design_);
  debugPrint(logger_,
             utl::DRT,
             "autotuner",
             1,
             "Init number of markers {}",
             getInitNumMarkers());
  if (!skipRouting_) {
    route_queue();
  }
  setGCWorker(nullptr);
  cleanup();
  std::string workerStr;
  serializeWorker(this, workerStr);
  return workerStr;
}

void FlexDRWorker::writeUpdates(const std::string& file_name)
{
  std::vector<std::vector<drUpdate>> updates(1);
  for (const auto& net : getDesign()->getTopBlock()->getNets()) {
    for (const auto& guide : net->getGuides()) {
      for (const auto& connFig : guide->getRoutes()) {
        drUpdate update(drUpdate::ADD_GUIDE);
        frPathSeg* pathSeg = static_cast<frPathSeg*>(connFig.get());
        update.setPathSeg(*pathSeg);
        update.setIndexInOwner(guide->getIndexInOwner());
        update.setNet(net.get());
        updates.back().push_back(update);
      }
    }
    for (const auto& shape : net->getShapes()) {
      drUpdate update;
      auto pathSeg = static_cast<frPathSeg*>(shape.get());
      update.setPathSeg(*pathSeg);
      update.setNet(net.get());
      update.setUpdateType(drUpdate::ADD_SHAPE);
      updates.back().push_back(update);
    }
    for (const auto& shape : net->getVias()) {
      drUpdate update;
      auto via = static_cast<frVia*>(shape.get());
      update.setVia(*via);
      update.setNet(net.get());
      update.setUpdateType(drUpdate::ADD_SHAPE);
      updates.back().push_back(update);
    }
    for (const auto& shape : net->getPatchWires()) {
      drUpdate update;
      auto patch = static_cast<frPatchWire*>(shape.get());
      update.setPatchWire(*patch);
      update.setNet(net.get());
      update.setUpdateType(drUpdate::ADD_SHAPE);
      updates.back().push_back(update);
    }
  }
  for (const auto& marker : getDesign()->getTopBlock()->getMarkers()) {
    drUpdate update;
    update.setMarker(*marker);
    update.setUpdateType(drUpdate::ADD_SHAPE);
    updates.back().push_back(update);
  }
  std::ofstream file(file_name.c_str());
  frOArchive ar(file);
  registerTypes(ar);
  ar << updates;
  file.close();
}

int FlexDRWorker::main(frDesign* design)
{
  ProfileTask profile("DRW:main");
  using std::chrono::high_resolution_clock;
  high_resolution_clock::time_point t0 = high_resolution_clock::now();
  auto micronPerDBU = 1.0 / getTech()->getDBUPerUU();
  if (router_cfg_->VERBOSE > 1) {
    logger_->report("start DR worker (BOX) ( {} {} ) ( {} {} )",
                    routeBox_.xMin() * micronPerDBU,
                    routeBox_.yMin() * micronPerDBU,
                    routeBox_.xMax() * micronPerDBU,
                    routeBox_.yMax() * micronPerDBU);
  }
  initMarkers(design);
  // P4.0 BoundaryDiag — capture initial marker count (unconditional;
  // cheap; no observable behavior change). markers_out and
  // markers_out_near_boundary are populated below before cleanup().
  mutableBoundaryDiagStats().markers_in = getInitNumMarkers();
  if (getDRIter() && getInitNumMarkers() == 0 && !needRecheck_) {
    skipRouting_ = true;
  }
  if (debugSettings_->debugDumpDR
      && (debugSettings_->box == odb::Rect(-1, -1, -1, -1)
          || routeBox_.intersects(debugSettings_->box))
      && !skipRouting_
      && (debugSettings_->iter == getDRIter()
          || debugSettings_->dumpLastWorker)) {
    std::string workerPath = fmt::format("{}/workerx{}_y{}",
                                         debugSettings_->dumpDir,
                                         routeBox_.xMin(),
                                         routeBox_.yMin());
    if (debugSettings_->dumpLastWorker) {
      workerPath = fmt::format("{}/workerx{}_y{}",
                               debugSettings_->dumpDir,
                               debugSettings_->box.xMin(),
                               debugSettings_->box.yMin());
    }
    if (mkdir(workerPath.c_str(), 0777) != 0) {
      logger_->error(
          DRT, 152, "Directory {} could not be created.", workerPath);
    }

    writeUpdates(fmt::format("{}/updates.bin", workerPath));
    {
      std::string viaDataStr;
      serializeViaData(*via_data_, viaDataStr);
      std::ofstream viaDataFile(
          fmt::format("{}/viadata.bin", workerPath).c_str());
      viaDataFile << viaDataStr;
      viaDataFile.close();
    }
    {
      std::string workerStr;
      serializeWorker(this, workerStr);
      std::ofstream workerFile(
          fmt::format("{}/worker.bin", workerPath).c_str());
      workerFile << workerStr;
      workerFile.close();
    }
    {
      std::ofstream router_cfgFile(
          fmt::format("{}/worker_router_cfg.bin", workerPath).c_str());
      frOArchive ar(router_cfgFile);
      registerTypes(ar);
      serializeGlobals(ar, router_cfg_);
      router_cfgFile.close();
    }
  }
  if (!skipRouting_) {
    init(design);
  }
  high_resolution_clock::time_point t1 = high_resolution_clock::now();
  if (!skipRouting_) {
    route_queue();
  }
  high_resolution_clock::time_point t2 = high_resolution_clock::now();
  const int num_markers = getNumMarkers();
  // P4.0 BoundaryDiag — pre-cleanup work. cleanup() clears markers_, so
  // anything that needs the post-route marker geometry must run here.
  // Two pieces:
  //   (1) compute markers_out / markers_out_near_boundary into the stats.
  //   (2) if MarkerDiagDump is enabled, emit one CSV row per marker.
  // The per-worker CSV row is emitted later in FlexDR::endWorkersBatch()
  // AFTER FlexDRWorker::end() runs so the merge / boundary-points
  // counters (populated by endRemoveNets / endAddNets_merge) are nonzero
  // by then. Pre-cleanup stash: emit_thread_num + emit_iter + emit_batch_id.
  if (BoundaryDiagDump::instance().enabled()) {
    auto& bd = mutableBoundaryDiagStats();
    bd.markers_out = num_markers;
    constexpr frCoord kBoundaryDist = 200;
    int near_boundary = 0;
    for (const auto& mk : markers_) {
      const auto bb = mk.getBBox();
      const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
      const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
      const frCoord dx_min = std::min(std::abs(cx - routeBox_.xMin()),
                                      std::abs(cx - routeBox_.xMax()));
      const frCoord dy_min = std::min(std::abs(cy - routeBox_.yMin()),
                                      std::abs(cy - routeBox_.yMax()));
      const frCoord edge_dist = std::min(dx_min, dy_min);
      if (edge_dist <= kBoundaryDist) {
        ++near_boundary;
      }
    }
    bd.markers_out_near_boundary = near_boundary;
    bd.emit_thread_num = omp_get_thread_num();
    bd.emit_iter = BoundaryDiagDump::instance().currentIter();
    bd.emit_batch_id = BoundaryDiagDump::instance().currentBatch();
    bd.emit_pending = true;
  }
  // P4.0 BoundaryDiag — per-marker CSV. One row per marker; gated by the
  // same env var as BoundaryDiagDump but written from the OMP-parallel
  // path here (the dump uses a mutex-locked appendRows). worker_id is
  // assigned in FlexDR::processWorkersBatch BEFORE main() runs; we skip
  // emit when emit_worker_id == -1 (env var unset path).
  if (MarkerDiagDump::instance().enabled() && !markers_.empty()
      && mutableBoundaryDiagStats().emit_worker_id != -1) {
    const int worker_id = mutableBoundaryDiagStats().emit_worker_id;
    std::stringstream ms;
    for (size_t mi = 0; mi < markers_.size(); ++mi) {
      const auto& mk = markers_[mi];
      const auto bb = mk.getBBox();
      // L-infinity distance from bbox to nearest routeBox edge. Negative
      // (outside) clipped to 0.
      const frCoord dx_left = bb.xMin() - routeBox_.xMin();
      const frCoord dx_right = routeBox_.xMax() - bb.xMax();
      const frCoord dy_bot = bb.yMin() - routeBox_.yMin();
      const frCoord dy_top = routeBox_.yMax() - bb.yMax();
      frCoord dist = std::min(std::min(dx_left, dx_right),
                              std::min(dy_bot, dy_top));
      if (dist < 0) {
        dist = 0;
      }
      const char* rule_class = "unknown";
      if (mk.getConstraint() != nullptr) {
        rule_class = boundaryDiagRuleClassName(mk.getConstraint()->typeId());
      }
      // Resolve source frNets via typeId() dispatch (frcInstTerm/frcBTerm/
      // frcNet). Other types contribute no net membership.
      int touches_boundary_net = 0;
      int src_has_ext = 0;
      int src_has_bpin = 0;
      std::set<frNet*> distinct_nets;
      int first_net_id = -1;
      for (auto* src : mk.getSrcs()) {
        if (src == nullptr) {
          continue;
        }
        frNet* net = nullptr;
        switch (src->typeId()) {
          case frcNet:
            net = static_cast<frNet*>(src);
            break;
          case frcInstTerm:
            net = static_cast<frInstTerm*>(src)->getNet();
            break;
          case frcBTerm:
            net = static_cast<frBTerm*>(src)->getNet();
            break;
          default:
            break;
        }
        if (net == nullptr) {
          continue;
        }
        if (distinct_nets.insert(net).second && first_net_id == -1) {
          first_net_id = net->getId();
        }
        if (boundary_crossing_nets_.count(net) != 0u) {
          touches_boundary_net = 1;
          src_has_bpin = 1;
        }
        if (ext_connfig_nets_.count(net) != 0u) {
          src_has_ext = 1;
        }
      }
      const int distinct_net_count = static_cast<int>(distinct_nets.size());
      const int same_net_self_violation = (distinct_net_count == 1) ? 1 : 0;
      // Per-marker region query against the worker's local drConnFig
      // rtree. Bloat marker bbox by 1 routing-track pitch (fallback 200
      // DBU). Query same layer only.
      int nearby_total = 0;
      int nearby_pathseg_local = 0;
      int nearby_pathseg_boundary = 0;
      int nearby_pathseg_ext = 0;
      int nearby_via_local = 0;
      int nearby_via_boundary = 0;
      int nearby_via_ext = 0;
      int nearby_patch_local = 0;
      int nearby_patch_boundary = 0;
      int nearby_patch_ext = 0;
      // SeamClassifier v5 — width signature across nearby pathSegs
      // (-1 sentinel before any pathseg seen).
      frCoord nearby_min_width_dbu = -1;
      frCoord nearby_max_width_dbu = -1;
      std::set<std::string> nearby_tokens;
      std::set<drNet*> nearby_distinct_nets_set;
      int nearby_local_or_boundary_any = 0;
      int nearby_ext_any = 0;
      {
        const frLayerNum mk_layer = mk.getLayerNum();
        const int num_layers = static_cast<int>(getTech()->getLayers().size());
        if (mk_layer >= 0 && mk_layer < num_layers) {
          frCoord pitch = 0;
          if (auto* layer = getTech()->getLayer(mk_layer); layer != nullptr) {
            pitch = static_cast<frCoord>(layer->getPitch());
          }
          if (pitch <= 0) {
            pitch = 200;  // safe fallback
          }
          odb::Rect bloated(bb.xMin() - pitch,
                            bb.yMin() - pitch,
                            bb.xMax() + pitch,
                            bb.yMax() + pitch);
          std::vector<drConnFig*> figs;
          getWorkerRegionQuery().query(bloated, mk_layer, figs);
          // Cache ext-vector pointer per drNet to avoid rescans.
          std::map<drNet*, const std::vector<std::unique_ptr<drConnFig>>*>
              ext_lists;
          for (auto* cf : figs) {
            if (cf == nullptr) {
              continue;
            }
            ++nearby_total;
            drNet* dnet = cf->getNet();
            if (dnet != nullptr) {
              nearby_distinct_nets_set.insert(dnet);
            }
            bool is_ext = false;
            if (dnet != nullptr) {
              auto it = ext_lists.find(dnet);
              if (it == ext_lists.end()) {
                it = ext_lists
                         .emplace(dnet, &dnet->getExtConnFigs())
                         .first;
              }
              for (const auto& up : *it->second) {
                if (up.get() == cf) {
                  is_ext = true;
                  break;
                }
              }
            }
            const ObjLocality loc
                = classifyObj(cf, routeBox_, getExtBox(), getDrcBox(), is_ext);
            const char* loc_tok = objLocalityToken(loc);
            const char* type_tok = nullptr;
            switch (cf->typeId()) {
              case drcPathSeg:
                type_tok = "pathseg";
                if (loc == ObjLocality::LOCAL_ROUTE) {
                  ++nearby_pathseg_local;
                } else if (loc == ObjLocality::BOUNDARY_TOUCHING) {
                  ++nearby_pathseg_boundary;
                } else if (loc == ObjLocality::EXT_CONTEXT) {
                  ++nearby_pathseg_ext;
                }
                {
                  // SeamClassifier v5 — capture width signature.
                  auto* ps = static_cast<drPathSeg*>(cf);
                  const frCoord w = ps->getStyle().getWidth();
                  if (w > 0) {
                    if (nearby_min_width_dbu < 0
                        || w < nearby_min_width_dbu) {
                      nearby_min_width_dbu = w;
                    }
                    if (w > nearby_max_width_dbu) {
                      nearby_max_width_dbu = w;
                    }
                  }
                }
                break;
              case drcVia:
                type_tok = "via";
                if (loc == ObjLocality::LOCAL_ROUTE) {
                  ++nearby_via_local;
                } else if (loc == ObjLocality::BOUNDARY_TOUCHING) {
                  ++nearby_via_boundary;
                } else if (loc == ObjLocality::EXT_CONTEXT) {
                  ++nearby_via_ext;
                }
                break;
              case drcPatchWire:
                type_tok = "patch";
                if (loc == ObjLocality::LOCAL_ROUTE) {
                  ++nearby_patch_local;
                } else if (loc == ObjLocality::BOUNDARY_TOUCHING) {
                  ++nearby_patch_boundary;
                } else if (loc == ObjLocality::EXT_CONTEXT) {
                  ++nearby_patch_ext;
                }
                break;
              default:
                break;
            }
            if (type_tok != nullptr) {
              std::string tok = type_tok;
              tok += '.';
              tok += loc_tok;
              nearby_tokens.insert(tok);
              if (loc == ObjLocality::EXT_CONTEXT) {
                nearby_ext_any = 1;
              } else if (loc == ObjLocality::LOCAL_ROUTE
                         || loc == ObjLocality::BOUNDARY_TOUCHING) {
                nearby_local_or_boundary_any = 1;
              }
            }
          }
        }
      }
      std::string nearby_pair_type;
      if (nearby_tokens.empty()) {
        nearby_pair_type = "none";
      } else {
        bool first_tok = true;
        for (const auto& tok : nearby_tokens) {
          if (!first_tok) {
            nearby_pair_type += "+";
          }
          nearby_pair_type += tok;
          first_tok = false;
        }
      }
      const int nearby_crosses_ownership
          = (nearby_local_or_boundary_any != 0 && nearby_ext_any != 0) ? 1 : 0;
      const int nearby_distinct_nets
          = static_cast<int>(nearby_distinct_nets_set.size());
      // routebox_side: K=200 DBU band; corner zone yields a diagonal.
      constexpr frCoord kSideBand = 200;
      const char* side = boundaryDiagRouteBoxSide(bb, routeBox_, kSideBand);
      // neighbor_worker_id: lookup in the global registry using the
      // routeBox shifted across the marker's side. INTERIOR -> -1.
      int neighbor_worker_id = -1;
      if (side[0] != 'I') {  // not "INTERIOR"
        const odb::Rect nb_box = boundaryDiagNeighborBox(routeBox_, side);
        neighbor_worker_id = WorkerBoxRegistry::instance().lookup(nb_box);
      }
      // owner_local_movable: this worker has a drNet for at least one of
      // the marker's source frNets with a non-empty routeConnFigs.
      int owner_local_movable = 0;
      for (frNet* fnet : distinct_nets) {
        if (fnet == nullptr) {
          continue;
        }
        const std::vector<drNet*>* dr_nets = getDRNets(fnet);
        if (dr_nets == nullptr) {
          continue;
        }
        for (drNet* dn : *dr_nets) {
          if (dn != nullptr && !dn->getRouteConnFigs().empty()) {
            owner_local_movable = 1;
            break;
          }
        }
        if (owner_local_movable != 0) {
          break;
        }
      }
      // owner_ext_movable_by_neighbor: generous first cut per spec.
      // If src_has_ext is set and any source frNet is not special, the
      // neighbor worker COULD rip the local copy. P4.0b refinement may
      // query the registry for the neighbor's routeConnFigs directly.
      int owner_ext_movable_by_neighbor = 0;
      if (src_has_ext != 0) {
        for (frNet* fnet : distinct_nets) {
          if (fnet == nullptr) {
            continue;
          }
          if (ext_connfig_nets_.count(fnet) == 0u) {
            continue;
          }
          if (!fnet->isSpecial()) {
            owner_ext_movable_by_neighbor = 1;
            break;
          }
        }
      }
      // SeamClassifier v5 — derive F10 root-cause type + ownership
      // class from the BoundaryDiag fields already computed above. No
      // additional region queries; pure derivation.
      const frCoord cx = (bb.xMin() + bb.xMax()) / 2;
      const frCoord cy = (bb.yMin() + bb.yMax()) / 2;
      const frCoord dist_to_prev_seam_dbu
          = PerIterSeams::instance().distToPrev(cx, cy);
      const frCoord nearby_min_width_dbu_out
          = (nearby_min_width_dbu < 0) ? 0 : nearby_min_width_dbu;
      const frCoord nearby_max_width_dbu_out
          = (nearby_max_width_dbu < 0) ? 0 : nearby_max_width_dbu;
      const int both_vias_crossing
          = (nearby_via_local > 0 && nearby_via_ext > 0) ? 1 : 0;
      const bool same_net_nearby = (nearby_distinct_nets <= 1);
      const bool cross = (nearby_crosses_ownership != 0);
      const bool widths_differ
          = (nearby_min_width_dbu > 0
             && nearby_max_width_dbu > nearby_min_width_dbu);
      const std::string rule_s(rule_class);
      const bool is_cut_rule
          = (rule_s.find("Cut") != std::string::npos)
            || (rule_s.find("cutSpc") != std::string::npos)
            || (rule_s.find("cut") != std::string::npos);
      const bool has_via_either
          = (nearby_via_local + nearby_via_boundary + nearby_via_ext > 0);
      const bool has_pathseg_either
          = (nearby_pathseg_local + nearby_pathseg_boundary
             + nearby_pathseg_ext
             > 0);
      int root_cause_type = 99;  // OTHER
      if (cross) {
        if (both_vias_crossing || is_cut_rule) {
          root_cause_type = 4;  // via-at-pin / via cut spacing
        } else if (has_via_either && has_pathseg_either) {
          root_cause_type = 2;  // layer disagreement (via vs metal)
        } else if (same_net_nearby && widths_differ) {
          root_cause_type = 3;  // width / NDR mismatch
        } else if (same_net_nearby) {
          root_cause_type = 1;  // y-mismatch (default cross-worker
                                // same-net same-width residual)
        } else {
          root_cause_type = 99;  // different-net OTHER
        }
      }
      constexpr frCoord kReroutePrx = 4000;  // 2 x MTSAFEDIST
      const char* ownership_class = "interior";
      if (cross && same_net_nearby) {
        ownership_class = "stitch_short";
      } else if (cross && !same_net_nearby) {
        ownership_class = "seam_spacing";
      } else if (!cross && dist_to_prev_seam_dbu < kReroutePrx) {
        ownership_class = "reroute_jog";
      }
      // Emit row. Column order MUST match the header above.
      ms << MarkerDiagDump::instance().currentIter() << ","
         << MarkerDiagDump::instance().currentBatch() << ","
         << omp_get_thread_num() << "," << worker_id << "," << mi << ","
         << mk.getLayerNum() << "," << rule_class << "," << bb.xMin() << ","
         << bb.yMin() << "," << bb.xMax() << "," << bb.yMax() << "," << dist
         << "," << side << "," << touches_boundary_net << "," << src_has_ext
         << "," << src_has_bpin << "," << nearby_total << ","
         << nearby_pathseg_local << "," << nearby_pathseg_boundary << ","
         << nearby_pathseg_ext << "," << nearby_via_local << ","
         << nearby_via_boundary << "," << nearby_via_ext << ","
         << nearby_patch_local << "," << nearby_patch_boundary << ","
         << nearby_patch_ext << "," << nearby_pair_type << ","
         << nearby_crosses_ownership << "," << nearby_distinct_nets << ","
         << first_net_id << "," << same_net_self_violation << ","
         << neighbor_worker_id << "," << owner_local_movable << ","
         << owner_ext_movable_by_neighbor << ","
         << nearby_min_width_dbu_out << "," << nearby_max_width_dbu_out << ","
         << both_vias_crossing << "," << dist_to_prev_seam_dbu << ","
         << root_cause_type << "," << ownership_class << "\n";
    }
    MarkerDiagDump::instance().appendRows(ms.str());
  }
  cleanup();
  high_resolution_clock::time_point t3 = high_resolution_clock::now();

  using std::chrono::duration;
  using std::chrono::duration_cast;
  duration<double> time_span0 = duration_cast<duration<double>>(t1 - t0);
  duration<double> time_span1 = duration_cast<duration<double>>(t2 - t1);
  duration<double> time_span2 = duration_cast<duration<double>>(t3 - t2);

  if (router_cfg_->VERBOSE > 1) {
    std::stringstream ss;
    ss << "time (INIT/ROUTE/POST) " << time_span0.count() << " "
       << time_span1.count() << " " << time_span2.count() << " " << std::endl;
    std::cout << ss.str() << std::flush;
  }

  debugPrint(logger_,
             DRT,
             "autotuner",
             1,
             "worker ({:.3f} {:.3f}) ({:.3f} {:.3f}) time {} prev_#DRVs {} "
             "curr_#DRVs {}",
             routeBox_.xMin() * micronPerDBU,
             routeBox_.yMin() * micronPerDBU,
             routeBox_.xMax() * micronPerDBU,
             routeBox_.yMax() * micronPerDBU,
             duration_cast<duration<double>>(t3 - t0).count(),
             getInitNumMarkers(),
             num_markers);

  return 0;
}

void FlexDRWorker::distributedMain(frDesign* design)
{
  ProfileTask profile("DR:main");
  if (router_cfg_->VERBOSE > 1) {
    logger_->report("start DR worker (BOX) ( {} {} ) ( {} {} )",
                    routeBox_.xMin() * 1.0 / getTech()->getDBUPerUU(),
                    routeBox_.yMin() * 1.0 / getTech()->getDBUPerUU(),
                    routeBox_.xMax() * 1.0 / getTech()->getDBUPerUU(),
                    routeBox_.yMax() * 1.0 / getTech()->getDBUPerUU());
  }
  initMarkers(design);
  if (getDRIter() && getInitNumMarkers() == 0 && !needRecheck_) {
    skipRouting_ = true;
    return;
  }
}

void FlexDR::initFromTA()
{
  // initialize lists
  for (auto& net : getDesign()->getTopBlock()->getNets()) {
    for (auto& guide : net->getGuides()) {
      for (auto& connFig : guide->getRoutes()) {
        if (connFig->typeId() == frcPathSeg) {
          std::unique_ptr<frShape> ps = std::make_unique<frPathSeg>(
              *(static_cast<frPathSeg*>(connFig.get())));
          auto [bp, ep] = static_cast<frPathSeg*>(ps.get())->getPoints();

          // skip TA dummy segment
          if (odb::Point::manhattanDistance(ep, bp) != 1) {
            net->addShape(std::move(ps));
          }
        } else {
          std::cout << "Error: initFromTA unsupported shape" << std::endl;
        }
      }
    }
  }
}

void FlexDR::initGCell2BoundaryPin()
{
  // initialize size
  auto topBlock = getDesign()->getTopBlock();
  auto gCellPatterns = topBlock->getGCellPatterns();
  auto& xgp = gCellPatterns.at(0);
  auto& ygp = gCellPatterns.at(1);
  auto tmpVec = std::vector<
      frOrderedIdMap<frNet*, std::set<std::pair<odb::Point, frLayerNum>>>>(
      (int) ygp.getCount());
  gcell2BoundaryPin_ = std::vector<std::vector<
      frOrderedIdMap<frNet*, std::set<std::pair<odb::Point, frLayerNum>>>>>(
      (int) xgp.getCount(), tmpVec);
  for (auto& net : topBlock->getNets()) {
    auto netPtr = net.get();
    for (auto& guide : net->getGuides()) {
      for (auto& connFig : guide->getRoutes()) {
        if (connFig->typeId() == frcPathSeg) {
          auto ps = static_cast<frPathSeg*>(connFig.get());
          auto [bp, ep] = ps->getPoints();
          frLayerNum layerNum = ps->getLayerNum();
          // skip TA dummy segment
          auto mdist = odb::Point::manhattanDistance(ep, bp);
          if (mdist == 1 || mdist == 0) {
            continue;
          }
          odb::Point idx1 = design_->getTopBlock()->getGCellIdx(bp);
          odb::Point idx2 = design_->getTopBlock()->getGCellIdx(ep);

          // update gcell2BoundaryPin
          // horizontal
          if (bp.y() == ep.y()) {
            int x1 = idx1.x();
            int x2 = idx2.x();
            int y = idx1.y();
            for (auto x = x1; x <= x2; ++x) {
              odb::Rect gcellBox = topBlock->getGCellBox(odb::Point(x, y));
              frCoord leftBound = gcellBox.xMin();
              frCoord rightBound = gcellBox.xMax();
              const bool hasLeftBound = bp.x() < leftBound;
              const bool hasRightBound = ep.x() >= rightBound;
              if (hasLeftBound) {
                odb::Point boundaryPt(leftBound, bp.y());
                gcell2BoundaryPin_[x][y][netPtr].emplace(boundaryPt, layerNum);
              }
              if (hasRightBound) {
                odb::Point boundaryPt(rightBound, ep.y());
                gcell2BoundaryPin_[x][y][netPtr].emplace(boundaryPt, layerNum);
              }
            }
          } else if (bp.x() == ep.x()) {
            int x = idx1.x();
            int y1 = idx1.y();
            int y2 = idx2.y();
            for (auto y = y1; y <= y2; ++y) {
              odb::Rect gcellBox = topBlock->getGCellBox(odb::Point(x, y));
              frCoord bottomBound = gcellBox.yMin();
              frCoord topBound = gcellBox.yMax();
              const bool hasBottomBound = bp.y() < bottomBound;
              const bool hasTopBound = ep.y() >= topBound;
              if (hasBottomBound) {
                odb::Point boundaryPt(bp.x(), bottomBound);
                gcell2BoundaryPin_[x][y][netPtr].emplace(boundaryPt, layerNum);
              }
              if (hasTopBound) {
                odb::Point boundaryPt(ep.x(), topBound);
                gcell2BoundaryPin_[x][y][netPtr].emplace(boundaryPt, layerNum);
              }
            }
          } else {
            std::cout
                << "Error: non-orthogonal pathseg in initGCell2BoundryPin\n";
          }
        }
      }
    }
  }
}

void FlexDR::init_halfViaEncArea()
{
  auto bottomLayerNum = getTech()->getBottomLayerNum();
  auto topLayerNum = getTech()->getTopLayerNum();
  auto& halfViaEncArea = via_data_.halfViaEncArea;
  for (int i = bottomLayerNum; i <= topLayerNum; i++) {
    if (getTech()->getLayer(i)->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    if (i + 1 <= topLayerNum
        && getTech()->getLayer(i + 1)->getType() == dbTechLayerType::CUT) {
      auto viaDef = getTech()->getLayer(i + 1)->getDefaultViaDef();
      if (viaDef) {
        frVia via(viaDef);
        odb::Rect layer1Box = via.getLayer1BBox();
        odb::Rect layer2Box = via.getLayer2BBox();
        auto layer1HalfArea = layer1Box.area() / 2;
        auto layer2HalfArea = layer2Box.area() / 2;
        halfViaEncArea.emplace_back(layer1HalfArea, layer2HalfArea);
      } else {
        halfViaEncArea.emplace_back(0, 0);
      }
    } else {
      halfViaEncArea.emplace_back(0, 0);
    }
  }
}

void FlexDR::init()
{
  ProfileTask profile("DR:init");
  frTime t;
  if (router_cfg_->VERBOSE > 0) {
    logger_->info(DRT, 187, "Start routing data preparation.");
  }
  initGCell2BoundaryPin();
  getRegionQuery()->initDRObj();  // first init in postProcess

  init_halfViaEncArea();

  if (router_cfg_->VERBOSE > 0) {
    t.print(logger_);
  }

  iter_ = 0;

  if (router_cfg_->VERBOSE > 0) {
    logger_->info(DRT, 194, "Start detail routing.");
  }
  for (const auto& net : getDesign()->getTopBlock()->getNets()) {
    if (net->hasInitialRouting()) {
      for (const auto& via : net->getVias()) {
        auto bottomBox = via->getLayer1BBox();
        auto topBox = via->getLayer2BBox();
        for (auto term : net->getInstTerms()) {
          if (term->getBBox().intersects(bottomBox)) {
            std::vector<frRect> shapes;
            term->getShapes(shapes);
            for (const auto& shape : shapes) {
              if (shape.getLayerNum() != via->getViaDef()->getLayer1Num()) {
                continue;
              }
              if (!shape.intersects(bottomBox)) {
                continue;
              }
              via->setBottomConnected(true);
              break;
            }
          }
          if (via->isBottomConnected()) {
            continue;
          }
          if (term->getBBox().intersects(topBox)) {
            std::vector<frRect> shapes;
            term->getShapes(shapes);
            for (const auto& shape : shapes) {
              if (shape.getLayerNum() != via->getViaDef()->getLayer2Num()) {
                continue;
              }
              if (!shape.intersects(topBox)) {
                continue;
              }
              via->setTopConnected(true);
              break;
            }
          }
        }
      }
    }
  }
}

void FlexDR::removeGCell2BoundaryPin()
{
  gcell2BoundaryPin_.clear();
  gcell2BoundaryPin_.shrink_to_fit();
}

frOrderedIdMap<frNet*, std::set<std::pair<odb::Point, frLayerNum>>>
FlexDR::initDR_mergeBoundaryPin(int startX,
                                int startY,
                                int size,
                                const odb::Rect& routeBox) const
{
  frOrderedIdMap<frNet*, std::set<std::pair<odb::Point, frLayerNum>>> bp;
  auto gCellPatterns = getDesign()->getTopBlock()->getGCellPatterns();
  auto& xgp = gCellPatterns.at(0);
  auto& ygp = gCellPatterns.at(1);
  for (int i = startX; i < (int) xgp.getCount() && i < startX + size; i++) {
    for (int j = startY; j < (int) ygp.getCount() && j < startY + size; j++) {
      auto& currBp = gcell2BoundaryPin_[i][j];
      for (auto& [net, s] : currBp) {
        for (auto& [pt, lNum] : s) {
          if (pt.x() == routeBox.xMin() || pt.x() == routeBox.xMax()
              || pt.y() == routeBox.yMin() || pt.y() == routeBox.yMax()) {
            bp[net].emplace(pt, lNum);
          }
        }
      }
    }
  }
  return bp;
}

void FlexDR::getBatchInfo(int& batchStepX, int& batchStepY)
{
  batchStepX = 2;
  batchStepY = 2;
}

std::unique_ptr<FlexDRWorker> FlexDR::createWorker(const int x_offset,
                                                   const int y_offset,
                                                   const SearchRepairArgs& args,
                                                   const odb::Rect& routeBoxIn)
{
  auto worker = std::make_unique<FlexDRWorker>(
      &via_data_, getDesign(), logger_, router_cfg_);
  odb::Rect route_box(routeBoxIn);
  if (route_box == odb::Rect(0, 0, 0, 0)) {
    auto gCellPatterns = getDesign()->getTopBlock()->getGCellPatterns();
    auto& xgp = gCellPatterns.at(0);
    auto& ygp = gCellPatterns.at(1);
    odb::Rect routeBox1 = getDesign()->getTopBlock()->getGCellBox(
        odb::Point(x_offset, y_offset));
    const int max_i
        = std::min((int) xgp.getCount() - 1, x_offset + args.size - 1);
    const int max_j = std::min((int) ygp.getCount(), y_offset + args.size - 1);
    odb::Rect routeBox2
        = getDesign()->getTopBlock()->getGCellBox(odb::Point(max_i, max_j));
    route_box.init(
        routeBox1.xMin(), routeBox1.yMin(), routeBox2.xMax(), routeBox2.yMax());
  }
  odb::Rect extBox;
  odb::Rect drcBox;
  route_box.bloat(router_cfg_->MTSAFEDIST, extBox);
  route_box.bloat(router_cfg_->DRCSAFEDIST, drcBox);
  worker->setRouteBox(route_box);
  worker->setExtBox(extBox);
  worker->setDrcBox(drcBox);
  worker->setMazeEndIter(args.mazeEndIter);
  worker->setDRIter(iter_);
  worker->setDebugSettings(router_->getDebugSettings());
  if (dist_on_) {
    worker->setDistributed(dist_, dist_ip_, dist_port_, dist_dir_);
  }
  if (!iter_) {
    auto bp = initDR_mergeBoundaryPin(x_offset, y_offset, args.size, route_box);
    worker->setDRIter(0, bp);
  }
  worker->setRipupMode(args.ripupMode);
  worker->setFollowGuide(args.followGuide);
  // TODO: only pass to relevant workers
  worker->setGraphics(graphics_.get());

  // outer_loop_plus Patch 3 — fetch a read-only policy snapshot from
  // the adaptive marker model based on this worker's drc_box overlap
  // with the persistent heat map. The policy is multiplied into the
  // worker's scalar cost knobs BEFORE setCost. With the model unset
  // (env var off) the if-block is skipped and the worker gets the
  // unmodified args.workerXxxCost values — bit-identical to upstream.
  auto worker_drc = args.workerDRCCost;
  auto worker_marker = args.workerMarkerCost;
  auto worker_fixed = args.workerFixedShapeCost;
  auto worker_decay = args.workerMarkerDecay;
  if (adaptive_marker_model_) {
    const auto policy = adaptive_marker_model_->getWorkerPolicy(
        worker->getRouteBox(),
        worker->getDrcBox(),
        iter_,
        args.workerDRCCost,
        args.workerMarkerCost,
        args.workerFixedShapeCost,
        args.workerMarkerDecay);
    worker->setAdaptivePolicy(policy);
    if (policy.enabled) {
      worker_drc = static_cast<frUInt4>(
          std::lround(static_cast<float>(worker_drc) * policy.drc_cost_mul));
      worker_marker = static_cast<frUInt4>(std::lround(
          static_cast<float>(worker_marker) * policy.marker_cost_mul));
      worker_fixed = static_cast<frUInt4>(std::lround(
          static_cast<float>(worker_fixed) * policy.fixed_shape_cost_mul));
      if (policy.marker_decay_override >= 0.0f) {
        worker_decay = policy.marker_decay_override;
      }
    }
  }

  worker->setCost(worker_drc, worker_marker, worker_fixed, worker_decay);
  return worker;
}

namespace {
void printIteration(utl::Logger* logger,
                    const int iter,
                    const std::string& flow_name)
{
  std::string suffix;
  if (iter == 1 || (iter > 20 && iter % 10 == 1)) {
    suffix = "st";
  } else if (iter == 2 || (iter > 20 && iter % 10 == 2)) {
    suffix = "nd";
  } else if (iter == 3 || (iter > 20 && iter % 10 == 3)) {
    suffix = "rd";
  } else {
    suffix = "th";
  }
  logger->info(DRT, 195, "Start {}{} {} iteration.", iter, suffix, flow_name);
}

void printIterationProgress(utl::Logger* logger,
                            FlexDR::IterationProgress& iter_prog,
                            const int num_markers,
                            const int max_perc = 90)
{
  int progress
      = (++iter_prog.cnt_done_workers * 100) / iter_prog.total_num_workers;
  progress = (progress / 10) * 10;  // clip it to multiples of 10%
  if (progress > iter_prog.last_reported_perc && progress <= max_perc) {
    iter_prog.last_reported_perc = progress;
    logger->report(
        "    Completing {}% with {} violations.", progress, num_markers);
    logger->report("    {}.", iter_prog.time);
  }
}
}  // namespace

void FlexDR::reportIterationViolations() const
{
  if (router_cfg_->VERBOSE > 0) {
    logger_->info(DRT,
                  199,
                  "  Number of violations = {}.",
                  getDesign()->getTopBlock()->getNumMarkers());
    if (getDesign()->getTopBlock()->getNumMarkers() > 0) {
      // report violations
      std::map<std::string, std::map<frLayerNum, uint32_t>> violations;
      std::set<frLayerNum> layers;
      const std::map<std::string, std::string> relabel
          = {{"Lef58SpacingEndOfLine", "EOL"},
             {"Lef58CutSpacingTable", "CutSpcTbl"},
             {"Lef58EolKeepOut", "eolKeepOut"}};
      for (const auto& marker : getDesign()->getTopBlock()->getMarkers()) {
        if (!marker->getConstraint()) {
          continue;
        }
        auto type = marker->getConstraint()->getViolName();
        if (relabel.find(type) != relabel.end()) {
          type = relabel.at(type);
        }
        violations[type][marker->getLayerNum()]++;
        layers.insert(marker->getLayerNum());
      }
      std::string line = fmt::format("{:<15}", "Viol/Layer");
      for (auto lNum : layers) {
        std::string lName = getTech()->getLayer(lNum)->getName();
        if (lName.size() >= 7) {
          lName = lName.substr(0, 2) + ".." + lName.substr(lName.size() - 2, 2);
        }
        line += fmt::format("{:>7}", lName);
      }
      logger_->report(line);
      for (auto& [type, typeViolations] : violations) {
        std::string typeName = type;
        if (typeName.size() >= 15) {
          typeName = typeName.substr(0, 12) + "..";
        }
        line = fmt::format("{:<15}", typeName);
        for (auto lNum : layers) {
          line += fmt::format("{:>7}", typeViolations[lNum]);
        }
        logger_->report(line);
      }
    }
  }
  if ((router_cfg_->DRC_RPT_ITER_STEP > 0 && iter_ > 0
       && iter_ % router_cfg_->DRC_RPT_ITER_STEP == 0)
      || logger_->debugCheck(DRT, "autotuner", 1)
      || logger_->debugCheck(DRT, "report", 1)) {
    router_->reportDRC(
        router_cfg_->DRC_RPT_FILE + '-' + std::to_string(iter_) + ".rpt",
        design_->getTopBlock()->getMarkers(),
        "DRC - iter " + std::to_string(iter_));
  }
}

void FlexDR::processWorkersBatch(
    std::vector<std::unique_ptr<FlexDRWorker>>& workers_batch,
    IterationProgress& iter_prog)
{
  // P4.0 BoundaryDiag — initialize the two CSV writers once (env-gated),
  // then bump the shared batch counter and update the per-iter file
  // handle. Both singletons share OPENROAD_DRT_BOUNDARY_DIAG_DIR and the
  // boundary_diag_batch_id_ counter so per-marker rows JOIN cleanly to
  // per-worker rows on (iter, batch_id, worker_id). When the env var is
  // unset both initFromEnv() calls leave .enabled() false and all
  // subsequent calls into the dumps are no-ops.
  BoundaryDiagDump::instance().initFromEnv();
  if (BoundaryDiagDump::instance().enabled()) {
    ++boundary_diag_batch_id_;
    BoundaryDiagDump::instance().setIterBatch(iter_, boundary_diag_batch_id_);
  }
  MarkerDiagDump::instance().initFromEnv();
  if (MarkerDiagDump::instance().enabled()) {
    MarkerDiagDump::instance().setIterBatch(iter_, boundary_diag_batch_id_);
  }
  // CSR E1 — init env-gate.
  CrossSeamRepair::instance().initFromEnv();
  const int num_markers = getDesign()->getTopBlock()->getNumMarkers();
  // P4.0 BoundaryDiag — assign a per-worker monotonic id within this
  // batch and populate the global WorkerBoxRegistry so the per-marker
  // CSV can resolve neighbor_worker_id. Cleared at batch start so each
  // batch sees only its own workers (acceptable: stitch markers always
  // emit in the same batch that produced them). Skipped entirely when
  // the env var is unset, leaving emit_worker_id at its default -1 — the
  // per-marker emit guards on that to short-circuit.
  if (BoundaryDiagDump::instance().enabled()) {
    WorkerBoxRegistry::instance().clear();
    for (int i = 0; i < (int) workers_batch.size(); i++) {
      const int wid = (boundary_diag_batch_id_ << 16) | (i & 0xffff);
      workers_batch[i]->mutableBoundaryDiagStats().emit_worker_id = wid;
      WorkerBoxRegistry::instance().registerWorker(
          workers_batch[i]->getRouteBox(), wid);
      // SeamClassifier v5 — record curr-iter seam coords for use by
      // NEXT iter's distToPrev queries (after snapshotAsPrev in
      // optimizationFlow).
      PerIterSeams::instance().addRouteBox(workers_batch[i]->getRouteBox());
    }
  }
  ThreadException exception;
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < (int) workers_batch.size(); i++) {  // NOLINT
    try {
      workers_batch[i]->main(getDesign());
#pragma omp critical
      {
        if (router_cfg_->VERBOSE > 0) {
          printIterationProgress(logger_, iter_prog, num_markers);
        }
      }
    } catch (...) {
      exception.capture();
    }
  }
  exception.rethrow();
}

void FlexDR::processWorkersBatchDistributed(
    std::vector<std::unique_ptr<FlexDRWorker>>& workers_batch,
    int& version,
    IterationProgress& iter_prog)
{
  if (router_->dist_pool_.has_value()) {
    router_->dist_pool_->join();
  }
  if (version++ == 0 && !design_->hasUpdates()) {
    std::string serializedViaData;
    serializeViaData(via_data_, serializedViaData);
    router_->sendGlobalsUpdates(router_cfg_path_, serializedViaData);
  } else {
    router_->sendDesignUpdates(router_cfg_path_, router_cfg_->MAX_THREADS);
  }

  ProfileTask task("DIST: PROCESS_BATCH");
  const int num_markers = getDesign()->getTopBlock()->getNumMarkers();
  // multi thread
  ThreadException exception;
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < (int) workers_batch.size(); i++) {  // NOLINT
    try {
      workers_batch[i]->distributedMain(getDesign());
    } catch (...) {
      exception.capture();
    }
  }
  exception.rethrow();
  int j = 0;
  std::vector<std::vector<std::pair<int, FlexDRWorker*>>> distWorkerBatches(
      router_->getCloudSize());
  for (int i = 0; i < workers_batch.size(); i++) {
    auto worker = workers_batch.at(i).get();
    if (!worker->isSkipRouting()) {
      distWorkerBatches[j].push_back({i, worker});
      j = (j + 1) % router_->getCloudSize();
    }
  }
  {
    ProfileTask task("DIST: SERIALIZE+SEND");
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < distWorkerBatches.size(); i++) {  // NOLINT
      sendWorkers(distWorkerBatches.at(i), workers_batch);
    }
  }
  std::vector<std::pair<int, std::string>> workers;
  router_->getWorkerResults(workers);
  {
    ProfileTask task("DIST: DESERIALIZING_BATCH");
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < workers.size(); i++) {  // NOLINT
      deserializeWorker(workers_batch.at(workers.at(i).first).get(),
                        design_,
                        workers.at(i).second);
#pragma omp critical
      {
        if (router_cfg_->VERBOSE > 0) {
          printIterationProgress(logger_, iter_prog, num_markers);
        }
      }
    }
  }
}

void FlexDR::endWorkersBatch(
    std::vector<std::unique_ptr<FlexDRWorker>>& workers_batch)
{
  ProfileTask profile("DR:end_batch");
  // single thread
  for (auto& worker : workers_batch) {
    if (worker->end(getDesign())) {
      numWorkUnits_ += 1;
    }
    if (worker->isCongested()) {
      increaseClipsize_ = true;
    }
    // P4.0 BoundaryDiag — emit per-worker CSV row HERE, AFTER end() has
    // populated boundary_points_removed / boundary_merge_attempts /
    // boundary_merge_success_h / boundary_merge_success_v. Pre-cleanup
    // data (markers_out, markers_out_near_boundary, emit_thread_num,
    // emit_iter, emit_batch_id) was stashed by FlexDRWorker::main()
    // before cleanup() cleared markers_. The v4 lesson: emitting from
    // main() (pre-end) leaves the merge counters at 0; that's why this
    // dump lives in endWorkersBatch instead of the worker's main.
    if (BoundaryDiagDump::instance().enabled()) {
      const auto& bd = worker->getBoundaryDiagStats();
      if (bd.emit_pending) {
        const auto& rb = worker->getRouteBox();
        std::stringstream bs;
        bs << bd.emit_iter << "," << bd.emit_batch_id << ","
           << bd.emit_thread_num << "," << bd.emit_worker_id << ","
           << rb.xMin() << "," << rb.yMin() << "," << rb.xMax() << ","
           << rb.yMax() << "," << bd.num_nets << "," << bd.num_true_pins
           << "," << bd.num_boundary_pins << "," << bd.num_boundary_nets
           << "," << bd.num_ext_connfigs << "," << bd.num_ext_pathsegs
           << "," << bd.num_ext_vias << "," << bd.num_ext_patchwires << ","
           << bd.markers_in << "," << bd.markers_out << ","
           << bd.markers_out_near_boundary << ","
           << bd.boundary_points_removed << ","
           << bd.boundary_merge_attempts << ","
           << bd.boundary_merge_success_h << ","
           << bd.boundary_merge_success_v << ","
           << packLayerHistogram(bd.boundary_pin_layer_hist) << ","
           << packLayerHistogram(bd.ext_connfig_layer_hist) << "\n";
        BoundaryDiagDump::instance().appendRow(bs.str());
      }
    }
  }
  workers_batch.clear();
}

odb::Rect FlexDR::getDRVBBox(const odb::Rect& drv_rect) const
{
  odb::Rect route_box = drv_rect;
  auto min_idx = getDesign()->getTopBlock()->getGCellIdx(route_box.ll());
  auto max_idx = getDesign()->getTopBlock()->getGCellIdx(route_box.ur());
  return {min_idx, max_idx};
}

namespace stub_tiles {
/**
 * @brief Clusters DRV boxes.
 *
 * The function uses a greedy algorithm. It starts with a box and tries to merge
 * all other boxes with the following condition:
 * - The resulting merged box is smaller than or equal to 4x4 GCells.
 * It keeps merging untill there is no other possible merges.
 *
 * @param drv_boxes A list of gcell rectangles(with the gcell indices) that hold
 * the violations.
 * @returns a list of merged boxes.
 */
std::vector<odb::Rect> mergeBoxes(std::vector<odb::Rect>& drv_boxes)
{
  std::vector<odb::Rect> merge_boxes{drv_boxes};
  bool merged;
  do {
    merged = false;
    for (auto it1 = merge_boxes.begin(); it1 != merge_boxes.end(); ++it1) {
      for (auto it2 = it1 + 1; it2 != merge_boxes.end(); ++it2) {
        odb::Rect merge_box = (*it1);
        merge_box.merge((*it2));
        if (merge_box.dx() > 4 || merge_box.dy() > 4) {
          continue;
        }
        (*it1).merge((*it2));
        merge_boxes.erase(it2);
        merged = true;
        break;
      }
      if (merged) {
        break;
      }
    }
  } while (merged);
  return merge_boxes;
}
/**
 * @brief Checks if the passed rectangle intersects with any other rectangle in
 * the grid.
 *
 * @param grid A 2-d grid representing the current grid. (-1 value means
 * unoccupied).
 * @param rect_id the current rectangle id to disregard in the check.
 * @return True if the rectangle intersects with an occupied cell by another
 * rectangle.
 */
bool hasOtherRect(const std::vector<std::vector<int>>& grid,
                  const odb::Rect& rect,
                  const int rect_id)
{
  return std::any_of(grid.begin() + rect.xMin(),
                     grid.begin() + rect.xMax() + 1,
                     [&](const std::vector<int>& row) {
                       return std::any_of(row.begin() + rect.yMin(),
                                          row.begin() + rect.yMax() + 1,
                                          [rect_id](int id) {
                                            return id != -1 && id != rect_id;
                                          });
                     });
}
/**
 * @brief Expands the passed rectangle in the required direction.
 *
 * The function expands the passed rectangle in the required direction a gcell
 * at a time with the following conditions:
 * - The expansion is less than or equal to the max_expansion
 * - The expansion does not result a rectangle that intersects with another
 *   rectangle in the grid.
 * @param box The rectangle to be expanded.
 * @param id The rectangle's id.
 * @param grid A 2-d grid representing the current grid. (-1 value means
 * unoccupied).
 * @param dir The direction of the expansion (East, West, North, South).
 * @return The number of expanded gcells (less than or equal to max_expansion)
 */
int expandBox(odb::Rect& box,
              const int id,
              const std::vector<std::vector<int>>& grid,
              const frDirEnum dir,
              const int max_expansion)
{
  auto min_idx = box.ll();
  auto max_idx = box.ur();
  for (int i = 1; i <= max_expansion; i++) {
    switch (dir) {
      case frDirEnum::E:
        max_idx.addX(1);
        break;
      case frDirEnum::W:
        if (min_idx.x() == 0) {
          return i - 1;
        }
        min_idx.addX(-1);
        break;
      case frDirEnum::N:
        max_idx.addY(1);
        break;
      case frDirEnum::S:
        if (min_idx.y() == 0) {
          return i - 1;
        }
        min_idx.addY(-1);
        break;
      default:
        break;
    }
    odb::Rect expanded_box = {min_idx, max_idx};
    if (hasOtherRect(grid, expanded_box, id)) {
      return i - 1;
    }
    box = expanded_box;
  }
  return max_expansion;
}

void populateGrid(std::vector<std::vector<int>>& grid,
                  const odb::Rect& box,
                  const int id)
{
  for (auto x = box.xMin(); x <= box.xMax(); x++) {
    std::fill(
        grid[x].begin() + box.yMin(), grid[x].begin() + box.yMax() + 1, id);
  }
}

struct Wavefront
{
  int id;
  int expansions_done;
  int expansion_east;
  int expansion_west;
  int expansion_north;
  int expansion_south;
  bool operator<(const Wavefront& rhs) const
  {
    return expansions_done > rhs.expansions_done;
  }
};

/**
 * @brief Expand the boxes to reach a max size of 7x7 gcells.
 *
 * The function tries to expand each of the passed merged_boxes into a 7x7 gcell
 * grids. There are 5 variantions of expansions that we consider in this
 * function:
 * - DRVs in Center-Center (Highest priority)
 * - DRVs in Center-East
 * - DRVs in Center-West
 * - DRVs in Center-North
 * - DRVs in Center-South
 * We first expand the first type and then work on the other 4 types of
 * expansions. We prioritize boxes with lower number of expansions done. That
 * way we try to balance the resulting boxes so that each DRV box optimally has
 * the same number of expanded boxes.
 */
std::vector<std::set<odb::Rect>> expandBoxes(
    std::vector<odb::Rect>& merged_boxes)
{
  frUInt4 min_x_idx = std::numeric_limits<frUInt4>::max();
  frUInt4 min_y_idx = std::numeric_limits<frUInt4>::max();
  frUInt4 max_x_idx = std::numeric_limits<frUInt4>::min();
  frUInt4 max_y_idx = std::numeric_limits<frUInt4>::min();
  for (const auto& box : merged_boxes) {
    min_x_idx = std::min(min_x_idx, (frUInt4) box.xMin());
    min_y_idx = std::min(min_y_idx, (frUInt4) box.yMin());
    max_x_idx = std::max(max_x_idx, (frUInt4) box.xMax());
    max_y_idx = std::max(max_y_idx, (frUInt4) box.yMax());
  }
  max_x_idx += 7;
  max_y_idx += 7;
  std::vector<std::vector<int>> grid(max_x_idx);
  std::fill(grid.begin(),
            grid.end(),
            std::vector<int>(max_y_idx, -1));  // -1 means unoccupied
  int id = 0;
  for (const auto& box : merged_boxes) {
    populateGrid(grid, box, id++);
  }

  std::vector<std::set<odb::Rect>> expanded_boxes;
  std::priority_queue<Wavefront> expansions;
  // first we create route boxes centering the violations
  id = 0;
  for (auto box : merged_boxes) {
    const int horizontal_expand = 6 - box.dx();
    const int half_horizontal_expand = horizontal_expand / 2;
    const int vertical_expand = 6 - box.dy();
    const int half_vertical_expand = vertical_expand / 2;
    const int expansion_east
        = expandBox(box, id, grid, frDirEnum::E, half_horizontal_expand);
    const int expansion_west = expandBox(
        box, id, grid, frDirEnum::W, horizontal_expand - expansion_east);
    const int expansion_north
        = expandBox(box, id, grid, frDirEnum::N, half_vertical_expand);
    const int expansion_south = expandBox(
        box, id, grid, frDirEnum::S, vertical_expand - expansion_north);

    expansions.push(
        {id, 1, 0, horizontal_expand, expansion_north, expansion_south});
    expansions.push(
        {id, 1, horizontal_expand, 0, expansion_north, expansion_south});
    expansions.push(
        {id, 1, expansion_east, expansion_west, 0, vertical_expand});
    expansions.push(
        {id, 1, expansion_east, expansion_west, vertical_expand, 0});
    expanded_boxes.push_back({box});
    populateGrid(grid, box, id++);
  }
  while (!expansions.empty()) {
    auto wavefront = expansions.top();
    expansions.pop();
    if (expanded_boxes[wavefront.id].size() != wavefront.expansions_done) {
      wavefront.expansions_done = expanded_boxes[wavefront.id].size();
      expansions.push(wavefront);
      continue;
    }
    auto box = merged_boxes[wavefront.id];
    expandBox(box, wavefront.id, grid, frDirEnum::E, wavefront.expansion_east);
    expandBox(box, wavefront.id, grid, frDirEnum::W, wavefront.expansion_west);
    expandBox(box, wavefront.id, grid, frDirEnum::N, wavefront.expansion_north);
    expandBox(box, wavefront.id, grid, frDirEnum::S, wavefront.expansion_south);
    expanded_boxes[wavefront.id].insert(box);
    populateGrid(grid, box, wavefront.id);
  }
  return expanded_boxes;
}

/**
 * @brief Distributes the workers into batches of non-intersecting borders.
 *
 * The function first calculates the external box to each of the passed route
 * boxes by bloating them by MTSAFEDIST. Then, it calculates the max box of
 * each worker by merging all possible variations of expanded boxes. Finally, it
 * distributes the workers into batches where each batch holds a set of workers
 * that do not intersect.
 */
std::vector<std::vector<int>> getWorkerBatchesBoxes(
    frDesign* design,
    std::vector<std::set<odb::Rect>>& expanded_boxes,
    const frCoord bloating_dist)
{
  if (expanded_boxes.empty()) {
    return {};
  }
  std::vector<odb::Rect> boxes_max;
  boxes_max.resize(expanded_boxes.size());
  int id = 0;
  for (auto& boxes : expanded_boxes) {
    boxes_max[id].mergeInit();
    for (auto rect : boxes) {
      rect.bloat(bloating_dist, rect);
      boxes_max[id].merge(rect);
    }
    ++id;
  }
  std::vector<std::vector<int>> batches;
  batches.push_back({0});
  for (int i = 1; i < boxes_max.size(); i++) {
    auto curr_box = boxes_max[i];
    bool added = false;
    for (auto& batch : batches) {
      bool intersects = false;
      for (const auto& id : batch) {
        if (curr_box.intersects(boxes_max[id])) {
          intersects = true;
          break;
        }
      }
      if (!intersects) {
        batch.emplace_back(i);
        added = true;
        break;
      }
    }
    if (!added) {
      batches.push_back({i});
    }
  }
  return batches;
}
}  // namespace stub_tiles

void FlexDR::stubbornTilesFlow(const SearchRepairArgs& args,
                               IterationProgress& iter_prog)
{
  if (graphics_) {
    graphics_->startIter(iter_, router_cfg_);
  }
  std::vector<odb::Rect> drv_boxes;
  for (const auto& marker : getDesign()->getTopBlock()->getMarkers()) {
    auto box = marker->getBBox();
    drv_boxes.push_back(getDRVBBox(box));
  }
  auto merged_boxes = stub_tiles::mergeBoxes(drv_boxes);
  auto expanded_boxes = stub_tiles::expandBoxes(merged_boxes);

  // Convert gcell indices to actual coordinates
  std::vector<std::set<odb::Rect>> expanded_boxes_coords;
  expanded_boxes_coords.reserve(expanded_boxes.size());
  std::ranges::transform(
      expanded_boxes,
      std::back_inserter(expanded_boxes_coords),
      [this](const auto& box_set) {
        std::set<odb::Rect> coord_set;
        std::ranges::transform(
            box_set,
            std::inserter(coord_set, coord_set.end()),
            [this](const auto& gcell_box) {
              return odb::Rect(
                  getDesign()->getTopBlock()->getGCellBox(gcell_box.ll()).ll(),
                  getDesign()->getTopBlock()->getGCellBox(gcell_box.ur()).ur());
            });
        return coord_set;
      });

  auto route_boxes_batches = stub_tiles::getWorkerBatchesBoxes(
      getDesign(), expanded_boxes_coords, router_cfg_->MTSAFEDIST);
  std::vector<frUInt4> drc_costs
      = {args.workerDRCCost, args.workerDRCCost / 2, args.workerDRCCost * 2};
  std::vector<frUInt4> marker_costs = {args.workerMarkerCost,
                                       args.workerMarkerCost / 2,
                                       args.workerMarkerCost * 2};
  std::vector<std::vector<std::unique_ptr<FlexDRWorker>>> workers_batches(
      route_boxes_batches.size());
  iter_prog.total_num_workers = 0;
  for (int batch_id = 0; batch_id < route_boxes_batches.size(); batch_id++) {
    auto& batch = route_boxes_batches[batch_id];
    for (const auto worker_id : batch) {
      for (const auto& route_box : expanded_boxes_coords[worker_id]) {
        for (auto drc_cost : drc_costs) {
          for (auto marker_cost : marker_costs) {
            auto worker_args = args;
            worker_args.workerDRCCost = drc_cost;
            worker_args.workerMarkerCost = marker_cost;
            workers_batches[batch_id].emplace_back(
                createWorker(0, 0, worker_args, route_box));
            workers_batches[batch_id].back()->setWorkerId(worker_id);
            iter_prog.total_num_workers++;
          }
        }
      }
    }
  }
  bool changed = false;
  omp_set_num_threads(router_cfg_->MAX_THREADS);
  for (auto& batch : workers_batches) {
    processWorkersBatch(batch, iter_prog);
    std::map<int, FlexDRWorker*>
        worker_best_result;  // holds the best worker in results
    for (auto& worker : batch) {
      const int worker_id = worker->getWorkerId();
      if (worker_best_result.find(worker_id) == worker_best_result.end()
          || worker->getBestNumMarkers()
                 < worker_best_result[worker_id]->getBestNumMarkers()) {
        worker_best_result[worker_id] = worker.get();
      }
    }
    for (auto [_, worker] : worker_best_result) {
      changed
          |= (worker->end(getDesign())
              && worker->getBestNumMarkers() != worker->getInitNumMarkers());
    }
    batch.clear();
  }
  flow_state_machine_->setLastIterationEffective(changed);
}

void FlexDR::guideTilesFlow(const SearchRepairArgs& args,
                            IterationProgress& iter_prog)
{
  if (graphics_) {
    graphics_->startIter(iter_, router_cfg_);
  }
  std::vector<odb::Rect> workers;
  for (const auto& marker : getDesign()->getTopBlock()->getMarkers()) {
    for (auto src : marker->getSrcs()) {
      if (src->typeId() == frcNet) {
        auto net = static_cast<frNet*>(src);
        if (net->getOrigGuides().empty()) {
          continue;
        }
        for (const auto& guide : net->getOrigGuides()) {
          if (!guide.getBBox().intersects(marker->getBBox())) {
            continue;
          }
          workers.push_back(guide.getBBox());
        }
      }
    }
  }
  if (workers.empty()) {
    iter_prog.total_num_workers = 1;
    iter_prog.cnt_done_workers = 1;
    flow_state_machine_->setLastIterationEffective(false);
    return;
  }

  for (auto itr1 = workers.begin(); itr1 != workers.end(); ++itr1) {
    for (auto itr2 = itr1 + 1; itr2 != workers.end(); ++itr2) {
      if (itr1->getDir() == itr2->getDir() && itr1->intersects(*itr2)) {
        itr1->merge(*itr2);
        workers.erase(itr2);
        itr2 = itr1;
      }
    }
  }
  std::vector<std::set<odb::Rect>> boxes_set;
  boxes_set.reserve(workers.size());
  for (auto& worker : workers) {
    boxes_set.push_back({worker});
  }
  auto route_boxes_batches = stub_tiles::getWorkerBatchesBoxes(
      getDesign(), boxes_set, router_cfg_->MTSAFEDIST);
  std::vector<std::vector<std::unique_ptr<FlexDRWorker>>> workers_batches(
      route_boxes_batches.size());
  iter_prog.total_num_workers = 0;
  for (int batch_id = 0; batch_id < route_boxes_batches.size(); batch_id++) {
    auto& batch = route_boxes_batches[batch_id];
    for (const auto worker_id : batch) {
      for (auto route_box : boxes_set[worker_id]) {
        workers_batches[batch_id].emplace_back(
            createWorker(0, 0, args, route_box));
        workers_batches[batch_id].back()->setWorkerId(worker_id);
        iter_prog.total_num_workers++;
      }
    }
  }
  bool changed = false;
  omp_set_num_threads(router_cfg_->MAX_THREADS);
  for (auto& batch : workers_batches) {
    processWorkersBatch(batch, iter_prog);
    std::map<int, FlexDRWorker*>
        worker_best_result;  // holds the best worker in results
    for (auto& worker : batch) {
      const int worker_id = worker->getWorkerId();
      if (worker_best_result.find(worker_id) == worker_best_result.end()
          || worker->getBestNumMarkers()
                 < worker_best_result[worker_id]->getBestNumMarkers()) {
        worker_best_result[worker_id] = worker.get();
      }
    }
    for (auto [_, worker] : worker_best_result) {
      changed
          |= (worker->end(getDesign())
              && worker->getBestNumMarkers() != worker->getInitNumMarkers());
    }
    batch.clear();
  }
  flow_state_machine_->setLastIterationEffective(changed);
}
void FlexDR::optimizationFlow(const SearchRepairArgs& args,
                              IterationProgress& iter_prog)
{
  if (graphics_) {
    graphics_->startIter(iter_, router_cfg_);
  }
  // SeamClassifier v5 — fold last iter's seams into prev so this iter's
  // marker emit can compute dist_to_prev_seam_dbu. Iter 0 has nothing
  // to fold; distToPrev returns INT_MAX → reroute_jog never fires.
  PerIterSeams::instance().snapshotAsPrev();
  auto gCellPatterns = getDesign()->getTopBlock()->getGCellPatterns();
  auto& xgp = gCellPatterns.at(0);
  auto& ygp = gCellPatterns.at(1);
  const int size = args.size;
  const int offset = args.offset;
  iter_prog.total_num_workers
      = (((int) xgp.getCount() - 1 - offset) / size + 1)
        * (((int) ygp.getCount() - 1 - offset) / size + 1);

  std::vector<std::unique_ptr<FlexDRWorker>> uworkers;
  int batchStepX, batchStepY;

  getBatchInfo(batchStepX, batchStepY);

  std::vector<std::vector<std::vector<std::unique_ptr<FlexDRWorker>>>> workers(
      batchStepX * batchStepY);

  int xIdx = 0, yIdx = 0;
  for (int i = offset; i < (int) xgp.getCount(); i += size) {
    for (int j = offset; j < (int) ygp.getCount(); j += size) {
      auto worker = createWorker(i, j, args);
      int batch_idx = (xIdx % batchStepX) * batchStepY + yIdx % batchStepY;
      const bool create_new_batch
          = workers[batch_idx].empty()
            || (!dist_on_
                && workers[batch_idx].back().size() >= router_cfg_->BATCHSIZE);
      if (create_new_batch) {
        workers[batch_idx].push_back(
            std::vector<std::unique_ptr<FlexDRWorker>>());
      }
      workers[batch_idx].back().push_back(std::move(worker));

      yIdx++;
    }
    yIdx = 0;
    xIdx++;
  }

  omp_set_num_threads(router_cfg_->MAX_THREADS);
  int version = 0;
  increaseClipsize_ = false;
  numWorkUnits_ = 0;
  // parallel execution
  for (auto& workerBatch : workers) {
    ProfileTask profile("DR:checkerboard");
    for (auto& workersInBatch : workerBatch) {
      {
        const std::string batch_name = std::string("DR:batch<")
                                       + std::to_string(workersInBatch.size())
                                       + ">";
        ProfileTask profile(batch_name.c_str());
        if (dist_on_) {
          processWorkersBatchDistributed(workersInBatch, version, iter_prog);
        } else {
          processWorkersBatch(workersInBatch, iter_prog);
        }
      }
      endWorkersBatch(workersInBatch);
    }
  }

  if (!iter_) {
    removeGCell2BoundaryPin();
  }
  // CSR E1 — selection-only analysis at iter end. ALWAYS runs when
  // OPENROAD_DRT_CSR_DIR is set; gated by enabled().
  if (CrossSeamRepair::instance().enabled()) {
    const auto xs = PerIterSeams::instance().getCurrXsSorted();
    const auto ys = PerIterSeams::instance().getCurrYsSorted();
    CrossSeamRepair::instance().endIterAnalysis(
        iter_,
        getDesign()->getTopBlock(),
        xs,
        ys,
        /*seam_band_dbu=*/router_cfg_->DRCSAFEDIST);
    // CSR E2 — repair-worker spawn. Gated by an additional env var
    // OPENROAD_DRT_CSR_REPAIR=1. Builds repair jobs, spawns workers
    // serially, commits only if local marker count improves.
    // SKIP iter 0: (a) gcell2BoundaryPin_ was just cleared by
    // removeGCell2BoundaryPin() above, so createWorker's iter-0
    // branch would crash, and (b) iter 0 uses RipUpMode::ALL so
    // CSR's targeted repair semantics don't apply.
    if (CrossSeamRepair::instance().repairEnabled() && iter_ != 0) {
      const frCoord kMergeT = 8000;        // ~2 GCells
      const frCoord kMaxBox = 14000;       // ~4 GCells per side
      auto jobs = CrossSeamRepair::instance().buildRepairJobs(
          iter_,
          getDesign()->getTopBlock(),
          xs,
          ys,
          /*drc_safe_dist_dbu=*/router_cfg_->DRCSAFEDIST,
          /*merge_t_dbu=*/kMergeT,
          /*max_box_dbu=*/kMaxBox);
      for (const auto& job : jobs) {
        auto worker = createWorker(0, 0, args, job.box);
        const int rc = worker->main(getDesign());
        if (rc != 0) {
          CrossSeamRepair::instance().recordJobResult(
              iter_, job.cluster_id, job.layer, job.box,
              job.num_markers_in_cluster, -1, -1, false, job.crosses_seam);
          continue;
        }
        const int pre = worker->getInitNumMarkers();
        const int post = worker->getNumMarkers();
        bool committed = false;
        if (post < pre) {
          worker->end(getDesign());
          committed = true;
        }
        CrossSeamRepair::instance().recordJobResult(
            iter_, job.cluster_id, job.layer, job.box,
            job.num_markers_in_cluster, pre, post, committed,
            job.crosses_seam);
      }
    }
  }
}

void FlexDR::searchRepair(const SearchRepairArgs& args)
{
  // Calculate flow state
  const auto flow_state = flow_state_machine_->determineNextFlow(
      {.num_violations = getDesign()->getTopBlock()->getNumMarkers(),
       .args = args});

  const RipUpMode ripupMode = args.ripupMode;
  if ((ripupMode == RipUpMode::DRC || ripupMode == RipUpMode::NEARDRC)
      && getDesign()->getTopBlock()->getMarkers().empty()) {
    return;
  }
  ProfileTask profile(fmt::format("DR:searchRepair{}", iter_).c_str());

  if (dist_on_) {
    if ((iter_ % 10 == 0 && iter_ != 60) || iter_ == 3 || iter_ == 15) {
      router_cfg_path_ = fmt::format("{}globals.{}.ar", dist_dir_, iter_);
      router_->writeGlobals(router_cfg_path_);
    }
  }

  if (router_cfg_->VERBOSE > 0) {
    if (flow_state != FlexDRFlow::State::SKIP) {
      printIteration(logger_, iter_, flow_state_machine_->getFlowName());
    } else {
      logger_->info(DRT, 200, "Skipping iteration {}", iter_);
    }
  }
  // outer_loop_plus Patch 2 — adaptive marker model lifecycle. Begin
  // here decays heat carried from previous iter; the per-iter
  // accumulators get reset by writeCsvRowIfEnabled at endOuterIter
  // below. Guard on the unique_ptr — when env var is unset the model
  // is nullptr and this is a no-op.
  if (adaptive_marker_model_) {
    adaptive_marker_model_->beginOuterIter(iter_);
  }
  // start timer for the current iteration
  IterationProgress iter_prog;

  switch (flow_state) {
    case FlexDRFlow::State::GUIDES:
      guideTilesFlow(args, iter_prog);
      break;

    case FlexDRFlow::State::STUBBORN:
      stubbornTilesFlow(args, iter_prog);
      break;

    case FlexDRFlow::State::OPTIMIZATION:
      optimizationFlow(args, iter_prog);
      break;

    case FlexDRFlow::State::SKIP:
      return;
  }

  if (router_cfg_->VERBOSE > 0) {
    iter_prog.cnt_done_workers--;  // decrement 1 and increment again in
                                   // printIterationProgress
    printIterationProgress(
        logger_, iter_prog, getDesign()->getTopBlock()->getNumMarkers(), 100);
  }
  FlexDRConnectivityChecker checker(
      router_, logger_, router_cfg_, graphics_.get(), dist_on_);
  checker.check(iter_);
  flow_state_machine_->setFixingMaxSpacing(false);
  if (getDesign()->getTopBlock()->getNumMarkers() == 0
      && getTech()->hasMaxSpacingConstraints()) {
    fixMaxSpacing();
  }
  numViols_.push_back(getDesign()->getTopBlock()->getNumMarkers());
  // outer_loop_plus Patch 2 — at end of iter, hand the final marker
  // list (after maxSpacing fix + connectivity check) to the model
  // for observation. Counted ONCE per iter — never per worker — so
  // overlapping worker DRC boxes don't double-count. endOuterIter
  // then updates the hotspot list and writes a CSV row when
  // OPENROAD_DRT_ADAPTIVE_MARKER_LOG is set.
  if (adaptive_marker_model_) {
    const auto& markers_list = getDesign()->getTopBlock()->getMarkers();
    adaptive_marker_model_->observeGlobalMarkers(markers_list);

    // P4.A — geometric stitch classification. A marker is "stitch" if
    // its bbox center is within bbh_band_width_dbu of any worker-grid
    // line. Worker grid lines are derived from args.size + args.offset
    // and the design's gcell pattern; this is a design-general proxy
    // (same approach used by the archived P4.1 trial). The classified
    // markers are fed to observeStitchMarker. When BBH is disabled
    // this loop is skipped entirely to preserve bit-identical
    // behaviour.
    const auto& adaptive_opts = adaptive_marker_model_->getOptions();
    if (adaptive_opts.bbh_enabled && !markers_list.empty()) {
      auto gCellPatterns = getDesign()->getTopBlock()->getGCellPatterns();
      if (gCellPatterns.size() >= 2) {
        const auto& xgp = gCellPatterns.at(0);
        const auto& ygp = gCellPatterns.at(1);
        const long long gcell_spacing_x
            = static_cast<long long>(xgp.getSpacing());
        const long long gcell_spacing_y
            = static_cast<long long>(ygp.getSpacing());
        const long long gcell_start_x
            = static_cast<long long>(xgp.getStartCoord());
        const long long gcell_start_y
            = static_cast<long long>(ygp.getStartCoord());
        const long long stride_x
            = static_cast<long long>(args.size) * gcell_spacing_x;
        const long long stride_y
            = static_cast<long long>(args.size) * gcell_spacing_y;
        const long long origin_x
            = gcell_start_x
              + static_cast<long long>(args.offset) * gcell_spacing_x;
        const long long origin_y
            = gcell_start_y
              + static_cast<long long>(args.offset) * gcell_spacing_y;
        const long long band = static_cast<long long>(
            std::lround(adaptive_opts.bbh_band_width_dbu));
        if (stride_x > 0 && stride_y > 0) {
          for (const auto& m : markers_list) {
            if (m == nullptr) {
              continue;
            }
            const odb::Rect bb = m->getBBox();
            const long long cx
                = (static_cast<long long>(bb.xMin())
                   + static_cast<long long>(bb.xMax()))
                  / 2;
            const long long cy
                = (static_cast<long long>(bb.yMin())
                   + static_cast<long long>(bb.yMax()))
                  / 2;
            // Distance from (cx, cy) to the nearest worker-grid line.
            // Positive modulo across all signs.
            auto pos_mod = [](long long a, long long b) {
              long long r = a % b;
              return r < 0 ? r + b : r;
            };
            const long long mx = pos_mod(cx - origin_x, stride_x);
            const long long my = pos_mod(cy - origin_y, stride_y);
            const long long dx_to_grid = std::min(mx, stride_x - mx);
            const long long dy_to_grid = std::min(my, stride_y - my);
            const bool is_stitch
                = std::min(dx_to_grid, dy_to_grid) <= band;
            if (is_stitch) {
              adaptive_marker_model_->observeStitchMarker(*m);
            }
          }
        }
      }
    }

    adaptive_marker_model_->endOuterIter();
  }
  debugPrint(logger_,
             utl::DRT,
             "workers",
             1,
             "Number of work units = {}.",
             numWorkUnits_);
  reportIterationViolations();
  if (router_cfg_->VERBOSE > 0) {
    iter_prog.time.print(logger_);
    std::cout << std::flush;
  }
  end();
}

void FlexDR::end(bool done)
{
  if (done) {
    router_->reportDRC(
        router_cfg_->DRC_RPT_FILE, design_->getTopBlock()->getMarkers(), "DRC");
  }
  if (done && router_cfg_->VERBOSE > 0) {
    logger_->info(DRT, 198, "Complete detail routing.");
  }

  using ULL = uint64_t;
  const auto size = getTech()->getLayers().size();
  std::vector<ULL> wlen(size, 0);
  std::vector<ULL> sCut(size, 0);
  std::vector<ULL> mCut(size, 0);
  auto topBlock = getDesign()->getTopBlock();
  for (auto& net : topBlock->getNets()) {
    for (auto& shape : net->getShapes()) {
      if (shape->typeId() == frcPathSeg) {
        auto obj = static_cast<frPathSeg*>(shape.get());
        auto [bp, ep] = obj->getPoints();
        auto lNum = obj->getLayerNum();
        frCoord psLen = odb::Point::manhattanDistance(ep, bp);
        wlen[lNum] += psLen;
      }
    }
    for (auto& via : net->getVias()) {
      auto lNum = via->getViaDef()->getCutLayerNum();
      if (via->getViaDef()->isMultiCut()) {
        ++mCut[lNum];
      } else {
        ++sCut[lNum];
      }
    }
  }

  const ULL totWlen = std::accumulate(wlen.begin(), wlen.end(), ULL(0));
  const ULL totSCut = std::accumulate(sCut.begin(), sCut.end(), ULL(0));
  const ULL totMCut = std::accumulate(mCut.begin(), mCut.end(), ULL(0));

  if (done) {
    logger_->metric("route__drc_errors", topBlock->getNumMarkers());
    logger_->metric("route__wirelength", totWlen / topBlock->getDBUPerUU());
    logger_->metric("route__vias", totSCut + totMCut);
    logger_->metric("route__vias__singlecut", totSCut);
    logger_->metric("route__vias__multicut", totMCut);
  } else {
    logger_->metric(fmt::format("route__drc_errors__iter:{}", iter_),
                    topBlock->getNumMarkers());
    logger_->metric(fmt::format("route__wirelength__iter:{}", iter_),
                    totWlen / topBlock->getDBUPerUU());
  }

  if (router_cfg_->VERBOSE > 0) {
    logger_->report("Total wire length = {} um.",
                    totWlen / topBlock->getDBUPerUU());

    for (int i = getTech()->getBottomLayerNum();
         i <= getTech()->getTopLayerNum();
         i++) {
      if (getTech()->getLayer(i)->getType() == dbTechLayerType::ROUTING) {
        logger_->report("Total wire length on LAYER {} = {} um.",
                        getTech()->getLayer(i)->getName(),
                        wlen[i] / topBlock->getDBUPerUU());
      }
    }
    logger_->report("Total number of vias = {}.", totSCut + totMCut);
    if (totMCut > 0) {
      logger_->report("Total number of multi-cut vias = {} ({:5.1f}%).",
                      totMCut,
                      totMCut * 100.0 / (totSCut + totMCut));
      logger_->report("Total number of single-cut vias = {} ({:5.1f}%).",
                      totSCut,
                      totSCut * 100.0 / (totSCut + totMCut));
    }
    logger_->report("Up-via summary (total {}):", totSCut + totMCut);
    int nameLen = 0;
    for (int i = getTech()->getBottomLayerNum();
         i <= getTech()->getTopLayerNum();
         i++) {
      if (getTech()->getLayer(i)->getType() == dbTechLayerType::CUT) {
        nameLen = std::max(nameLen,
                           (int) getTech()->getLayer(i - 1)->getName().size());
      }
    }
    int maxL = 1 + nameLen + 4 + (int) std::to_string(totSCut).length();
    if (totMCut) {
      maxL += 9 + 4 + (int) std::to_string(totMCut).length() + 9 + 4
              + (int) std::to_string(totSCut + totMCut).length();
    }
    std::ostringstream msg;
    if (totMCut) {
      msg << " "
          << std::setw(nameLen + 4 + (int) std::to_string(totSCut).length() + 9)
          << "single-cut";
      msg << std::setw(4 + (int) std::to_string(totMCut).length() + 9)
          << "multi-cut"
          << std::setw(4 + (int) std::to_string(totSCut + totMCut).length())
          << "total";
    }
    msg << std::endl;
    for (int i = 0; i < maxL; i++) {
      msg << "-";
    }
    msg << std::endl;
    for (int i = getTech()->getBottomLayerNum();
         i <= getTech()->getTopLayerNum();
         i++) {
      if (getTech()->getLayer(i)->getType() == dbTechLayerType::CUT) {
        msg << " " << std::setw(nameLen)
            << getTech()->getLayer(i - 1)->getName() << "    "
            << std::setw((int) std::to_string(totSCut).length()) << sCut[i];
        if (totMCut) {
          msg << " (" << std::setw(5)
              << (double) ((sCut[i] + mCut[i])
                               ? sCut[i] * 100.0 / (sCut[i] + mCut[i])
                               : 0.0)
              << "%)";
          msg << "    " << std::setw((int) std::to_string(totMCut).length())
              << mCut[i] << " (" << std::setw(5)
              << (double) ((sCut[i] + mCut[i])
                               ? mCut[i] * 100.0 / (sCut[i] + mCut[i])
                               : 0.0)
              << "%)    "
              << std::setw((int) std::to_string(totSCut + totMCut).length())
              << sCut[i] + mCut[i];
        }
        msg << std::endl;
      }
    }
    for (int i = 0; i < maxL; i++) {
      msg << "-";
    }
    msg << std::endl;
    msg << " " << std::setw(nameLen) << "    "
        << std::setw((int) std::to_string(totSCut).length()) << totSCut;
    if (totMCut) {
      msg << " (" << std::setw(5)
          << ((totSCut + totMCut) ? totSCut * 100.0 / (totSCut + totMCut) : 0.0)
          << "%)";
      msg << "    " << std::setw((int) std::to_string(totMCut).length())
          << totMCut << " (" << std::setw(5)
          << ((totSCut + totMCut) ? totMCut * 100.0 / (totSCut + totMCut) : 0.0)
          << "%)    "
          << std::setw((int) std::to_string(totSCut + totMCut).length())
          << totSCut + totMCut;
    }
    msg << std::endl << std::endl;
    logger_->report("{}", msg.str());
  }
  // P4.0 BoundaryDiag — flush + close both CSVs at FlexDR::end. Safe to
  // call when env var unset (no-op when not enabled).
  BoundaryDiagDump::instance().closeOnEnd();
  MarkerDiagDump::instance().closeOnEnd();
}

std::vector<FlexDR::SearchRepairArgs> strategy(const frUInt4 shapeCost,
                                               const frUInt4 markerCost)
{
  // clang-format off
  return {
    {7,  0,  3,      shapeCost,               0,       shapeCost, 0.950, RipUpMode::ALL    ,  true}, //  0
    {7, -2,  3,      shapeCost,       shapeCost,       shapeCost, 0.950, RipUpMode::ALL    ,  true}, //  1
    {7, -5,  3,      shapeCost,       shapeCost,       shapeCost, 0.950, RipUpMode::ALL    ,  true}, //  2
    {7,  0,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  3
    {7, -1,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  4
    {7, -2,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  5
    {7, -3,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  6
    {7, -4,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  7
    {7, -5,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  8
    {7, -6,  8,      shapeCost,      markerCost,   2 * shapeCost, 0.950, RipUpMode::DRC    , false}, //  9
    {7,  0,  8,  2 * shapeCost,      markerCost,   3 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 10
    {7, -1,  8,  2 * shapeCost,      markerCost,   3 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 11
    {7, -2,  8,  2 * shapeCost,      markerCost,   3 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 12
    {7, -3,  8,  2 * shapeCost,      markerCost,   3 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 13
    {7, -4,  8,  2 * shapeCost,      markerCost,   3 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 14
    {7, -5,  8,  2 * shapeCost,      markerCost,   4 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 15
    {7, -6,  8,  2 * shapeCost,      markerCost,   4 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 16
    {7, -3,  8,      shapeCost,      markerCost,   4 * shapeCost, 0.950, RipUpMode::ALL    , false}, // 17
    {7,  0,  8,  4 * shapeCost,      markerCost,   4 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 18
    {7, -1,  8,  4 * shapeCost,      markerCost,   4 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 19
    {7, -2,  8,  4 * shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 20
    {7, -3,  8,  4 * shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 21
    {7, -4,  8,  4 * shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 22
    {7, -5,  8,      shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::NEARDRC, false}, // 23
    {7, -6,  8,  4 * shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 24
    {5, -2,  8,      shapeCost,      markerCost,  10 * shapeCost, 0.950, RipUpMode::ALL    , false}, // 25
    {7,  0,  8,  8 * shapeCost,  2 * markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 26
    {7, -1,  8,  8 * shapeCost,  2 * markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 27
    {7, -2,  8,  8 * shapeCost,  2 * markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 28
    {7, -3,  8,  8 * shapeCost,  2 * markerCost,  10 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 29
    {7, -4,  8,      shapeCost,      markerCost,  50 * shapeCost, 0.950, RipUpMode::NEARDRC, false}, // 30
    {7, -5,  8,  8 * shapeCost,  2 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 31
    {7, -6,  8,  8 * shapeCost,  2 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 32
    {3, -1,  8,      shapeCost,      markerCost,  50 * shapeCost, 0.950, RipUpMode::ALL    , false}, // 33
    {7,  0,  8, 16 * shapeCost,  4 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 34
    {7, -1,  8, 16 * shapeCost,  4 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 35
    {7, -2,  8, 16 * shapeCost,  4 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 36
    {7, -3,  8,      shapeCost,      markerCost,  50 * shapeCost, 0.950, RipUpMode::NEARDRC, false}, // 37
    {7, -4,  8, 16 * shapeCost,  4 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 38
    {7, -5,  8, 16 * shapeCost,  4 * markerCost,  50 * shapeCost, 0.950, RipUpMode::DRC    , false}, // 39
    {7, -6,  8, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 40
    {3, -2,  8,      shapeCost,      markerCost, 100 * shapeCost, 0.990, RipUpMode::ALL    , false}, // 41
    {7,  0, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 42
    {7, -1, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 43
    {7, -2, 16,      shapeCost,      markerCost, 100 * shapeCost, 0.990, RipUpMode::NEARDRC, false}, // 44
    {7, -3, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 45
    {7, -4, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 46
    {7, -5, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 47
    {7, -6, 16, 16 * shapeCost,  4 * markerCost, 100 * shapeCost, 0.990, RipUpMode::DRC    , false}, // 48
    {3, -0,  8,      shapeCost,      markerCost, 100 * shapeCost, 0.990, RipUpMode::ALL    , false}, // 49
    {7,  0, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 50
    {7, -1, 32,      shapeCost,      markerCost, 100 * shapeCost, 0.999, RipUpMode::NEARDRC, false}, // 51
    {7, -2, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 52
    {7, -3, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 53
    {7, -4, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 54
    {7, -5, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 55
    {7, -6, 32, 32 * shapeCost,  8 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 56
    {3, -1,  8,      shapeCost,      markerCost, 100 * shapeCost, 0.999, RipUpMode::ALL    , false}, // 57
    {7,  0, 64,      shapeCost,      markerCost, 100 * shapeCost, 0.999, RipUpMode::NEARDRC, false}, // 58
    {7, -1, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 59
    {7, -2, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 60
    {7, -3, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 61
    {7, -4, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 62
    {7, -5, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}, // 63
    {7, -6, 64, 64 * shapeCost, 16 * markerCost, 100 * shapeCost, 0.999, RipUpMode::DRC    , false}  // 64
  };
  // clang-format on
}

void addRectToPolySet(gtl::polygon_90_set_data<frCoord>& polySet,
                      odb::Rect rect)
{
  gtl::polygon_90_data<frCoord> poly;
  std::vector<gtl::point_data<frCoord>> points;
  for (const auto& point : rect.getPoints()) {
    points.emplace_back(point.x(), point.y());
  }
  poly.set(points.begin(), points.end());
  using boost::polygon::operators::operator+=;
  polySet += poly;
}

void FlexDR::reportGuideCoverage()
{
  using boost::polygon::operators::operator&;

  const auto numLayers = getTech()->getLayers().size();
  std::vector<uint64_t> totalAreaByLayerNum(numLayers, 0);
  std::vector<uint64_t> totalCoveredAreaByLayerNum(numLayers, 0);
  std::map<frNet*, std::vector<float>> netsCoverage;
  const auto& nets = getDesign()->getTopBlock()->getNets();
  omp_set_num_threads(router_cfg_->MAX_THREADS);
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < nets.size(); i++) {  // NOLINT
    const auto& net = nets.at(i);
    std::vector<gtl::polygon_90_set_data<frCoord>> routeSetByLayerNum(
        numLayers),
        guideSetByLayerNum(numLayers);
    for (const auto& shape : net->getShapes()) {
      odb::Rect rect = shape->getBBox();
      addRectToPolySet(routeSetByLayerNum[shape->getLayerNum()], rect);
    }
    for (const auto& pwire : net->getPatchWires()) {
      odb::Rect rect = pwire->getBBox();
      addRectToPolySet(routeSetByLayerNum[pwire->getLayerNum()], rect);
    }
    for (const auto& via : net->getVias()) {
      {
        odb::Rect rect = via->getLayer1BBox();
        addRectToPolySet(routeSetByLayerNum[via->getViaDef()->getLayer1Num()],
                         rect);
      }
      {
        odb::Rect rect = via->getLayer2BBox();
        addRectToPolySet(routeSetByLayerNum[via->getViaDef()->getLayer2Num()],
                         rect);
      }
    }

    for (const auto& shape : net->getOrigGuides()) {
      odb::Rect rect = shape.getBBox();
      addRectToPolySet(guideSetByLayerNum[shape.getLayerNum()], rect);
    }

    for (frLayerNum lNum = 0; lNum < numLayers; lNum++) {
      if (getTech()->getLayer(lNum)->getType() != dbTechLayerType::ROUTING
          || lNum > router_cfg_->TOP_ROUTING_LAYER) {
        continue;
      }
      float coveredPercentage = -1.0;
      uint64_t routingArea = 0;
      uint64_t coveredArea = 0;
      if (!routeSetByLayerNum[lNum].empty()) {
        routingArea = gtl::area(routeSetByLayerNum[lNum]);
        coveredArea
            = gtl::area(routeSetByLayerNum[lNum] & guideSetByLayerNum[lNum]);
        if (routingArea == 0) {
          coveredPercentage = -1.0;
        } else {
          coveredPercentage = (coveredArea / (double) routingArea) * 100;
        }
      }

#pragma omp critical
      {
        netsCoverage[net.get()].push_back(coveredPercentage);
        totalAreaByLayerNum[lNum] += routingArea;
        totalCoveredAreaByLayerNum[lNum] += coveredArea;
      }
    }
  }

  std::ofstream file(router_cfg_->GUIDE_REPORT_FILE);
  file << "Net,";
  for (const auto& layer : getTech()->getLayers()) {
    if (layer->getType() == dbTechLayerType::ROUTING
        && layer->getLayerNum() <= router_cfg_->TOP_ROUTING_LAYER) {
      file << layer->getName() << ",";
    }
  }
  file << std::endl;
  for (const auto& [net, coverage] : netsCoverage) {
    file << net->getName() << ",";
    for (auto coveredPercentage : coverage) {
      if (coveredPercentage < 0.0) {
        file << "NA,";
      } else {
        file << fmt::format("{:.2f}%,", coveredPercentage);
      }
    }
    file << std::endl;
  }
  file << "Total,";
  uint64_t totalArea = 0;
  uint64_t totalCoveredArea = 0;
  for (const auto& layer : getTech()->getLayers()) {
    if (layer->getType() == dbTechLayerType::ROUTING
        && layer->getLayerNum() <= router_cfg_->TOP_ROUTING_LAYER) {
      if (totalAreaByLayerNum[layer->getLayerNum()] == 0) {
        file << "NA,";
        continue;
      }
      totalArea += totalAreaByLayerNum[layer->getLayerNum()];
      totalCoveredArea += totalCoveredAreaByLayerNum[layer->getLayerNum()];
      auto coveredPercentage
          = (totalCoveredAreaByLayerNum[layer->getLayerNum()]
             / (double) totalAreaByLayerNum[layer->getLayerNum()])
            * 100;
      file << fmt::format("{:.2f}%,", coveredPercentage);
    }
  }
  if (totalArea == 0) {
    file << "NA";
  } else {
    auto totalCoveredPercentage = (totalCoveredArea / (double) totalArea) * 100;
    file << fmt::format("{:.2f}%,", totalCoveredPercentage);
  }
  file.close();
}
void FlexDR::fixMaxSpacing()
{
  logger_->info(DRT, 227, "Checking For LEF58_MAXSPACING violations");
  io::Parser parser(db_, getDesign(), logger_, router_cfg_);
  parser.initSecondaryVias();
  std::vector<frVia*> lonely_vias;
  for (const auto& layer : getTech()->getLayers()) {
    if (layer->getType() == odb::dbTechLayerType::CUT) {
      if (!layer->hasLef58MaxSpacingConstraints()) {
        continue;
      }
      for (const auto& rule : layer->getLef58MaxSpacingConstraints()) {
        auto result = getLonelyVias(
            layer.get(), rule->getMaxSpacing(), rule->getCutClassIdx());
        lonely_vias.insert(lonely_vias.end(), result.begin(), result.end());
        for (const auto via_def : layer->getViaDefs()) {
          if (via_def->getCutClassIdx() == rule->getCutClassIdx()) {
            router_->addAvoidViaDefPA(via_def);
          }
        }
      }
    }
  }
  std::vector<odb::Rect> lonely_vias_regions;
  lonely_vias_regions.reserve(lonely_vias.size());
  for (const auto& via : lonely_vias) {
    // Create LEF58_MAXSPACING Markers for the lonely vias
    auto marker = std::make_unique<frMarker>();
    marker->setBBox(via->getBBox());
    auto layer = getTech()->getLayer(via->getViaDef()->getCutLayerNum());
    marker->setLayerNum(layer->getLayerNum());
    marker->setConstraint(layer->getLef58MaxSpacingConstraints().at(0));
    marker->addSrc(via->getNet());
    marker->addVictim(
        via->getNet(),
        std::make_tuple(layer->getLayerNum(), via->getBBox(), false));
    marker->addAggressor(
        via->getNet(),
        std::make_tuple(layer->getLayerNum(), via->getBBox(), false));
    getRegionQuery()->addMarker(marker.get());
    getDesign()->getTopBlock()->addMarker(std::move(marker));
    // Define the lonely via region needed for drWorker creation
    // The region is of size 3x3 GCELLBOX with the via in the center GCELLBOX
    auto origin = via->getOrigin();
    auto block = getDesign()->getTopBlock();
    auto gcell_idx = block->getGCellIdx(origin);
    odb::Rect region, tmp_box;
    tmp_box = block->getGCellBox({gcell_idx.x() - 1, gcell_idx.y() - 1});
    region.set_xlo(tmp_box.xMin());
    region.set_ylo(tmp_box.yMin());
    tmp_box = block->getGCellBox({gcell_idx.x() + 1, gcell_idx.y() + 1});
    region.set_xhi(tmp_box.xMax());
    region.set_yhi(tmp_box.yMax());
    lonely_vias_regions.emplace_back(region);
    if (via->isBottomConnected() || via->isTopConnected()) {
      // get pins connected to the via
      frRegionQuery::Objects<frBlockObject> result;
      getRegionQuery()->query(
          via->isTopConnected() ? via->getLayer2BBox() : via->getLayer1BBox(),
          via->isTopConnected() ? via->getViaDef()->getLayer2Num()
                                : via->getViaDef()->getLayer1Num(),
          result);
      for (auto& [bx, obj] : result) {
        if (obj->typeId() == frcInstTerm) {
          auto inst_term = static_cast<frInstTerm*>(obj);
          if (inst_term->getNet() != via->getNet()) {
            continue;
          }
          inst_term->setStubborn(true);
          router_->addInstancePAData(inst_term->getInst());
        }
      }
    }
  }
  router_->updateDirtyPAData();
  // merge intersecting regions
  std::sort(lonely_vias_regions.begin(),
            lonely_vias_regions.end(),
            [](const odb::Rect& a, const odb::Rect& b) {
              return a.xMin() < b.xMin();
            });
  std::vector<odb::Rect> merged_regions;
  for (const auto& region : lonely_vias_regions) {
    bool found = false;
    for (auto& merged_region : merged_regions) {
      if (region.intersects(merged_region)) {
        merged_region.merge(region);
        found = true;
        break;
      }
    }
    if (!found) {
      merged_regions.emplace_back(region);
    }
  }
  // create drWorkers for the final regions
  omp_set_num_threads(router_cfg_->MAX_THREADS);
#pragma omp parallel for schedule(dynamic)
  for (size_t i = 0; i < merged_regions.size(); i++) {
    auto route_box = merged_regions.at(i);
    auto worker = std::make_unique<FlexDRWorker>(
        &via_data_, design_, logger_, router_cfg_);
    odb::Rect ext_box;
    odb::Rect drc_box;
    route_box.bloat(router_cfg_->MTSAFEDIST, ext_box);
    route_box.bloat(router_cfg_->DRCSAFEDIST, drc_box);
    worker->setRouteBox(route_box);
    worker->setExtBox(ext_box);
    worker->setDrcBox(drc_box);
    worker->setDRIter(64);
    worker->setDebugSettings(router_->getDebugSettings());
    worker->setRipupMode(RipUpMode::VIASWAP);
    worker->setGraphics(graphics_.get());
    worker->main(getDesign());
#pragma omp critical
    {
      worker->end(getDesign());
    }
  }
  flow_state_machine_->setFixingMaxSpacing(true);
}

std::vector<frVia*> FlexDR::getLonelyVias(frLayer* layer,
                                          int max_spc,
                                          int cut_class)
{
  std::vector<frVia*> lonely_vias;
  if (layer->getSecondaryViaDefs().empty()) {
    return lonely_vias;
  }
  auto vias = getRegionQuery()->getVias(layer->getLayerNum());
  std::vector<odb::Point> via_positions;
  via_positions.reserve(vias.size());
  for (auto [obj, box] : vias) {
    via_positions.emplace_back(box.xCenter(), box.yCenter());
  }
  KDTree tree(via_positions);
  std::vector<std::atomic_bool> visited(via_positions.size());
  std::fill(visited.begin(), visited.end(), false);
  std::set<int> isolated_via_nodes;
  omp_set_num_threads(router_cfg_->MAX_THREADS);
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < via_positions.size(); i++) {
    if (visited[i].load()) {
      continue;
    }
    visited[i].store(true);
    std::vector<int> neighbors = tree.radiusSearch(via_positions[i], max_spc);
    // Check if there are neighbors other than the point itself
    bool is_isolated = true;
    for (const auto& neighbor : neighbors) {
      if (neighbor != i) {
        visited[neighbor].store(true);
        is_isolated = false;
      }
    }
    if (is_isolated) {
#pragma omp critical
      {
        isolated_via_nodes.insert(i);
      }
    }
  }
  for (auto i : isolated_via_nodes) {
    auto obj = vias[i].first;
    if (obj->typeId() != frcVia) {
      continue;
    }
    auto via = static_cast<frVia*>(obj);
    auto net = via->getNet();
    if (net == nullptr || net->isFake() || net->isSpecial()) {
      continue;
    }
    if (via->getViaDef()->getCutClassIdx() != cut_class) {
      continue;
    }
    via->setIsLonely(true);
    lonely_vias.emplace_back(via);
  }
  return lonely_vias;
}

int FlexDR::main()
{
  ProfileTask profile("DR:main");
  auto reporter = logger_->progress()->startIterationReporting(
      "detailed routing", std::min(64, router_cfg_->END_ITERATION), {});

  init();
  frTime t;
  bool incremental = false;
  bool hasFixed = false;
  for (const auto& net : getDesign()->getTopBlock()->getNets()) {
    incremental |= net->hasInitialRouting();
    hasFixed |= net->isFixed();
    if (incremental && hasFixed) {
      break;
    }
  }
  for (auto& args :
       strategy(router_cfg_->ROUTESHAPECOST, router_cfg_->MARKERCOST)) {
    if (iter_ > router_cfg_->END_ITERATION) {
      break;
    }
    int clipSize = args.size;
    if (args.ripupMode != RipUpMode::ALL) {
      if (increaseClipsize_) {
        clipSizeInc_ += 2;
      } else {
        clipSizeInc_ = std::max((float) 0, clipSizeInc_ - 0.2f);
      }
      clipSize += std::min(router_cfg_->MAX_CLIPSIZE_INCREASE,
                           (int) round(clipSizeInc_));
    }
    args.size = clipSize;
    if (args.ripupMode == RipUpMode::ALL) {
      if (hasFixed || (incremental && iter_ <= 2)) {
        args.ripupMode = RipUpMode::INCR;
      }
    }
    searchRepair(args);
    if (getDesign()->getTopBlock()->getNumMarkers() == 0) {
      break;
    }
    if (logger_->debugCheck(DRT, "snapshot", 1)) {
      io::Writer writer(getDesign(), logger_);
      writer.updateDb(db_, router_cfg_, false, true);

      std::string snapshotPath = fmt::format(
          "{}/drt_iter{}.odb", router_->getDebugSettings()->snapshotDir, iter_);
      db_->write(utl::OutStreamHandler(snapshotPath.c_str(), true).getStream());
    }
    if (reporter->incrementProgress()) {
      break;
    }
    ++iter_;
  }

  end(/* done */ true);
  reporter->end(true);

  if (!router_cfg_->GUIDE_REPORT_FILE.empty()) {
    reportGuideCoverage();
  }
  if (router_cfg_->VERBOSE > 0) {
    t.print(logger_);
    std::cout << std::endl;
  }
  return 0;
}

void FlexDR::sendWorkers(
    const std::vector<std::pair<int, FlexDRWorker*>>& remote_batch,
    std::vector<std::unique_ptr<FlexDRWorker>>& batch)
{
  if (remote_batch.empty()) {
    return;
  }
  std::vector<std::pair<int, std::string>> workers;
  {
    ProfileTask task("DIST: SERIALIZE_BATCH");
    for (auto& [idx, worker] : remote_batch) {
      std::string workerStr;
      serializeWorker(worker, workerStr);
      workers.emplace_back(idx, workerStr);
    }
  }
  std::string remote_ip = dist_ip_;
  uint16_t remote_port = dist_port_;
  if (router_->getCloudSize() > 1) {
    dst::JobMessage msg(dst::JobMessage::kBalancer),
        result(dst::JobMessage::kNone);
    bool ok = dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
    if (!ok) {
      logger_->error(utl::DRT, 7461, "Balancer failed");
    } else {
      dst::BalancerJobDescription* desc
          = static_cast<dst::BalancerJobDescription*>(
              result.getJobDescription());
      remote_ip = desc->getWorkerIP();
      remote_port = desc->getWorkerPort();
    }
  }
  {
    dst::JobMessage msg(dst::JobMessage::kRouting),
        result(dst::JobMessage::kNone);
    std::unique_ptr<dst::JobDescription> desc
        = std::make_unique<RoutingJobDescription>();
    RoutingJobDescription* rjd
        = static_cast<RoutingJobDescription*>(desc.get());
    rjd->setWorkers(workers);
    rjd->setSharedDir(dist_dir_);
    rjd->setSendEvery(20);
    msg.setJobDescription(std::move(desc));
    ProfileTask task("DIST: SENDJOB");
    bool ok = dist_->sendJobMultiResult(
        msg, remote_ip.c_str(), remote_port, result);
    if (!ok) {
      logger_->error(utl::DRT, 500, "Sending worker {} failed");
    }
    for (const auto& one_desc : result.getAllJobDescriptions()) {
      RoutingJobDescription* result_desc
          = static_cast<RoutingJobDescription*>(one_desc.get());
      router_->addWorkerResults(result_desc->getWorkers());
    }
  }
}

bool FlexDR::SearchRepairArgs::isEqualIgnoringSizeAndOffset(
    const SearchRepairArgs& other) const
{
  return (mazeEndIter == other.mazeEndIter
          && workerDRCCost == other.workerDRCCost
          && workerMarkerCost == other.workerMarkerCost
          && workerFixedShapeCost == other.workerFixedShapeCost
          && std::fabs(workerMarkerDecay - other.workerMarkerDecay) < 1e-6
          && ripupMode == other.ripupMode && followGuide == other.followGuide);
}

template <class Archive>
void FlexDRWorker::serialize(Archive& ar, const unsigned int version)
{
  // // We always serialize before calling main on the work unit so various
  // // fields are empty and don't need to be serialized.  I skip these to
  // // save having to write lots of serializers that will never be called.
  // if (!apSVia_.empty() || !nets_.empty() || !owner2nets_.empty()
  //     || !rq_.isEmpty() || gcWorker_) {
  //   logger_->error(DRT, 999, "Can't serialize used worker");
  // }

  // The logger_, graphics_ and debugSettings_ are handled by the caller to
  // use the current ones.
  if (is_loading(ar)) {
    frDesign* design = ar.getDesign();
    design_ = design;
  }
  (ar) & routeBox_;
  (ar) & extBox_;
  (ar) & drcBox_;
  (ar) & drIter_;
  (ar) & mazeEndIter_;
  (ar) & followGuide_;
  (ar) & needRecheck_;
  (ar) & skipRouting_;
  (ar) & ripupMode_;
  (ar) & workerDRCCost_;
  (ar) & workerMarkerCost_;
  (ar) & workerFixedShapeCost_;
  (ar) & workerMarkerDecay_;
  (ar) & initNumMarkers_;
  (ar) & nets_;
  (ar) & markers_;
  (ar) & bestMarkers_;
  (ar) & isCongested_;
  if (is_loading(ar)) {
    // boundaryPin_
    int sz = 0;
    (ar) & sz;
    while (sz--) {
      frBlockObject* obj;
      serializeBlockObject(ar, obj);
      std::set<std::pair<odb::Point, frLayerNum>> val;
      (ar) & val;
      boundaryPin_[(frNet*) obj] = std::move(val);
    }
    // owner2nets_
    for (auto& net : nets_) {
      owner2nets_[net->getFrNet()].push_back(net.get());
    }
    dist_on_ = true;
  } else {
    // boundaryPin_
    int sz = boundaryPin_.size();
    (ar) & sz;
    for (auto& [net, value] : boundaryPin_) {
      frBlockObject* obj = (frBlockObject*) net;
      serializeBlockObject(ar, obj);
      (ar) & value;
    }
  }
}

std::unique_ptr<FlexDRWorker> FlexDRWorker::load(
    const std::string& workerStr,
    FlexDRViaData* via_data,
    frDesign* design,
    utl::Logger* logger,
    RouterConfiguration* router_cfg)
{
  auto worker
      = std::make_unique<FlexDRWorker>(via_data, design, logger, router_cfg);
  deserializeWorker(worker.get(), design, workerStr);
  return worker;
}

// Explicit instantiations
template void FlexDRWorker::serialize<frIArchive>(
    frIArchive& ar,
    const unsigned int file_version);

template void FlexDRWorker::serialize<frOArchive>(
    frOArchive& ar,
    const unsigned int file_version);

}  // namespace drt
