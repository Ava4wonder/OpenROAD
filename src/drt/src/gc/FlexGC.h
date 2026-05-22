// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#pragma once

#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include "db/obj/frBlockObject.h"
#include "db/obj/frMarker.h"
#include "db/tech/frTechObject.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "global.h"
#include "utl/Logger.h"

namespace drt {
class drNet;
class drPatchWire;
class FlexDRWorker;
class gcNet;
class gcPin;

// outer_loop_plus profiling — per-FlexGCWorker phase-wall accumulator.
// Each FlexGCWorker::Impl::main() invocation adds its per-phase chrono
// deltas into this struct. Default-OFF; only populated when the FlexDR-
// side env var is set (the chrono wrappers are always live but cheap).
struct FlexGCStats
{
  // Each FlexGCWorker::Impl::main() bumps call_count once + adds time
  // into the relevant per-phase doubles (milliseconds).
  int call_count = 0;
  double total_ms = 0.0;
  double update_ms = 0.0;
  double surg_metal_shape_ms = 0.0;
  double patch_metal_shape_ms = 0.0;
  double metal_corner_spacing_ms = 0.0;
  double metal_spacing_ms = 0.0;
  double metal_shape_ms = 0.0;
  double metal_eol_ms = 0.0;
  double cut_spacing_ms = 0.0;
  double metal_spacing_table_influence_ms = 0.0;
  double minimum_cut_ms = 0.0;
  double metal_width_via_table_ms = 0.0;
  double modify_markers_ms = 0.0;
  double normalize_marker_order_ms = 0.0;

  // EOL_GC.P.1 V1 — when OPENROAD_DRT_EOL_KERNELS=1, the kernel
  // code path bumps eol_kernel_calls + eol_kernels_ms so we can
  // verify the path is taken and compare its wall to the baseline.
  // metal_eol_ms still accumulates the total (both paths) so the
  // header-level comparison stays meaningful.
  long eol_kernel_calls = 0;
  double eol_kernels_ms = 0.0;
};

class FlexGCWorker
{
 public:
  // constructors
  FlexGCWorker(frTechObject* techIn,
               utl::Logger* logger,
               RouterConfiguration* router_cfg,
               FlexDRWorker* drWorkerIn = nullptr);
  ~FlexGCWorker();
  // setters
  void setExtBox(const odb::Rect& in);
  void setDrcBox(const odb::Rect& in);
  bool setTargetNet(frBlockObject* in);
  bool setTargetNet(drNet* in);
  gcNet* getTargetNet();
  void resetTargetNet();
  void addTargetObj(frBlockObject* in);
  void setTargetObjs(const std::set<frBlockObject*>& targetObjs);
  void setIgnoreDB();
  void setIgnoreMinArea();
  void setIgnoreLongSideEOL();
  void setIgnoreCornerSpacing();
  void setEnableSurgicalFix(bool in);
  void addPAObj(frConnFig* obj, frBlockObject* owner);
  // getters
  std::vector<std::unique_ptr<gcNet>>& getNets();
  gcNet* getNet(frNet* net);
  frDesign* getDesign() const;
  const std::vector<std::unique_ptr<frMarker>>& getMarkers() const;
  const std::vector<std::unique_ptr<drPatchWire>>& getPWires() const;
  // others
  void init(const frDesign* design);
  int main();
  void clearPWires();
  // outer_loop_plus profiling — per-FlexGCWorker phase wall accumulator.
  const FlexGCStats& getStats() const;
  void resetStats();
  // initialization from FlexPA, initPA0 --> addPAObj --> initPA1
  void initPA0(const frDesign* design);
  void initPA1();
  void updateDRNet(drNet* net);
  // used in rp_prep
  void checkMinStep(gcPin* pin);
  void updateGCWorker();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
struct MarkerId
{
  odb::Rect box;
  frLayerNum lNum;
  frConstraint* con;
  frOrderedIdSet<frBlockObject*> srcs;
  bool operator<(const MarkerId& rhs) const
  {
    return std::tie(box, lNum, con, srcs)
           < std::tie(rhs.box, rhs.lNum, rhs.con, rhs.srcs);
  }
};
}  // namespace drt
