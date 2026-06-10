// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// R2 (MCF-DRT) — see McfExport.h.

#include "dr/McfExport.h"

#include <fstream>

#include "db/obj/frBTerm.h"
#include "db/obj/frBlock.h"
#include "db/obj/frGuide.h"
#include "db/obj/frInstTerm.h"
#include "db/obj/frNet.h"
#include "frDesign.h"
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
