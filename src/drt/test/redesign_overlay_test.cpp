// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.c — unit tests for Footprint.h (DeltaId, WriteFootprint::Of,
// ReadFootprint multi-domain layout, and a synthetic OCC
// commit-eligibility check). Built only when both
// ENABLE_DRT_REDESIGN=ON and ENABLE_DRT_REDESIGN_OVERLAY=ON.

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "redesign/Footprint.h"
#include "redesign/overlay/GeometryView.h"
#include "redesign/overlay/MemoryBackedGeometryView.h"
#include "redesign/overlay/SnapshotHandle.h"

namespace r = drt::redesign;
namespace ro = drt::redesign::overlay;

namespace {

// Synthetic eligibility check exercising the multi-domain
// ReadFootprint API. V2.1 implementation is intentionally trivial —
// real conflict graphs land in V2.4. The point of this test is to
// prove the API surface is broad enough for V2.4 to layer on without
// schema churn.
bool ReadFootprintIntersectsCommittedWrites(
    const r::ReadFootprint& rf,
    const std::vector<r::Rect>& committed_write_rects,
    const std::vector<r::LayerNum>& committed_write_layers)
{
  // Geometry domain — same-layer bbox overlap check.
  for (size_t i = 0; i < rf.geometry.rects.size(); ++i) {
    const r::Rect& g = rf.geometry.rects[i];
    const r::LayerNum gl = rf.geometry.layers[i];
    for (size_t j = 0; j < committed_write_rects.size(); ++j) {
      if (gl != committed_write_layers[j]) {
        continue;
      }
      const r::Rect& w = committed_write_rects[j];
      const bool overlap = !(g.ur.x < w.ll.x || w.ur.x < g.ll.x
                             || g.ur.y < w.ll.y || w.ur.y < g.ll.y);
      if (overlap) {
        return true;
      }
    }
  }
  // Marker domain check intentionally elided in V2.1 — the committed-
  // write set is geometry-typed here; markers compare against future
  // MarkerWrites once the commit type is fleshed out in V2.2/V2.4.
  // What V2.1.c proves is that the struct slot exists and is reachable.
  (void) rf.markers;
  return false;
}

bool TestDeltaIdOrdering()
{
  r::DeltaId a{1, 0, 0};
  r::DeltaId b{1, 0, 1};
  r::DeltaId c{1, 1, 0};
  r::DeltaId d{2, 0, 0};
  if (!(a < b && b < c && c < d)) {
    return false;
  }
  if (!(a == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (a == b) {
    return false;
  }
  return true;
}

bool TestWriteFootprintOfAddWire()
{
  r::AddWire add;
  add.bbox = r::Rect{r::Point{10, 20}, r::Point{30, 40}};
  add.layer = 5;
  r::Delta d = add;
  r::WriteFootprint wf = r::WriteFootprint::Of(d);
  if (wf.unknown) {
    return false;  // AddWire footprint is sound — must not be unknown.
  }
  if (wf.shapes.size() != 1u || wf.layers.size() != 1u) {
    return false;
  }
  if (wf.shapes[0].ll.x != 10 || wf.shapes[0].ll.y != 20
      || wf.shapes[0].ur.x != 30 || wf.shapes[0].ur.y != 40) {
    return false;
  }
  return wf.layers[0] == 5;
}

bool TestWriteFootprintOfInsertShield()
{
  r::InsertShield s;
  s.coverage = r::Rect{r::Point{0, 0}, r::Point{100, 50}};
  s.layer = 7;
  r::Delta d = s;
  r::WriteFootprint wf = r::WriteFootprint::Of(d);
  if (wf.unknown) {
    return false;
  }
  if (wf.shapes.size() != 1u || wf.layers.size() != 1u) {
    return false;
  }
  return wf.shapes[0].ur.x == 100 && wf.layers[0] == 7;
}

bool TestWriteFootprintOfAddViaExpands()
{
  r::AddVia v;
  v.location = r::Point{500, 500};
  r::Delta d = v;
  r::WriteFootprint wf = r::WriteFootprint::Of(d);
  if (wf.unknown) {
    return false;
  }
  if (wf.shapes.size() != 1u) {
    return false;
  }
  // Bbox must be a non-zero region around the via location (V2.1 stub
  // expansion). V2.2.a will replace the default halo with the real
  // enclosure from the via_def.
  return wf.shapes[0].ur.x > wf.shapes[0].ll.x
         && wf.shapes[0].ur.y > wf.shapes[0].ll.y;
}

bool TestWriteFootprintOfDeleteWireIsUnknown()
{
  // SAFETY INVARIANT: V2.1 cannot produce a sound footprint for
  // DeleteWire without a design-side lookup. The empty (shapes,
  // layers) MUST be paired with unknown=true so V2.4's commit path
  // rejects this Delta from any parallel batch and forces a serial
  // fallback. An empty-with-unknown=false would be a real "writes
  // nothing" claim, which would be a safety bug for DeleteWire.
  r::DeleteWire del;
  del.segment_id = 42;
  r::Delta d = del;
  r::WriteFootprint wf = r::WriteFootprint::Of(d);
  return wf.unknown && wf.shapes.empty() && wf.layers.empty();
}

bool TestWriteFootprintOfAllUnimplementedKindsAreUnknown()
{
  // Same invariant for every design-lookup-dependent kind: V2.1 must
  // mark them unknown so the parallel-commit gate rejects them. This
  // test shrinks the surface area for the V2.4 pre-gate review:
  // before V2.4 lands, every false here is a kind that needs a sound
  // footprint or an explicit serial-fallback classification.
  r::DeleteVia dv;
  dv.via_id = 1;
  r::MoveCell mc;
  mc.inst = nullptr;
  r::ChangePinAccess cpa;
  cpa.iterm = nullptr;
  r::ChangeLayerAssignment cla;
  cla.segment_id = 7;
  cla.new_layer = 3;
  r::ResizeCell rc;
  rc.inst = nullptr;
  r::Delta deltas[] = {dv, mc, cpa, cla, rc};
  for (const auto& d : deltas) {
    r::WriteFootprint wf = r::WriteFootprint::Of(d);
    if (!wf.unknown) {
      return false;
    }
  }
  return true;
}

bool TestReadFootprintAllDomainsExist()
{
  // Populate each domain in turn — proves the multi-domain layout is
  // wired. V2.1 only fills geometry + markers in real proposers; V2.2+
  // fills the rest. The test exercises the API for all six.
  r::ReadFootprint rf;
  rf.geometry.rects.push_back(r::Rect{r::Point{0, 0}, r::Point{10, 10}});
  rf.geometry.layers.push_back(2);
  rf.topology.nets.push_back(7);
  rf.markers.rects.push_back(r::Rect{r::Point{5, 5}, r::Point{15, 15}});
  rf.guides.rects.push_back(r::Rect{r::Point{0, 0}, r::Point{100, 100}});
  rf.guides.layers.push_back(3);
  rf.cost_fields.region = r::Rect{r::Point{0, 0}, r::Point{50, 50}};
  rf.cost_fields.fields_read
      = r::ReadFootprint::CostFieldDomain::HistoryCost
        | r::ReadFootprint::CostFieldDomain::Congestion;
  rf.pin_access.iterms.push_back(99);
  return rf.geometry.rects.size() == 1u && rf.topology.nets.size() == 1u
         && rf.markers.rects.size() == 1u && rf.guides.rects.size() == 1u
         && rf.cost_fields.fields_read != 0u
         && rf.pin_access.iterms.size() == 1u;
}

bool TestEligibilityIntersectionPositive()
{
  r::ReadFootprint rf;
  rf.geometry.rects.push_back(r::Rect{r::Point{0, 0}, r::Point{100, 100}});
  rf.geometry.layers.push_back(2);
  std::vector<r::Rect> committed = {
      r::Rect{r::Point{50, 50}, r::Point{60, 60}}};
  std::vector<r::LayerNum> committed_layers = {2};
  return ReadFootprintIntersectsCommittedWrites(rf, committed,
                                                committed_layers);
}

bool TestEligibilityIntersectionNegativeWrongLayer()
{
  r::ReadFootprint rf;
  rf.geometry.rects.push_back(r::Rect{r::Point{0, 0}, r::Point{100, 100}});
  rf.geometry.layers.push_back(2);
  std::vector<r::Rect> committed = {
      r::Rect{r::Point{50, 50}, r::Point{60, 60}}};
  std::vector<r::LayerNum> committed_layers = {3};  // different layer
  return !ReadFootprintIntersectsCommittedWrites(rf, committed,
                                                 committed_layers);
}

bool TestEligibilityIntersectionNegativeNoOverlap()
{
  r::ReadFootprint rf;
  rf.geometry.rects.push_back(r::Rect{r::Point{0, 0}, r::Point{10, 10}});
  rf.geometry.layers.push_back(2);
  std::vector<r::Rect> committed = {
      r::Rect{r::Point{1000, 1000}, r::Point{1010, 1010}}};
  std::vector<r::LayerNum> committed_layers = {2};
  return !ReadFootprintIntersectsCommittedWrites(rf, committed,
                                                 committed_layers);
}

// ===== V2.1.d — GeometryView / MemoryBackedGeometryView / SnapshotHandle =====

ro::MarkerRef MakeMarker(int x1, int y1, int x2, int y2,
                         std::optional<r::LayerNum> layer = std::nullopt,
                         std::optional<uint32_t> ctype
                         = std::nullopt)
{
  ro::MarkerRef m;
  m.bbox = r::Rect{r::Point{x1, y1}, r::Point{x2, y2}};
  m.layer = layer;
  m.constraint_type_id = ctype;
  return m;
}

bool TestMemoryBackedQueryMarkersReturnsOverlapping()
{
  std::vector<ro::MarkerRef> backing;
  backing.push_back(MakeMarker(0, 0, 10, 10, /*layer=*/2));
  backing.push_back(MakeMarker(50, 50, 60, 60, /*layer=*/3));
  backing.push_back(MakeMarker(8, 8, 20, 20, /*layer=*/2));
  ro::MemoryBackedGeometryView view(std::move(backing));

  auto out
      = view.QueryMarkers(r::Rect{r::Point{5, 5}, r::Point{15, 15}});
  // Two of the three markers overlap the query box.
  return out.size() == 2u;
}

bool TestMemoryBackedQueryMarkersEmptyOnEmptyBacking()
{
  ro::MemoryBackedGeometryView view({});
  auto out
      = view.QueryMarkers(r::Rect{r::Point{0, 0}, r::Point{1000, 1000}});
  return out.empty();
}

bool TestMemoryBackedQueryMarkersDistinguishesAbsentLayer()
{
  // Canonical hashing must distinguish "layer absent" from
  // "layer == 0". The MemoryBackedGeometryView round-trips both
  // verbatim.
  std::vector<ro::MarkerRef> backing;
  backing.push_back(MakeMarker(0, 0, 10, 10));  // layer absent
  backing.push_back(
      MakeMarker(0, 0, 10, 10, /*layer=*/r::LayerNum{0}));  // layer = 0
  ro::MemoryBackedGeometryView view(std::move(backing));
  auto out
      = view.QueryMarkers(r::Rect{r::Point{0, 0}, r::Point{20, 20}});
  if (out.size() != 2u) {
    return false;
  }
  // First has no layer; second has layer=0.
  return !out[0].layer.has_value() && out[1].layer.has_value()
         && out[1].layer.value() == 0;
}

bool TestQueryRouteShapesThrowsInV21()
{
  ro::MemoryBackedGeometryView view({});
  try {
    (void) view.QueryRouteShapes(r::Rect{}, 0);
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool TestQueryGuidesThrowsInV21()
{
  ro::MemoryBackedGeometryView view({});
  try {
    (void) view.QueryGuides(r::Rect{});
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool TestQueryBlockagesThrowsInV21()
{
  ro::MemoryBackedGeometryView view({});
  try {
    (void) view.QueryBlockages(r::Rect{}, 0);
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool TestSnapshotHandleHoldsViewByShared()
{
  // Lifetime test: SnapshotHandle's shared_ptr keeps the GeometryView
  // alive after the original creating shared_ptr is dropped.
  std::vector<ro::MarkerRef> backing;
  backing.push_back(MakeMarker(0, 0, 10, 10));
  std::shared_ptr<const ro::GeometryView> view
      = std::make_shared<ro::MemoryBackedGeometryView>(std::move(backing));

  ro::SnapshotHandle h(r::Snapshot{}, view);
  view.reset();  // drop the original
  // h.geometry_ still owns the view; query must still work.
  auto out
      = h.geometry().QueryMarkers(r::Rect{r::Point{0, 0}, r::Point{20, 20}});
  return out.size() == 1u;
}

bool TestProposedDeltaCarriesAll()
{
  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 10}};
  add.layer = 4;
  r::ProposedDelta pd;
  pd.delta = add;
  pd.id = r::DeltaId{1, 2, 3};
  pd.write_footprint = r::WriteFootprint::Of(pd.delta);
  pd.read_footprint.geometry.rects.push_back(
      r::Rect{r::Point{-50, -50}, r::Point{60, 60}});
  pd.read_footprint.geometry.layers.push_back(4);
  return pd.write_footprint.shapes.size() == 1u
         && pd.read_footprint.geometry.rects.size() == 1u
         && pd.id == r::DeltaId{1, 2, 3};
}

}  // namespace

int main()
{
  struct {
    const char* name;
    bool (*fn)();
  } cases[] = {
      {"DeltaId ordering", TestDeltaIdOrdering},
      {"WriteFootprint::Of(AddWire)", TestWriteFootprintOfAddWire},
      {"WriteFootprint::Of(InsertShield)",
       TestWriteFootprintOfInsertShield},
      {"WriteFootprint::Of(AddVia) expands",
       TestWriteFootprintOfAddViaExpands},
      {"WriteFootprint::Of(DeleteWire) sets unknown=true",
       TestWriteFootprintOfDeleteWireIsUnknown},
      {"All unimplemented DeltaKinds are unknown",
       TestWriteFootprintOfAllUnimplementedKindsAreUnknown},
      {"ReadFootprint all domains exist",
       TestReadFootprintAllDomainsExist},
      {"Eligibility positive same-layer overlap",
       TestEligibilityIntersectionPositive},
      {"Eligibility negative different-layer",
       TestEligibilityIntersectionNegativeWrongLayer},
      {"Eligibility negative no-overlap",
       TestEligibilityIntersectionNegativeNoOverlap},
      {"ProposedDelta carries id+footprints",
       TestProposedDeltaCarriesAll},
      {"MemoryBackedGeometryView::QueryMarkers overlap filter",
       TestMemoryBackedQueryMarkersReturnsOverlapping},
      {"MemoryBackedGeometryView::QueryMarkers empty backing",
       TestMemoryBackedQueryMarkersEmptyOnEmptyBacking},
      {"MarkerRef.layer distinguishes absent from value 0",
       TestMemoryBackedQueryMarkersDistinguishesAbsentLayer},
      {"GeometryView::QueryRouteShapes throws in V2.1",
       TestQueryRouteShapesThrowsInV21},
      {"GeometryView::QueryGuides throws in V2.1",
       TestQueryGuidesThrowsInV21},
      {"GeometryView::QueryBlockages throws in V2.1",
       TestQueryBlockagesThrowsInV21},
      {"SnapshotHandle holds GeometryView via shared_ptr",
       TestSnapshotHandleHoldsViewByShared},
  };
  int passed = 0;
  int failed = 0;
  for (const auto& c : cases) {
    const bool ok = c.fn();
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", c.name);
    if (ok) {
      ++passed;
    } else {
      ++failed;
    }
  }
  std::printf("redesign_overlay_test: %d passed, %d failed\n", passed,
              failed);
  return failed == 0 ? 0 : 1;
}
