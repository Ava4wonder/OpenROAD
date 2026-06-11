// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// R2 (MCF-DRT) — see McfExport.h.

#include "dr/McfExport.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "db/obj/frBTerm.h"
#include "db/obj/frBlock.h"
#include "db/obj/frGuide.h"
#include "db/obj/frInstTerm.h"
#include "db/obj/frNet.h"
#include "frDesign.h"
#include "frRegionQuery.h"
#include "utl/Logger.h"

namespace drt::mcf {

int exportInstance(frDesign* design,
                   const std::string& dir,
                   utl::Logger* logger)
{
  auto* block = design->getTopBlock();
  auto* tech = design->getTech();
  const auto gcps = block->getGCellPatterns();
  if (gcps.size() < 2) {
    logger->warn(utl::DRT, 9301, "[mcf] no GCell patterns; skip export");
    return 0;
  }
  const auto& xgp = gcps[0];
  const auto& ygp = gcps[1];

  // --- grid + layers -------------------------------------------------
  std::ofstream grid(dir + "/mcf_grid.txt");
  grid << xgp.getCount() << " " << ygp.getCount() << " "
       << xgp.getStartCoord() << " " << ygp.getStartCoord() << " "
       << xgp.getSpacing() << " " << ygp.getSpacing() << "\n";
  for (const auto& layer : tech->getLayers()) {
    if (layer->getType() != odb::dbTechLayerType::ROUTING) {
      continue;
    }
    const bool routable = layer->isRoutable();
    const char dirc
        = (layer->getDir() == odb::dbTechLayerDir::HORIZONTAL) ? 'H' : 'V';
    grid << "L " << layer->getLayerNum() << " " << (routable ? 1 : 0)
         << " " << dirc << " " << layer->getPitch() << "\n";
  }
  grid.close();

  // --- blockage-aware edge capacities (R3.5b) -------------------------
  // Per GCell-boundary capacity = tracks crossing the boundary minus
  // tracks under fixed shapes (pins, obstructions, PG) intersecting a
  // half-pitch band around it. Edge order matches drt_load.build_graph:
  // x-major (gx outer, gy inner), H layers = +x edges, V layers = +y.
  {
    auto* rq = design->getRegionQuery();
    std::ofstream caps(dir + "/mcf_caps.txt");
    const int nx = xgp.getCount();
    const int ny = ygp.getCount();
    const int xs = xgp.getStartCoord();
    const int ys = ygp.getStartCoord();
    const int dxs = xgp.getSpacing();
    const int dys = ygp.getSpacing();
    frRegionQuery::Objects<frBlockObject> objs;
    std::vector<std::pair<int, int>> iv;
    for (const auto& layer : tech->getLayers()) {
      if (layer->getType() != odb::dbTechLayerType::ROUTING
          || !layer->isRoutable()) {
        continue;
      }
      const int pitch = std::max(1, static_cast<int>(layer->getPitch()));
      const int half = pitch / 2;
      const bool horiz
          = layer->getDir() == odb::dbTechLayerDir::HORIZONTAL;
      const frLayerNum ln = layer->getLayerNum();
      const int64_t n_edges = horiz ? static_cast<int64_t>(nx - 1) * ny
                                    : static_cast<int64_t>(nx) * (ny - 1);
      caps << "CAPS " << ln << " " << n_edges << "\n";
      const int gx_end = horiz ? nx - 1 : nx;
      const int gy_end = horiz ? ny : ny - 1;
      for (int gx = 0; gx < gx_end; ++gx) {
        for (int gy = 0; gy < gy_end; ++gy) {
          int lo, hi, base;
          odb::Rect qbox;
          if (horiz) {  // +x edge: vertical boundary, horizontal tracks
            const int bx = xs + (gx + 1) * dxs;
            lo = ys + gy * dys;
            hi = lo + dys;
            base = std::max(1, dys / pitch);
            qbox = odb::Rect(bx - half, lo, bx + half, hi);
          } else {  // +y edge: horizontal boundary, vertical tracks
            const int by = ys + (gy + 1) * dys;
            lo = xs + gx * dxs;
            hi = lo + dxs;
            base = std::max(1, dxs / pitch);
            qbox = odb::Rect(lo, by - half, hi, by + half);
          }
          objs.clear();
          rq->query(qbox, ln, objs);
          int cap = base;
          if (!objs.empty()) {
            iv.clear();
            for (const auto& [r, obj] : objs) {
              const int a = std::max(horiz ? r.yMin() : r.xMin(), lo);
              const int b = std::min(horiz ? r.yMax() : r.xMax(), hi);
              if (a < b) {
                iv.emplace_back(a, b);
              }
            }
            if (!iv.empty()) {
              std::sort(iv.begin(), iv.end());
              int64_t blocked = 0;
              int ca = iv[0].first, cb = iv[0].second;
              for (size_t i = 1; i < iv.size(); ++i) {
                if (iv[i].first > cb) {
                  blocked += cb - ca;
                  ca = iv[i].first;
                  cb = iv[i].second;
                } else {
                  cb = std::max(cb, iv[i].second);
                }
              }
              blocked += cb - ca;
              cap = std::max(
                  0, base - static_cast<int>(blocked / pitch));
            }
          }
          caps << cap << ' ';
        }
      }
      caps << "\n";
    }
  }

  // --- nets + guides --------------------------------------------------
  std::ofstream nets(dir + "/mcf_nets.txt");
  std::ofstream names(dir + "/mcf_netnames.txt");
  std::ofstream guides(dir + "/mcf_guides.txt");
  int net_idx = 0;
  int skipped = 0;
  for (const auto& net : block->getNets()) {
    const auto& gs = net->getGuides();
    // pin GCells: instTerms + bterms, dedup'd
    std::vector<std::pair<int, int>> pins;
    auto add_pin = [&](const odb::Rect& bb) {
      const odb::Point center((bb.xMin() + bb.xMax()) / 2,
                              (bb.yMin() + bb.yMax()) / 2);
      const odb::Point idx = block->getGCellIdx(center);
      const std::pair<int, int> p(idx.x(), idx.y());
      for (const auto& q : pins) {
        if (q == p) {
          return;
        }
      }
      pins.push_back(p);
    };
    for (auto* it : net->getInstTerms()) {
      add_pin(it->getBBox());
    }
    for (auto* bt : net->getBTerms()) {
      add_pin(bt->getBBox());
    }
    if (pins.size() < 2 || gs.empty()) {
      ++skipped;  // single-GCell or guide-less nets need no MCF routing
      continue;
    }
    nets << pins.size();
    for (const auto& [gx, gy] : pins) {
      nets << " " << gx << " " << gy;
    }
    nets << "\n";
    names << net->getName() << "\n";
    for (const auto& g : gs) {
      const auto [bp, ep] = g->getPoints();
      const odb::Point bi = block->getGCellIdx(bp);
      const odb::Point ei = block->getGCellIdx(ep);
      const int zlo = std::min(g->getBeginLayerNum(), g->getEndLayerNum());
      const int zhi = std::max(g->getBeginLayerNum(), g->getEndLayerNum());
      guides << net_idx << " " << std::min(bi.x(), ei.x()) << " "
             << std::min(bi.y(), ei.y()) << " " << std::max(bi.x(), ei.x())
             << " " << std::max(bi.y(), ei.y()) << " " << zlo << " " << zhi
             << "\n";
    }
    ++net_idx;
  }
  logger->report(
      "[mcf] exported {} nets ({} skipped: <2 pin-gcells or no guides) "
      "grid {}x{} to {}",
      net_idx, skipped, xgp.getCount(), ygp.getCount(), dir);
  return net_idx;
}

}  // namespace drt::mcf
