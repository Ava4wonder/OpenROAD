// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.b — extract ClipRecord from upstream gcNet + frMarker state.

#include "GcWorkerClipBuilder.h"

#include <boost/polygon/polygon.hpp>

#include "db/gcObj/gcNet.h"
#include "db/obj/frMarker.h"

namespace drt::redesign::legality {

namespace gtl = boost::polygon;

namespace {

bool BBoxIntersect(std::int32_t ax1,
                   std::int32_t ay1,
                   std::int32_t ax2,
                   std::int32_t ay2,
                   const odb::Rect& b)
{
  return !(ax2 < b.xMin() || b.xMax() < ax1 || ay2 < b.yMin()
           || b.yMax() < ay1);
}

}  // namespace

ClipRecord BuildClipRecord(
    const std::vector<std::unique_ptr< ::drt::gcNet >>& nets,
    const std::vector<std::unique_ptr< ::drt::frMarker >>& markers,
    const odb::Rect& drc_box,
    const odb::Rect& ext_box,
    ::drt::frLayerNum min_layer_num,
    ::drt::frLayerNum max_layer_num,
    std::uint64_t clip_id,
    const std::string& design,
    const std::string& pdk)
{
  ClipRecord rec;
  rec.meta.clip_id = clip_id;
  rec.meta.design = design;
  rec.meta.pdk = pdk;
  rec.meta.tech_hash = 0;
  rec.meta.rule_deck_fingerprint = 0;
  rec.meta.clip_x1 = ext_box.xMin();
  rec.meta.clip_y1 = ext_box.yMin();
  rec.meta.clip_x2 = ext_box.xMax();
  rec.meta.clip_y2 = ext_box.yMax();
  rec.meta.route_x1 = drc_box.xMin();
  rec.meta.route_y1 = drc_box.yMin();
  rec.meta.route_x2 = drc_box.xMax();
  rec.meta.route_y2 = drc_box.yMax();

  // ----- Layer A: gather all gcNet rectangles intersecting drc_box -----
  for (const auto& net : nets) {
    // net_id 0 reserved for "blockage / fixed metal" per the predicate
    // semantics in Predicates.cpp; non-zero == per-clip-local sequential
    // identifier.
    const std::uint64_t net_id
        = net->isBlockage() ? 0u
                            : static_cast<std::uint64_t>(net->getId()) + 1u;
    for (auto layer_num = min_layer_num; layer_num <= max_layer_num;
         ++layer_num) {
      for (const bool is_fixed : {true, false}) {
        const auto& rects = net->getRectangles(layer_num, is_fixed);
        for (const auto& r : rects) {
          const std::int32_t x1 = static_cast<std::int32_t>(gtl::xl(r));
          const std::int32_t y1 = static_cast<std::int32_t>(gtl::yl(r));
          const std::int32_t x2 = static_cast<std::int32_t>(gtl::xh(r));
          const std::int32_t y2 = static_cast<std::int32_t>(gtl::yh(r));
          if (!BBoxIntersect(x1, y1, x2, y2, drc_box)) {
            continue;
          }
          Shape s;
          s.x1 = x1;
          s.y1 = y1;
          s.x2 = x2;
          s.y2 = y2;
          s.layer = static_cast<std::int16_t>(layer_num);
          s.net_id = is_fixed ? 0u : net_id;
          rec.candidates.push_back(s);
        }
      }
    }
  }

  rec.context.clear();
  rec.labels.assign(rec.candidates.size(), ProjectedUpstreamLabel{});

  // ----- Layer B: project markers onto candidates ------------------
  for (const auto& mk : markers) {
    const odb::Rect mk_bbox = mk->getBBox();
    std::vector<std::size_t> hits;
    hits.reserve(2);
    for (std::size_t i = 0; i < rec.candidates.size(); ++i) {
      const auto& s = rec.candidates[i];
      if (BBoxIntersect(s.x1, s.y1, s.x2, s.y2, mk_bbox)) {
        hits.push_back(i);
      }
    }
    for (const std::size_t idx : hits) {
      ++rec.labels[idx].projected_marker_count;
    }
    if (hits.size() > 1) {
      for (const std::size_t idx : hits) {
        rec.labels[idx].projection_ambiguous = 1;
      }
    }
  }
  for (auto& l : rec.labels) {
    l.projected_legal = l.projected_marker_count == 0 ? 1 : 0;
  }

  return rec;
}

}  // namespace drt::redesign::legality
