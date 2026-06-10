// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// R2 (MCF-DRT) — export the routing instance FlexDR sees as a
// multicommodity-flow problem for the GPU PDHG solver (mcr_sparse.py).
// See /Users/ava/OpenROAD/MCF_DRT_refactor_plan.md.
//
// Activation: env OPENROAD_DRT_MCF_EXPORT=<dir>, hooked at the top of
// FlexDR::main() after init(). Text format, three files:
//   mcf_grid.txt   — GCell grid dims/origin/spacing + per-layer
//                    (layerNum, routable, dir, pitch)
//   mcf_nets.txt   — one line per exported net: numPins gx gy gx gy …
//   mcf_netnames.txt — net name per line (same order; for R3 wiring)
//   mcf_guides.txt — netIdx gxlo gylo gxhi gyhi zlo zhi (guide rects
//                    in GCell coords; zlo/zhi are frLayerNums)

#pragma once

#include <string>

namespace utl {
class Logger;
}

namespace drt {
class frDesign;

namespace mcf {

// Returns number of nets exported (0 on failure).
int exportInstance(frDesign* design,
                   const std::string& dir,
                   utl::Logger* logger);

}  // namespace mcf
}  // namespace drt
