// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// outer_loop_plus Patch 4 — ConstraintFieldBuilder.
//
// Walks a FlexDRWorker's local geometry and populates a ConstraintField
// for that worker. Worker-local — no global mutation, safe to run inside
// the OMP parallel-for. Designed-in -1 placeholders for the
// metadata it cannot easily extract yet so future phases can fill them
// without churning the call sites.

#pragma once

#include <chrono>

#include "dr/ConstraintField.h"
#include "dr/ConstraintFieldTypes.h"

namespace utl {
class Logger;
}

namespace drt {

class FlexDRWorker;
class frBlock;
class frDesign;

class ConstraintFieldBuilder
{
 public:
  ConstraintFieldBuilder(const ConstraintFieldPolicy& policy,
                         utl::Logger* logger);

  // Build a worker-scoped ConstraintField. Phase 4.1: iterates the
  // worker's drcBox shapes/vias to populate ONLY the stats (count
  // seen, build wall, memory). No tile splats. Phase 4.2 adds:
  //   - via/cut splats with spacing-kernel inflation
  // Phase 4.3 will add planar metal spacing dilation.
  void build(ConstraintField& field,
             FlexDRWorker* worker,
             frDesign* design,
             int iter,
             int worker_id,
             int batch_id);

 private:
  ConstraintFieldPolicy policy_;
  utl::Logger* logger_ = nullptr;
};

}  // namespace drt
