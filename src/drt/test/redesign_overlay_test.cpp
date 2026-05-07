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

#include "redesign/EvalOutcome.h"
#include "redesign/Footprint.h"
#include "redesign/LegalityVerdict.h"
#include "redesign/PhysicalState.h"
#include "redesign/Score.h"
#include "redesign/overlay/GeometryView.h"
#include "redesign/overlay/Hashing.h"
#include "redesign/overlay/MazeSearchProposer.h"
#include "redesign/overlay/MemoryBackedGeometryView.h"
#include "redesign/overlay/MutableGeometryStore.h"
#include "redesign/overlay/OracleCandidate.h"
#include "redesign/overlay/OverlayGeometryView.h"
#include "redesign/overlay/RegionQueryGeometryView.h"
#include "redesign/overlay/ShadowDump.h"
#include "redesign/overlay/SnapshotHandle.h"
#include "redesign/overlay/SyntheticOracle.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

ro::ShapeRef MakeShape(int x1, int y1, int x2, int y2,
                       std::optional<r::LayerNum> layer = std::nullopt,
                       std::optional<r::NetId> net_id = std::nullopt)
{
  ro::ShapeRef s;
  s.bbox = r::Rect{r::Point{x1, y1}, r::Point{x2, y2}};
  s.layer = layer;
  s.net_id = net_id;
  return s;
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

bool TestMemoryBackedQueryRouteShapesFilters()
{
  // V2.2.a.1.mem — MemoryBackedGeometryView::QueryRouteShapes is now
  // a real impl backed by an in-memory vector. This is the
  // V2.2.b prerequisite: OverlayGeometryView's unit tests need
  // route-shape support on the base view.
  std::vector<ro::MarkerRef> empty_markers;
  std::vector<ro::ShapeRef> shapes;
  shapes.push_back(MakeShape(0, 0, 10, 10, /*layer=*/2, /*net=*/100));
  shapes.push_back(
      MakeShape(50, 50, 60, 60, /*layer=*/3, /*net=*/200));
  shapes.push_back(MakeShape(8, 8, 20, 20, /*layer=*/2, /*net=*/100));
  ro::MemoryBackedGeometryView view(empty_markers, std::move(shapes));

  // Overlap query on layer 2: two shapes match.
  auto on_layer2
      = view.QueryRouteShapes(r::Rect{r::Point{5, 5}, r::Point{15, 15}},
                              /*layer=*/2);
  if (on_layer2.size() != 2u) {
    return false;
  }
  // Same bbox, different layer: no shapes match.
  auto on_layer4
      = view.QueryRouteShapes(r::Rect{r::Point{5, 5}, r::Point{15, 15}},
                              /*layer=*/4);
  if (!on_layer4.empty()) {
    return false;
  }
  // Layer 3 covers the second shape.
  auto on_layer3 = view.QueryRouteShapes(
      r::Rect{r::Point{40, 40}, r::Point{70, 70}}, /*layer=*/3);
  return on_layer3.size() == 1u;
}

// V2.2.a.2 — MemoryBackedGeometryView::QueryGuides has a real impl.
// The throw test from V2.1 is replaced.
bool TestMemoryBackedQueryGuidesFilters()
{
  ro::GuideRef g1;
  g1.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 50}};
  g1.begin_layer = 2;
  g1.end_layer = 4;
  g1.net_id = 100;
  ro::GuideRef g2;
  g2.bbox = r::Rect{r::Point{200, 200}, r::Point{250, 250}};
  g2.begin_layer = 6;
  g2.end_layer = 6;
  g2.net_id = 200;

  ro::MemoryBackedGeometryView view({}, {}, {g1, g2});

  auto in_box1 = view.QueryGuides(
      r::Rect{r::Point{10, 10}, r::Point{30, 30}});
  if (in_box1.size() != 1u
      || !in_box1[0].begin_layer.has_value()
      || in_box1[0].begin_layer.value() != 2
      || !in_box1[0].end_layer.has_value()
      || in_box1[0].end_layer.value() != 4) {
    return false;
  }
  auto in_box2 = view.QueryGuides(
      r::Rect{r::Point{220, 220}, r::Point{230, 230}});
  if (in_box2.size() != 1u
      || in_box2[0].begin_layer.value() != 6) {
    return false;
  }
  auto in_neither = view.QueryGuides(
      r::Rect{r::Point{100, 100}, r::Point{110, 110}});
  return in_neither.empty();
}

bool TestGuideRefHashCapturesBothLayerEndpoints()
{
  // Two guides differing only in end_layer must hash differently —
  // this proves the begin/end layer pair captures full identity, not
  // just begin.
  ro::GuideRef g1;
  g1.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 10}};
  g1.begin_layer = 2;
  g1.end_layer = 4;
  g1.net_id = 100;
  ro::GuideRef g2 = g1;
  g2.end_layer = 5;  // only diff
  std::vector<ro::GuideRef> v1 = {g1};
  std::vector<ro::GuideRef> v2 = {g2};
  return ro::HashCanonicalRange(v1) != ro::HashCanonicalRange(v2);
}

// V2.2.a.3 — MemoryBackedGeometryView::QueryBlockages has a real impl.
bool TestMemoryBackedQueryBlockagesFilters()
{
  ro::BlockageRef b1;  // PDK-level blockage on layer 2.
  b1.bbox = r::Rect{r::Point{0, 0}, r::Point{30, 30}};
  b1.layer = 2;
  ro::BlockageRef b2;  // Inst-blockage on same bbox, different layer.
  b2.bbox = r::Rect{r::Point{0, 0}, r::Point{30, 30}};
  b2.layer = 4;
  b2.source_inst_id = 42;

  ro::MemoryBackedGeometryView view({}, {}, {}, {b1, b2});

  auto on_2 = view.QueryBlockages(
      r::Rect{r::Point{10, 10}, r::Point{20, 20}}, 2);
  if (on_2.size() != 1u || on_2[0].source_inst_id.has_value()) {
    return false;  // PDK blockage matches; no source_inst_id
  }
  auto on_4 = view.QueryBlockages(
      r::Rect{r::Point{10, 10}, r::Point{20, 20}}, 4);
  if (on_4.size() != 1u || !on_4[0].source_inst_id.has_value()
      || on_4[0].source_inst_id.value() != 42u) {
    return false;
  }
  auto on_5 = view.QueryBlockages(
      r::Rect{r::Point{10, 10}, r::Point{20, 20}}, 5);
  return on_5.empty();
}

bool TestBlockageRefHashDistinguishesPdkFromInst()
{
  // PDK blockage and inst-blockage with same bbox+layer must hash
  // differently — proves source_inst_id participates in canonical
  // identity (not just bbox+layer).
  ro::BlockageRef pdk;
  pdk.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 10}};
  pdk.layer = 2;
  // pdk.source_inst_id absent
  ro::BlockageRef inst = pdk;
  inst.source_inst_id = 7;
  std::vector<ro::BlockageRef> v_pdk = {pdk};
  std::vector<ro::BlockageRef> v_inst = {inst};
  return ro::HashCanonicalRange(v_pdk) != ro::HashCanonicalRange(v_inst);
}

// V2.2.a.4 — MemoryBackedGeometryView::QueryPinAccess has a real
// in-memory impl. RegionQueryGeometryView::QueryPinAccess still
// throws (the V2.2.a.4 B+C hybrid scope: API/projection-validated,
// not live-backend validated — see plan note in
// v2_drt_redesign_execution_plan.md).
bool TestMemoryBackedQueryPinAccessFilters()
{
  ro::PinAccessRef p1;
  p1.bbox = r::Rect{r::Point{10, 10}, r::Point{10, 10}};  // single point
  p1.layer = 2;
  p1.iterm_id = 100;
  p1.access_point_id = 1;
  p1.has_planar_access = true;
  ro::PinAccessRef p2;
  p2.bbox = r::Rect{r::Point{500, 500}, r::Point{500, 500}};
  p2.layer = 4;
  p2.iterm_id = 200;
  p2.access_point_id = 2;
  p2.has_up_access = true;

  ro::MemoryBackedGeometryView view({}, {}, {}, {}, {p1, p2});

  auto in_box1 = view.QueryPinAccess(
      r::Rect{r::Point{5, 5}, r::Point{15, 15}});
  if (in_box1.size() != 1u
      || !in_box1[0].iterm_id.has_value()
      || in_box1[0].iterm_id.value() != 100u
      || !in_box1[0].has_planar_access.has_value()
      || !in_box1[0].has_planar_access.value()) {
    return false;
  }
  auto in_neither = view.QueryPinAccess(
      r::Rect{r::Point{200, 200}, r::Point{300, 300}});
  return in_neither.empty();
}

bool TestRegionQueryPinAccessStillThrows()
{
  // V2.2.a.4 B+C hybrid contract: RegionQueryGeometryView's
  // QueryPinAccess is intentionally NOT implemented — pin access has
  // no semantically equivalent legacy spatial query in
  // frRegionQuery. Real backend lands when a concrete consumer
  // identifies the right semantic source; until then, calling it is
  // a programming error and must throw.
  ro::RegionQueryGeometryView view(nullptr);
  // (nullptr design_ would short-circuit to {} for implemented
  // methods; QueryPinAccess instead throws because the method body
  // is unimplemented, not because design_ is null.)
  try {
    (void) view.QueryPinAccess(r::Rect{});
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool TestPinAccessRefHashCapturesAccessFlags()
{
  // Two pin-access candidates differing only in has_up_access must
  // hash differently. Proves the optional-bool fields participate in
  // canonical identity.
  ro::PinAccessRef p1;
  p1.bbox = r::Rect{r::Point{0, 0}, r::Point{0, 0}};
  p1.layer = 2;
  p1.iterm_id = 100;
  p1.has_up_access = false;
  ro::PinAccessRef p2 = p1;
  p2.has_up_access = true;
  std::vector<ro::PinAccessRef> v1 = {p1};
  std::vector<ro::PinAccessRef> v2 = {p2};
  return ro::HashCanonicalRange(v1) != ro::HashCanonicalRange(v2);
}

bool TestPinAccessRefHashAbsentVsFalseFlag()
{
  // Critical invariant: an absent has_up_access (nullopt) must hash
  // differently from has_up_access=false. This is the absent-vs-zero
  // discipline applied to optional<bool>.
  ro::PinAccessRef absent;
  absent.bbox = r::Rect{r::Point{0, 0}, r::Point{0, 0}};
  absent.layer = 2;
  // absent.has_up_access stays nullopt
  ro::PinAccessRef explicit_false = absent;
  explicit_false.has_up_access = false;
  std::vector<ro::PinAccessRef> v_absent = {absent};
  std::vector<ro::PinAccessRef> v_false = {explicit_false};
  return ro::HashCanonicalRange(v_absent)
         != ro::HashCanonicalRange(v_false);
}

// ===== V2.1.e.2 — Canonical hashing framework =====

bool TestHashCanonicalRangeOrderInsensitive()
{
  // Same multiset, different insertion order → identical hash.
  std::vector<ro::MarkerRef> a;
  a.push_back(MakeMarker(0, 0, 10, 10, 2, 5));
  a.push_back(MakeMarker(50, 50, 60, 60, 3, 7));
  std::vector<ro::MarkerRef> b;
  b.push_back(MakeMarker(50, 50, 60, 60, 3, 7));
  b.push_back(MakeMarker(0, 0, 10, 10, 2, 5));
  return ro::HashCanonicalRange(a) == ro::HashCanonicalRange(b);
}

bool TestHashCanonicalRangeDistinguishesContent()
{
  std::vector<ro::MarkerRef> a;
  a.push_back(MakeMarker(0, 0, 10, 10, 2, 5));
  std::vector<ro::MarkerRef> b;
  b.push_back(MakeMarker(0, 0, 10, 10, 2, 6));  // different ctype
  return ro::HashCanonicalRange(a) != ro::HashCanonicalRange(b);
}

bool TestHashCanonicalRangeDistinguishesAbsentFromZero()
{
  // CRITICAL invariant per V2.1.e: a field that is absent must hash
  // differently from a field that is present-with-value-0. If these
  // collide, the hash discipline is broken and ShapeSetHash
  // verification is silently wrong.
  ro::MarkerRef absent = MakeMarker(0, 0, 10, 10);  // layer absent
  ro::MarkerRef zero
      = MakeMarker(0, 0, 10, 10, /*layer=*/r::LayerNum{0});
  std::vector<ro::MarkerRef> va = {absent};
  std::vector<ro::MarkerRef> vz = {zero};
  return ro::HashCanonicalRange(va) != ro::HashCanonicalRange(vz);
}

bool TestHashCanonicalRangeStableAcrossRuns()
{
  // FNV-1a is deterministic; two evaluations within one process must
  // be identical.  Cross-run stability is taken on faith from FNV's
  // algorithm; this test confirms determinism within one run.
  std::vector<ro::MarkerRef> a;
  a.push_back(MakeMarker(0, 0, 10, 10, 2, 5));
  a.push_back(MakeMarker(50, 50, 60, 60, 3, 7));
  uint64_t h1 = ro::HashCanonicalRange(a);
  uint64_t h2 = ro::HashCanonicalRange(a);
  return h1 == h2;
}

bool TestHashCanonicalRangeEmptyRange()
{
  std::vector<ro::MarkerRef> empty;
  // Empty range hashes to the FNV offset (an arbitrary but stable
  // value). The exact value doesn't matter; the test confirms it
  // doesn't crash.
  uint64_t h = ro::HashCanonicalRange(empty);
  (void) h;
  return true;
}

// ===== V2.2.a.1 — ShapeRef CanonicalTuple + Hash =====

bool TestShapeRefHashOrderInsensitive()
{
  std::vector<ro::ShapeRef> a = {MakeShape(0, 0, 10, 5, 2, 100),
                                 MakeShape(20, 20, 25, 25, 3, 200)};
  std::vector<ro::ShapeRef> b = {MakeShape(20, 20, 25, 25, 3, 200),
                                 MakeShape(0, 0, 10, 5, 2, 100)};
  return ro::HashCanonicalRange(a) == ro::HashCanonicalRange(b);
}

bool TestShapeRefHashDistinguishesNet()
{
  std::vector<ro::ShapeRef> a = {MakeShape(0, 0, 10, 5, 2, 100)};
  std::vector<ro::ShapeRef> b = {MakeShape(0, 0, 10, 5, 2, 101)};
  return ro::HashCanonicalRange(a) != ro::HashCanonicalRange(b);
}

bool TestShapeRefHashDistinguishesAbsentFromZero()
{
  std::vector<ro::ShapeRef> absent = {MakeShape(0, 0, 10, 5, 2)};
  std::vector<ro::ShapeRef> zero
      = {MakeShape(0, 0, 10, 5, 2, /*net_id=*/r::NetId{0})};
  return ro::HashCanonicalRange(absent)
         != ro::HashCanonicalRange(zero);
}

bool TestShapeRefHashDifferentEntityFromMarker()
{
  // ShapeRef and MarkerRef with the same bbox/layer should NOT hash
  // identically — the per-entity CanonicalTuple is responsible for
  // not conflating distinct entities (different field count + types).
  // This tests that the canonical-tuple discipline holds.
  std::vector<ro::ShapeRef> shapes = {MakeShape(0, 0, 10, 5, 2)};
  std::vector<ro::MarkerRef> markers = {MakeMarker(0, 0, 10, 5, 2)};
  return ro::HashCanonicalRange(shapes)
         != ro::HashCanonicalRange(markers);
}

// ===== V2.1.e.5 — ShadowDump =====

bool TestShadowDumpCountersIncrement()
{
  ro::ShadowDump::ResetForTest();
  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");

  r::Rect box{r::Point{0, 0}, r::Point{10, 10}};
  ro::ShadowDump::RecordMarkerComparison(0xAA, 0xAA, 1, 1, box);
  ro::ShadowDump::RecordMarkerComparison(0xAA, 0xBB, 1, 1, box);
  ro::ShadowDump::RecordMarkerComparison(0xCC, 0xCC, 5, 5, box);

  return ro::ShadowDump::comparison_count() == 3
         && ro::ShadowDump::mismatch_count() == 1;
}

bool TestShadowDumpDisabledByDefaultWritesNoFile()
{
  ro::ShadowDump::ResetForTest();
  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  // Set the path to a location that would fail if the dump tried to
  // write — but with DUMP_HASHES unset, the path should be ignored
  // and no file should be opened.
  ::setenv("OPENROAD_OVERLAY_DUMP_PATH",
           "/nonexistent_dir_12345/should_never_be_created.csv", 1);

  r::Rect box{r::Point{0, 0}, r::Point{10, 10}};
  ro::ShadowDump::RecordMarkerComparison(0xAA, 0xAA, 1, 1, box);

  std::ifstream f(
      "/nonexistent_dir_12345/should_never_be_created.csv");
  const bool no_file = !f.is_open();
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");
  return no_file;
}

bool TestShadowDumpEnabledWritesHeaderAndRow()
{
  ro::ShadowDump::ResetForTest();
  // Use /tmp + a unique-ish name; cleanup at end.
  std::ostringstream os;
  os << "/tmp/openroad-overlay-hashes-test-"
     << static_cast<unsigned>(::getpid()) << ".csv";
  const std::string path = os.str();
  std::remove(path.c_str());

  ::setenv("OPENROAD_OVERLAY_DUMP_HASHES", "1", 1);
  ::setenv("OPENROAD_OVERLAY_DUMP_PATH", path.c_str(), 1);

  r::Rect box{r::Point{1, 2}, r::Point{3, 4}};
  ro::ShadowDump::RecordMarkerComparison(0x11, 0x22, 7, 8, box);

  std::ifstream f(path);
  if (!f.is_open()) {
    ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
    ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");
    return false;
  }
  std::string header;
  std::string row;
  std::getline(f, header);
  std::getline(f, row);
  f.close();
  std::remove(path.c_str());
  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");

  // Header should mention all V2.2.a.0 column names.
  const bool header_ok
      = header.find("seqno") != std::string::npos
        && header.find("entity") != std::string::npos
        && header.find("query_kind") != std::string::npos
        && header.find("layer") != std::string::npos
        && header.find("legacy_hash") != std::string::npos
        && header.find("overlay_hash") != std::string::npos;
  // Row should contain the marker entity tag, queryMarker query kind,
  // bbox coordinates, and hashes we passed in.
  const bool row_ok = row.find(",marker,") != std::string::npos
                      && row.find(",queryMarker,") != std::string::npos
                      && row.find(",1,2,3,4,") != std::string::npos
                      && row.find(",17,") != std::string::npos
                      && row.find(",34,") != std::string::npos;
  return header_ok && row_ok;
}

bool TestShadowDumpRouteShapeRowHasEntityAndLayer()
{
  // V2.2.a.0 — verify per-entity recording for route shapes. CSV row
  // must carry entity=route_shape, query_kind=query, and a non-empty
  // layer cell.
  ro::ShadowDump::ResetForTest();
  std::ostringstream os;
  os << "/tmp/openroad-overlay-hashes-test-rs-"
     << static_cast<unsigned>(::getpid()) << ".csv";
  const std::string path = os.str();
  std::remove(path.c_str());

  ::setenv("OPENROAD_OVERLAY_DUMP_HASHES", "1", 1);
  ::setenv("OPENROAD_OVERLAY_DUMP_PATH", path.c_str(), 1);

  r::Rect box{r::Point{5, 6}, r::Point{7, 8}};
  ro::ShadowDump::RecordRouteShapeComparison(0xAA, 0xAA, 3, 3, box, 12);

  std::ifstream f(path);
  std::string header, row;
  std::getline(f, header);
  std::getline(f, row);
  f.close();
  std::remove(path.c_str());
  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");

  const bool entity_ok
      = row.find(",route_shape,") != std::string::npos;
  const bool query_kind_ok = row.find(",query,") != std::string::npos;
  const bool bbox_ok = row.find(",5,6,7,8,") != std::string::npos;
  const bool layer_ok = row.find(",12,") != std::string::npos;
  return entity_ok && query_kind_ok && bbox_ok && layer_ok;
}

bool TestShadowDumpPerEntityCounters()
{
  // V2.2.a.0 — per-entity comparison counts diverge correctly when
  // records of different entities arrive.
  ro::ShadowDump::ResetForTest();
  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");

  r::Rect box{r::Point{0, 0}, r::Point{1, 1}};
  ro::ShadowDump::RecordMarkerComparison(0xAA, 0xAA, 1, 1, box);
  ro::ShadowDump::RecordMarkerComparison(0xAA, 0xBB, 1, 1, box);
  ro::ShadowDump::RecordRouteShapeComparison(0xCC, 0xCC, 5, 5, box, 2);
  ro::ShadowDump::RecordRouteShapeComparison(0xCC, 0xDD, 5, 5, box, 2);
  ro::ShadowDump::RecordRouteShapeComparison(0xCC, 0xCC, 5, 5, box, 3);

  using E = ro::ShadowDump::Entity;
  return ro::ShadowDump::comparison_count() == 5
         && ro::ShadowDump::mismatch_count() == 2
         && ro::ShadowDump::comparison_count_for(E::Marker) == 2
         && ro::ShadowDump::mismatch_count_for(E::Marker) == 1
         && ro::ShadowDump::comparison_count_for(E::RouteShape) == 3
         && ro::ShadowDump::mismatch_count_for(E::RouteShape) == 1
         && ro::ShadowDump::comparison_count_for(E::Guide) == 0;
}

bool TestShadowDumpBadPathDoesNotThrow()
{
  ro::ShadowDump::ResetForTest();
  // Use a path inside a directory we can't create (a path under an
  // existing file like /etc/hostname). The mkdir + open both fail;
  // the discipline is "warn once, disable, never throw."
  ::setenv("OPENROAD_OVERLAY_DUMP_HASHES", "1", 1);
  ::setenv("OPENROAD_OVERLAY_DUMP_PATH",
           "/etc/hostname/never_writable_subdir/dump.csv", 1);

  r::Rect box{r::Point{0, 0}, r::Point{1, 1}};
  bool threw = false;
  try {
    ro::ShadowDump::RecordMarkerComparison(0xDE, 0xAD, 0, 0, box);
    ro::ShadowDump::RecordMarkerComparison(0xBE, 0xEF, 0, 0, box);
  } catch (...) {
    threw = true;
  }

  ::unsetenv("OPENROAD_OVERLAY_DUMP_HASHES");
  ::unsetenv("OPENROAD_OVERLAY_DUMP_PATH");

  // Counters still incremented (diagnostic invariant); no throw.
  return !threw && ro::ShadowDump::comparison_count() == 2;
}

// ===== V2.2.b — OverlayGeometryView =====

bool TestOverlayPassThroughEmptyDeltas()
{
  // No deltas → overlay queries equal base queries.
  std::vector<ro::MarkerRef> base_markers;
  base_markers.push_back(MakeMarker(0, 0, 10, 10, 2));
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeShape(0, 0, 100, 5, 2, 1));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      base_markers, base_shapes);

  ro::OverlayGeometryView overlay(base, {});

  auto base_markers_q
      = base->QueryMarkers(r::Rect{r::Point{-5, -5}, r::Point{50, 50}});
  auto overlay_markers_q
      = overlay.QueryMarkers(r::Rect{r::Point{-5, -5}, r::Point{50, 50}});
  if (base_markers_q.size() != overlay_markers_q.size()) {
    return false;
  }

  auto base_shapes_q = base->QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{200, 50}}, 2);
  auto overlay_shapes_q = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{200, 50}}, 2);
  return base_shapes_q.size() == overlay_shapes_q.size()
         && base_shapes_q.size() == 1u;
}

bool TestOverlayAddsAddWireToQueryRouteShapes()
{
  // Empty base + AddWire delta → QueryRouteShapes returns the added
  // shape on the matching layer + bbox.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::AddWire add;
  add.bbox = r::Rect{r::Point{10, 0}, r::Point{50, 5}};
  add.layer = 3;
  add.net = nullptr;  // synthetic; net_id will be absent in V2.2.b
  std::vector<r::Delta> deltas{add};

  ro::OverlayGeometryView overlay(base, std::move(deltas));

  // Same layer, overlapping bbox: returns 1 shape.
  auto on_3 = overlay.QueryRouteShapes(
      r::Rect{r::Point{0, -10}, r::Point{100, 10}}, 3);
  if (on_3.size() != 1u
      || on_3[0].bbox.ll.x != 10 || on_3[0].bbox.ur.x != 50
      || !on_3[0].layer.has_value() || on_3[0].layer.value() != 3
      || on_3[0].net_id.has_value()) {
    return false;
  }

  // Different layer: empty.
  auto on_4 = overlay.QueryRouteShapes(
      r::Rect{r::Point{0, -10}, r::Point{100, 10}}, 4);
  if (!on_4.empty()) {
    return false;
  }

  // Same layer, non-overlapping bbox: empty.
  auto far_box = overlay.QueryRouteShapes(
      r::Rect{r::Point{1000, 1000}, r::Point{2000, 2000}}, 3);
  return far_box.empty();
}

bool TestOverlayComposesAddWithBase()
{
  // Base has 1 shape on layer 2. Add delta on layer 2 — query
  // returns 2 shapes.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeShape(0, 0, 100, 5, 2, 7));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{200, 0}, r::Point{300, 5}};
  add.layer = 2;
  std::vector<r::Delta> deltas{add};

  ro::OverlayGeometryView overlay(base, std::move(deltas));

  auto on_2 = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{500, 50}}, 2);
  return on_2.size() == 2u;
}

bool TestOverlayStacksOverlayOverOverlay()
{
  // Composability: OverlayGeometryView over an OverlayGeometryView
  // over a base.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::AddWire a;
  a.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  a.layer = 2;
  auto inner = std::make_shared<ro::OverlayGeometryView>(
      base, std::vector<r::Delta>{a});

  r::AddWire b;
  b.bbox = r::Rect{r::Point{20, 0}, r::Point{30, 5}};
  b.layer = 2;
  ro::OverlayGeometryView outer(inner, std::vector<r::Delta>{b});

  auto out = outer.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{100, 50}}, 2);
  return out.size() == 2u;
}

bool TestOverlayInsertShieldComposes()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::InsertShield shield;
  shield.coverage = r::Rect{r::Point{0, 0}, r::Point{100, 50}};
  shield.layer = 5;
  std::vector<r::Delta> deltas{shield};

  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto on_5 = overlay.QueryRouteShapes(
      r::Rect{r::Point{10, 10}, r::Point{20, 20}}, 5);
  if (on_5.size() != 1u
      || on_5[0].layer.value_or(-1) != 5) {
    return false;
  }
  auto on_6 = overlay.QueryRouteShapes(
      r::Rect{r::Point{10, 10}, r::Point{20, 20}}, 6);
  return on_6.empty();
}

// V2.2.b.del — DeleteWire with resolved identity removes a single
// base shape via multiset subtraction.
//
// Helper to construct a base ShapeRef matching the V2.2.b.del
// canonical-identity contract (bbox, layer, net_id, shape_kind).
ro::ShapeRef MakeFullShape(int x1, int y1, int x2, int y2,
                           r::LayerNum layer, r::NetId net_id,
                           std::uint8_t kind)
{
  ro::ShapeRef s;
  s.bbox = r::Rect{r::Point{x1, y1}, r::Point{x2, y2}};
  s.layer = layer;
  s.net_id = net_id;
  s.shape_kind = kind;
  return s;
}

r::DeleteWire MakeDeleteWire(int x1, int y1, int x2, int y2,
                             r::LayerNum layer, std::uint64_t net_id,
                             std::uint8_t kind)
{
  r::DeleteWire d;
  d.segment_id = 1;
  d.bbox = r::Rect{r::Point{x1, y1}, r::Point{x2, y2}};
  d.layer = layer;
  d.resolved_net_id = net_id;
  d.shape_kind = kind;
  return d;
}

bool TestDeleteMultiset_BaseAA_DelA_ResultA()
{
  // base=[A,A], delete=[A] → result=[A]
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100, 1),
                                MakeFullShape(0, 0, 10, 5, 2, 100, 1)});
  std::vector<r::Delta> deltas{MakeDeleteWire(0, 0, 10, 5, 2, 100, 1)};
  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{50, 50}}, 2);
  return out.size() == 1u;
}

bool TestDeleteMultiset_BaseA_DelAA_ResultEmpty()
{
  // base=[A], delete=[A,A] → result=[]
  // Extra delete beyond base count is silently tolerated.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100, 1)});
  std::vector<r::Delta> deltas{MakeDeleteWire(0, 0, 10, 5, 2, 100, 1),
                               MakeDeleteWire(0, 0, 10, 5, 2, 100, 1)};
  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{50, 50}}, 2);
  return out.empty();
}

bool TestDeleteMultiset_BaseAB_DelA_ResultB()
{
  // base=[A,B], delete=[A] → result=[B]
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100, 1),
                                MakeFullShape(20, 0, 30, 5, 2, 200, 1)});
  std::vector<r::Delta> deltas{MakeDeleteWire(0, 0, 10, 5, 2, 100, 1)};
  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{50, 50}}, 2);
  return out.size() == 1u
         && out[0].bbox.ll.x == 20  // B's bbox, not A's
         && out[0].net_id.value_or(0) == 200u;
}

bool TestDeleteMultiset_DelAbsent_NoEffect()
{
  // delete=[C] absent from base → no effect, no crash.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100, 1)});
  std::vector<r::Delta> deltas{
      MakeDeleteWire(500, 500, 600, 600, 5, 999, 1)};  // absent
  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{1000, 1000}}, 2);
  return out.size() == 1u;
}

bool TestDeleteUnresolvedNetIdIsNoop()
{
  // V2.2.b.del contract: a DeleteWire without resolved_net_id is
  // not safe for commit. OverlayGeometryView silently skips it
  // (production validation happens at WriteFootprint::unknown +
  // commit-path rejection, not in the view).
  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  del.layer = 2;
  // resolved_net_id and shape_kind intentionally NOT set
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100, 1)});
  ro::OverlayGeometryView overlay(base, std::vector<r::Delta>{del});
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{50, 50}}, 2);
  return out.size() == 1u;  // base shape preserved
}

bool TestDeleteShapeKindMismatchDoesNotMatch()
{
  // A path-seg ShapeRef and a delete-wire targeting a "patch wire"
  // (different shape_kind) at the same bbox+layer+net should NOT
  // match — exact-identity removal, not bbox-only subtraction.
  // path_seg = 12, patch_wire = 22 (frBlockObjectEnum values).
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{MakeFullShape(0, 0, 10, 5, 2, 100,
                                              /*kind=path_seg*/ 12)});
  std::vector<r::Delta> deltas{
      MakeDeleteWire(0, 0, 10, 5, 2, 100, /*kind=patch_wire*/ 22)};
  ro::OverlayGeometryView overlay(base, std::move(deltas));
  auto out = overlay.QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{50, 50}}, 2);
  return out.size() == 1u;  // path-seg base preserved; patch-wire
                            // delete didn't match
}

bool TestDeleteWriteFootprintUnknownWhenUnresolved()
{
  // V2.2.b.del contract: DeleteWire without resolved fields has
  // WriteFootprint::unknown=true (V2.4 commit path will reject it).
  r::DeleteWire incomplete;
  incomplete.segment_id = 1;
  // bbox left zero-area; resolved_net_id absent
  r::Delta d_inc = incomplete;
  auto wf_inc = r::WriteFootprint::Of(d_inc);
  if (!wf_inc.unknown) {
    return false;
  }

  // V2.2.b.del contract: DeleteWire with resolved fields has
  // WriteFootprint::unknown=false (committable in parallel batches).
  r::DeleteWire complete;
  complete.segment_id = 1;
  complete.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  complete.layer = 2;
  complete.resolved_net_id = 100;
  complete.shape_kind = 1;
  r::Delta d_com = complete;
  auto wf_com = r::WriteFootprint::Of(d_com);
  return !wf_com.unknown && wf_com.shapes.size() == 1u
         && wf_com.layers.size() == 1u && wf_com.layers[0] == 2;
}

bool TestOverlayPassesGuidesAndMarkersThrough()
{
  // V2.2.b: no Add/Delete deltas for markers or guides; pass-through
  // to base.
  std::vector<ro::MarkerRef> base_markers;
  base_markers.push_back(MakeMarker(0, 0, 10, 10, 2));
  ro::GuideRef g;
  g.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 50}};
  g.begin_layer = 2;
  g.end_layer = 4;
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      base_markers, std::vector<ro::ShapeRef>{},
      std::vector<ro::GuideRef>{g});

  // Even with an unrelated AddWire delta in the list, markers and
  // guides are still pass-through.
  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  add.layer = 2;
  ro::OverlayGeometryView overlay(base, std::vector<r::Delta>{add});

  auto markers
      = overlay.QueryMarkers(r::Rect{r::Point{-5, -5}, r::Point{50, 50}});
  auto guides
      = overlay.QueryGuides(r::Rect{r::Point{-5, -5}, r::Point{100, 100}});
  return markers.size() == 1u && guides.size() == 1u;
}

// ===== V2.2.c.proj — PhysicalState::eval =====

bool TestEvalAddWireProducesPositiveWirelength()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{100, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  auto outcome = state.eval(*base, pd);

  if (!outcome.score.has_value()) {
    return false;
  }
  // Manhattan length proxy = max(dx=100, dy=5) = 100.
  if (outcome.score->delta_wirelength_proxy != 100.0) {
    return false;
  }
  if (outcome.score->delta_via_count != 0) {
    return false;
  }
  // V2.1.b hard-gate: stub legality is NEVER commit-eligible, even
  // though legal=true (the placeholder default).
  if (outcome.legality.source != r::LegalitySource::StubAssumeLegal) {
    return false;
  }
  return outcome.legality.legal && !outcome.legality.commit_eligible;
}

bool TestEvalDeleteWireProducesNegativeWirelength()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  del.resolved_net_id = 100;
  del.shape_kind = 1;
  r::ProposedDelta pd;
  pd.delta = del;

  r::PhysicalState state;
  auto outcome = state.eval(*base, pd);

  if (!outcome.score.has_value()) {
    return false;
  }
  // Manhattan length proxy = -max(50, 5) = -50.
  if (outcome.score->delta_wirelength_proxy != -50.0) {
    return false;
  }
  // Stub legality, even for resolved deletes.
  return outcome.legality.source == r::LegalitySource::StubAssumeLegal
         && !outcome.legality.commit_eligible;
}

bool TestEvalUnresolvedDeleteIdentitySurfaces()
{
  // Unresolved DeleteWire (no resolved_net_id, no shape_kind) →
  // EvalOutcome surfaces it loudly with
  // LegalitySource::UnresolvedFootprint and commit_eligible=false.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{},
      std::vector<ro::ShapeRef>{
          MakeFullShape(0, 0, 100, 5, 2, 100, 1)});

  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  // resolved_net_id and shape_kind intentionally absent
  r::ProposedDelta pd;
  pd.delta = del;

  r::PhysicalState state;
  auto outcome = state.eval(*base, pd);

  return outcome.legality.source
             == r::LegalitySource::UnresolvedFootprint
         && !outcome.legality.legal
         && !outcome.legality.commit_eligible
         && !outcome.score.has_value();  // no score by default
}

bool TestEvalUnresolvedDeleteScoreOnlyOnRequest()
{
  // Even with score_even_if_illegal, an unresolved-footprint
  // verdict stays non-committable. The score is provided but it's
  // a zero-filled stub — eval cannot meaningfully cost a delta
  // whose effect is unknown.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::DeleteWire del;
  del.segment_id = 1;
  // unresolved
  r::ProposedDelta pd;
  pd.delta = del;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.score_even_if_illegal = true;
  auto outcome = state.eval(*base, pd, opts);

  return outcome.legality.source
             == r::LegalitySource::UnresolvedFootprint
         && !outcome.legality.commit_eligible
         && outcome.score.has_value()
         && outcome.score->delta_wirelength_proxy == 0.0;
}

bool TestEvalAddViaIncrementsViaCount()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  r::AddVia v;
  v.location = r::Point{500, 500};
  r::ProposedDelta pd;
  pd.delta = v;

  r::PhysicalState state;
  auto outcome = state.eval(*base, pd);

  return outcome.score.has_value()
         && outcome.score->delta_via_count == 1
         && !outcome.legality.commit_eligible;  // stub
}

bool TestEvalSnapshotOverloadInvalidSnapshotIsUnresolved()
{
  // V2.2.d: Snapshot-taking eval no longer throws; an invalid
  // (default-constructed) Snapshot returns LegalitySource::
  // UnresolvedFootprint, commit_eligible=false. A valid Snapshot
  // produces real eval output via Snapshot::geometry().
  r::PhysicalState state;
  r::Snapshot bad;  // default-constructed = invalid
  r::ProposedDelta pd;
  pd.delta = r::AddWire{};
  auto outcome = state.eval(bad, pd);
  return outcome.legality.source
             == r::LegalitySource::UnresolvedFootprint
         && !outcome.legality.commit_eligible;
}

// ===== V2.2.c.bridge — DeltaToOracleInput =====

bool TestBridgeAddWireProducesOneCandidateWire()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::AddWire add;
  add.bbox = r::Rect{r::Point{10, 0}, r::Point{50, 5}};
  add.layer = 3;
  r::ProposedDelta pd;
  pd.delta = add;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  if (batch.status != ro::OracleCandidateBatch::Status::Ok) {
    return false;
  }
  if (batch.added.size() != 1u || !batch.deleted_context.empty()) {
    return false;
  }
  const auto& c = batch.added[0];
  return c.kind == ro::OracleCandidateShape::Kind::Wire
         && c.bbox.ll.x == 10 && c.bbox.ur.x == 50
         && c.layer == 3 && !c.net_id.has_value();
}

bool TestBridgeAddViaProducesViaCutCandidate()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::AddVia v;
  v.location = r::Point{500, 500};
  r::ProposedDelta pd;
  pd.delta = v;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  return batch.status == ro::OracleCandidateBatch::Status::Ok
         && batch.added.size() == 1u
         && batch.added[0].kind
                == ro::OracleCandidateShape::Kind::ViaCut
         && batch.added[0].bbox.ur.x > batch.added[0].bbox.ll.x
         && batch.added[0].bbox.ur.y > batch.added[0].bbox.ll.y
         && batch.deleted_context.empty();
}

bool TestBridgeInsertShieldProducesShieldCandidate()
{
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::InsertShield s;
  s.coverage = r::Rect{r::Point{0, 0}, r::Point{100, 50}};
  s.layer = 5;
  r::ProposedDelta pd;
  pd.delta = s;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  return batch.status == ro::OracleCandidateBatch::Status::Ok
         && batch.added.size() == 1u
         && batch.added[0].kind == ro::OracleCandidateShape::Kind::Shield
         && batch.added[0].layer == 5
         && batch.deleted_context.empty();
}

bool TestBridgeResolvedDeleteWireGoesToContext()
{
  // DeleteWire with full identity → no `added` entry, but
  // `deleted_context` records the removal target.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  del.resolved_net_id = 100;
  del.shape_kind = 22;  // frcPatchWire raw value
  r::ProposedDelta pd;
  pd.delta = del;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  return batch.status == ro::OracleCandidateBatch::Status::Ok
         && batch.added.empty()
         && batch.deleted_context.size() == 1u
         && batch.deleted_context[0].kind
                == ro::OracleCandidateShape::Kind::PatchWire
         && batch.deleted_context[0].layer == 2
         && batch.deleted_context[0].net_id.has_value()
         && batch.deleted_context[0].net_id.value() == 100u;
}

bool TestBridgeUnresolvedDeleteFlagsStatus()
{
  // DeleteWire missing resolved_net_id → status =
  // UnresolvedFootprint, no entries.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  // resolved_net_id and shape_kind absent
  r::ProposedDelta pd;
  pd.delta = del;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  return batch.status
             == ro::OracleCandidateBatch::Status::UnresolvedFootprint
         && batch.added.empty()
         && batch.deleted_context.empty();
}

bool TestBridgeUnsupportedDeltaKindFlagsStatus()
{
  // MoveCell is not adapter-eligible in V2.2.c.bridge.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::MoveCell mc;
  mc.inst = nullptr;
  mc.new_origin = r::Point{0, 0};
  r::ProposedDelta pd;
  pd.delta = mc;

  auto batch = ro::DeltaToOracleInput(*base, pd);
  return batch.status
             == ro::OracleCandidateBatch::Status::UnsupportedDelta
         && batch.added.empty()
         && batch.deleted_context.empty();
}

bool TestEvalOptionsLegalityModeDefaultsToStub()
{
  // V2.2.c.bridge declared LegalityMode but eval keeps StubAssumeLegal
  // semantics until V2.2.c.legality.synthetic. Verify the default and
  // that explicitly passing LegalityMode::SyntheticOracle does NOT
  // yet activate synthetic logic (it stays stub-noncommittable until
  // the next sub-commit wires the real branch).
  r::EvalOptions opts_default;
  if (opts_default.legality_mode != r::LegalityMode::StubAssumeLegal) {
    return false;
  }
  // Just check the enum values are distinct and addressable.
  return r::LegalityMode::StubAssumeLegal
             != r::LegalityMode::SyntheticOracle
         && r::LegalityMode::SyntheticOracle
                != r::LegalityMode::CpuDrcOracleRealDeck;
}

// ===== V2.2.c.legality.synthetic — SyntheticOracle =====

bool TestSyntheticOracleAddWireFarFromBaseIsLegal()
{
  // Base shape on layer 2 at (0..10, 0..5). Add a wire on layer 2
  // far away (200..300, 200..205). Expect: legal,
  // source=SyntheticOracle, commit_eligible=true.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeFullShape(0, 0, 10, 5, 2, 100, 12));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{200, 200}, r::Point{300, 205}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto outcome = state.eval(*base, pd, opts);

  return outcome.legality.legal
         && outcome.legality.source
                == r::LegalitySource::SyntheticOracle
         && outcome.legality.commit_eligible
         && outcome.legality.violations.empty();
}

bool TestSyntheticOracleAddWireOverlapBaseIsIllegal()
{
  // Base at (0..50, 0..5) on layer 2. Add wire overlapping it on
  // layer 2. Expect: illegal (Short), commit_eligible=false even
  // though source is SyntheticOracle.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeFullShape(0, 0, 50, 5, 2, 100, 12));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{20, 0}, r::Point{30, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto outcome = state.eval(*base, pd, opts);

  return !outcome.legality.legal
         && outcome.legality.source
                == r::LegalitySource::SyntheticOracle
         && !outcome.legality.commit_eligible
         && !outcome.legality.violations.empty();
}

bool TestSyntheticOracleAddWireOnDifferentLayerIsLegal()
{
  // Base on layer 2; add wire on layer 3 at the same coords. Even
  // bbox-overlap doesn't matter because different layers don't
  // interact in the synthetic rules.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeFullShape(0, 0, 50, 5, 2, 100, 12));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  add.layer = 3;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto outcome = state.eval(*base, pd, opts);

  return outcome.legality.legal
         && outcome.legality.commit_eligible;
}

bool TestSyntheticOracleSpacingViolationDetected()
{
  // Base wire on layer 2 at (0..50, 0..5). Add wire on layer 2 at
  // (60..100, 0..5). Edge distance = 60 - 50 = 10, well below
  // threshold 50. Expect PrlSpacing violation.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeFullShape(0, 0, 50, 5, 2, 100, 12));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{60, 0}, r::Point{100, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto outcome = state.eval(*base, pd, opts);

  return !outcome.legality.legal
         && !outcome.legality.commit_eligible
         && std::find(outcome.legality.violations.begin(),
                      outcome.legality.violations.end(),
                      r::MarkerKind::PrlSpacing)
                != outcome.legality.violations.end();
}

bool TestSyntheticOracleDefaultModeRemainsStub()
{
  // Without explicit legality_mode, eval still uses StubAssumeLegal
  // and refuses commit-eligibility. PoC must be opt-in.
  std::vector<ro::ShapeRef> base_shapes;
  base_shapes.push_back(MakeFullShape(0, 0, 50, 5, 2, 100, 12));
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, base_shapes);

  r::AddWire add;
  add.bbox = r::Rect{r::Point{200, 200}, r::Point{300, 205}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  // No opts — defaults
  auto outcome = state.eval(*base, pd);

  return outcome.legality.source == r::LegalitySource::StubAssumeLegal
         && !outcome.legality.commit_eligible;
}

bool TestSyntheticOracleCpuDrcModeFallsBackToStub()
{
  // V2.2.c.legality.synthetic: requesting real-PDK mode before it's
  // wired returns a non-committable stub. No silent fall-through to
  // synthetic.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});
  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;
  auto outcome = state.eval(*base, pd, opts);

  // Source falls back to StubAssumeLegal (NOT CpuDrcOracle, because
  // we didn't actually run that oracle), commit_eligible=false.
  return outcome.legality.source == r::LegalitySource::StubAssumeLegal
         && !outcome.legality.commit_eligible;
}

bool TestSyntheticOracleTrustedSourceRule()
{
  // Compile-time + runtime assertions about the trusted-source rule
  // from LegalityVerdict.h.
  return r::IsTrustedLegalitySource(r::LegalitySource::SyntheticOracle)
         && r::IsTrustedLegalitySource(r::LegalitySource::CpuDrcOracle)
         && r::IsTrustedLegalitySource(r::LegalitySource::UpstreamExact)
         && !r::IsTrustedLegalitySource(
             r::LegalitySource::StubAssumeLegal)
         && !r::IsTrustedLegalitySource(
             r::LegalitySource::UnresolvedFootprint);
}

// ===== V2.2.d — try_commit single-Delta path =====

// Minimal ConflictPolicy stub for V2.2.d tests. Single-Delta scope
// doesn't consult the policy, so the impl just returns no
// committed indices.
class V22dStubPolicy : public r::ConflictPolicy
{
 public:
  std::vector<std::size_t> select(
      const std::vector<r::ScoredProposal>& scored,
      const std::vector<std::pair<std::size_t, std::size_t>>&
          conflict_edges) override
  {
    (void) scored;
    (void) conflict_edges;
    return {};
  }
  r::PolicyKind kind() const noexcept override
  {
    return r::PolicyKind::GreedyPriority;
  }
};

bool TestTryCommitDefaultModeRejectsAll()
{
  // V2.2.d: try_commit with default opts uses StubAssumeLegal,
  // which is non-committable. AddWire proposal → rejected.
  // Version stays at 1.
  r::PhysicalState state;
  auto snap = state.snapshot();

  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;
  pd.snapshot_version = snap.version();

  V22dStubPolicy policy;
  auto result = state.try_commit(snap, {pd}, policy);

  return result.committed_indices.empty()
         && result.rejected_indices.size() == 1u
         && result.rejected_indices[0] == 0u
         && result.snapshot_stale.empty()
         && result.new_version == snap.version();
}

bool TestTryCommitWithSyntheticCommitsLegalDelta()
{
  // V2.2.d: try_commit_with_opts using LegalityMode::SyntheticOracle
  // commits an AddWire that doesn't violate the synthetic rules.
  // The store gains the new shape; version bumps from 1 to 2.
  r::PhysicalState state;
  auto snap = state.snapshot();
  const auto v_before = snap.version();

  r::AddWire add;
  add.bbox = r::Rect{r::Point{0, 0}, r::Point{100, 5}};
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;
  pd.snapshot_version = v_before;

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  if (result.committed_indices.size() != 1u
      || !result.rejected_indices.empty()
      || !result.snapshot_stale.empty()) {
    return false;
  }
  if (result.new_version != v_before + 1) {
    return false;
  }
  // Store now contains the added shape.
  return state.mutable_store_for_test().route_shape_count() == 1u;
}

bool TestTryCommitWithSyntheticRejectsIllegalDelta()
{
  // Seed an existing shape, then propose an overlapping AddWire.
  // Synthetic oracle returns illegal; try_commit rejects without
  // mutation; version stays the same.
  r::PhysicalState state;
  state.mutable_store_for_test().SeedRouteShapes(
      {MakeFullShape(0, 0, 50, 5, 2, 100, 12)});
  auto snap = state.snapshot();
  const auto v_before = snap.version();

  r::AddWire add;
  add.bbox = r::Rect{r::Point{20, 0}, r::Point{30, 5}};  // overlaps
  add.layer = 2;
  r::ProposedDelta pd;
  pd.delta = add;
  pd.snapshot_version = v_before;

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  return result.committed_indices.empty()
         && result.rejected_indices.size() == 1u
         && result.snapshot_stale.empty()
         && result.new_version == v_before
         && state.mutable_store_for_test().route_shape_count() == 1u;
}

bool TestTryCommitStaleSnapshotIsFlagged()
{
  // Commit one delta, advancing version. A second proposal whose
  // snapshot_version still points to the old version is flagged
  // snapshot_stale, not rejected/committed.
  r::PhysicalState state;
  auto snap_v1 = state.snapshot();

  r::AddWire add1;
  add1.bbox = r::Rect{r::Point{0, 0}, r::Point{100, 5}};
  add1.layer = 2;
  r::ProposedDelta pd1;
  pd1.delta = add1;
  pd1.snapshot_version = snap_v1.version();

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  state.try_commit_with_opts(snap_v1, {pd1}, policy, opts);

  // Now version is 2. snap_v1 still has version 1. A new proposal
  // generated against snap_v1 is stale.
  r::AddWire add2;
  add2.bbox = r::Rect{r::Point{500, 0}, r::Point{600, 5}};
  add2.layer = 2;
  r::ProposedDelta pd2;
  pd2.delta = add2;
  pd2.snapshot_version = snap_v1.version();  // = 1, but cur = 2

  auto result
      = state.try_commit_with_opts(snap_v1, {pd2}, policy, opts);

  return result.snapshot_stale.size() == 1u
         && result.committed_indices.empty()
         && result.rejected_indices.empty();
}

bool TestTryCommitUnresolvedDeleteIsRejected()
{
  // DeleteWire with unresolved net_id → eval surfaces
  // UnresolvedFootprint → try_commit rejects.
  r::PhysicalState state;
  auto snap = state.snapshot();

  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  // resolved_net_id and shape_kind absent
  r::ProposedDelta pd;
  pd.delta = del;
  pd.snapshot_version = snap.version();

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  return result.rejected_indices.size() == 1u
         && result.committed_indices.empty()
         && result.new_version == snap.version();
}

bool TestTryCommitDeleteResolvedRemovesShape()
{
  // Seed a shape, then commit a DeleteWire matching its identity.
  // After commit, the store has 0 shapes.
  r::PhysicalState state;
  state.mutable_store_for_test().SeedRouteShapes(
      {MakeFullShape(0, 0, 50, 5, 2, 100, 12)});
  auto snap = state.snapshot();
  const auto v_before = snap.version();

  r::DeleteWire del;
  del.segment_id = 1;
  del.bbox = r::Rect{r::Point{0, 0}, r::Point{50, 5}};
  del.layer = 2;
  del.resolved_net_id = 100;
  del.shape_kind = 12;
  r::ProposedDelta pd;
  pd.delta = del;
  pd.snapshot_version = v_before;

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  return result.committed_indices.size() == 1u
         && result.new_version == v_before + 1
         && state.mutable_store_for_test().route_shape_count() == 0u;
}

bool TestSnapshotGeometryReturnsView()
{
  // V2.2.d: Snapshot::geometry() now returns a real GeometryView
  // pointer (was nullptr in V2.1.d).
  r::PhysicalState state;
  state.mutable_store_for_test().SeedRouteShapes(
      {MakeFullShape(0, 0, 100, 5, 2, 100, 12)});
  auto snap = state.snapshot();
  const auto* view = snap.geometry();
  if (view == nullptr) {
    return false;
  }
  auto out = view->QueryRouteShapes(
      r::Rect{r::Point{-5, -5}, r::Point{200, 50}}, 2);
  return out.size() == 1u;
}

// ===== V2.2.e — MazeSearchProposer (synthetic) =====

bool TestProposerEmitsSingleAddWireProposal()
{
  // Input: target net 100, route_box (10..50, 0..5), layer 2.
  // Output: ProposedDelta wrapping AddWire with that bbox + layer.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  ro::MazeSearchProposer::Input in;
  in.net_id = 100;
  in.route_box = r::Rect{r::Point{10, 0}, r::Point{50, 5}};
  in.layer = 2;
  in.delta_id = r::DeltaId{1, 0, 0};
  in.snapshot_version = 1;

  auto pd = ro::MazeSearchProposer::Propose(*base, in);

  // Delta payload is AddWire with the input box + layer.
  if (!std::holds_alternative<r::AddWire>(pd.delta)) {
    return false;
  }
  const auto& aw = std::get<r::AddWire>(pd.delta);
  if (aw.bbox.ll.x != 10 || aw.bbox.ur.x != 50 || aw.layer != 2) {
    return false;
  }

  // ProposedDelta metadata.
  if (pd.source != r::DeltaSource::DetailedRoutePatch) {
    return false;
  }
  if (pd.snapshot_version != 1u) {
    return false;
  }
  if (!(pd.id == r::DeltaId{1, 0, 0})) {
    return false;
  }

  // Read footprint records the proposer's queried region.
  if (pd.read_footprint.geometry.rects.size() != 1u
      || pd.read_footprint.geometry.layers.size() != 1u
      || pd.read_footprint.geometry.layers[0] != 2) {
    return false;
  }

  // Write footprint sound (unknown=false), shapes/layers populated.
  return !pd.write_footprint.unknown
         && pd.write_footprint.shapes.size() == 1u
         && pd.write_footprint.layers.size() == 1u
         && pd.write_footprint.layers[0] == 2;
}

bool TestProposerToTryCommitEndToEnd()
{
  // Empty PhysicalState. Proposer emits an AddWire on layer 2.
  // try_commit_with_opts(SyntheticOracle) commits it. Store gains
  // 1 shape; version bumps.
  r::PhysicalState state;
  auto snap = state.snapshot();

  ro::MazeSearchProposer::Input in;
  in.net_id = 100;
  in.route_box = r::Rect{r::Point{10, 0}, r::Point{50, 5}};
  in.layer = 2;
  in.delta_id = r::DeltaId{1, 0, 0};
  in.snapshot_version = snap.version();

  auto pd = ro::MazeSearchProposer::Propose(*snap.geometry(), in);

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  return result.committed_indices.size() == 1u
         && result.rejected_indices.empty()
         && state.mutable_store_for_test().route_shape_count() == 1u;
}

bool TestProposerOverlapsBaseGetsRejected()
{
  // Seed a shape at (0..50, 0..5) layer 2. Proposer emits an
  // overlapping AddWire. Synthetic oracle says illegal; try_commit
  // rejects.
  r::PhysicalState state;
  state.mutable_store_for_test().SeedRouteShapes(
      {MakeFullShape(0, 0, 50, 5, 2, 100, 12)});
  auto snap = state.snapshot();

  ro::MazeSearchProposer::Input in;
  in.net_id = 200;
  in.route_box = r::Rect{r::Point{20, 0}, r::Point{30, 5}};  // overlap
  in.layer = 2;
  in.delta_id = r::DeltaId{1, 0, 0};
  in.snapshot_version = snap.version();

  auto pd = ro::MazeSearchProposer::Propose(*snap.geometry(), in);

  V22dStubPolicy policy;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto result
      = state.try_commit_with_opts(snap, {pd}, policy, opts);

  return result.rejected_indices.size() == 1u
         && result.committed_indices.empty()
         && state.mutable_store_for_test().route_shape_count() == 1u;
}

bool TestProposerStableDeltaIdAcrossCalls()
{
  // Two Propose calls with the same Input.delta_id produce
  // ProposedDelta values whose `id` field matches. Determinism
  // requirement from v2 §7.
  auto base = std::make_shared<ro::MemoryBackedGeometryView>(
      std::vector<ro::MarkerRef>{}, std::vector<ro::ShapeRef>{});

  ro::MazeSearchProposer::Input in;
  in.net_id = 100;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{10, 5}};
  in.layer = 2;
  in.delta_id = r::DeltaId{42, 7, 3};

  auto pd1 = ro::MazeSearchProposer::Propose(*base, in);
  auto pd2 = ro::MazeSearchProposer::Propose(*base, in);
  return pd1.id == pd2.id && pd1.id == r::DeltaId{42, 7, 3};
}

// ===== Compile-time absence of CanonicalTuple for V2.2+ entities =====
//
// V2.1.e shipped MarkerRef CanonicalTuple. V2.2.a.1 added ShapeRef.
// GuideRef / BlockageRef / PinAccessRef CanonicalTuples land in
// V2.2.a.{2,3,4} respectively.
static_assert(ro::has_canonical_tuple<ro::MarkerRef>::value,
              "MarkerRef must have CanonicalTuple");
static_assert(ro::has_canonical_tuple<ro::ShapeRef>::value,
              "ShapeRef must have CanonicalTuple after V2.2.a.1");
static_assert(ro::has_canonical_tuple<ro::GuideRef>::value,
              "GuideRef must have CanonicalTuple after V2.2.a.2");
static_assert(ro::has_canonical_tuple<ro::BlockageRef>::value,
              "BlockageRef must have CanonicalTuple after V2.2.a.3");
static_assert(ro::has_canonical_tuple<ro::PinAccessRef>::value,
              "PinAccessRef must have CanonicalTuple after V2.2.a.4");

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
      {"MemoryBackedGeometryView::QueryRouteShapes filters",
       TestMemoryBackedQueryRouteShapesFilters},
      {"MemoryBackedGeometryView::QueryGuides filters",
       TestMemoryBackedQueryGuidesFilters},
      {"GuideRef hash captures both layer endpoints",
       TestGuideRefHashCapturesBothLayerEndpoints},
      {"MemoryBackedGeometryView::QueryBlockages filters",
       TestMemoryBackedQueryBlockagesFilters},
      {"BlockageRef hash distinguishes PDK from inst-blockage",
       TestBlockageRefHashDistinguishesPdkFromInst},
      {"MemoryBackedGeometryView::QueryPinAccess filters",
       TestMemoryBackedQueryPinAccessFilters},
      {"RegionQueryGeometryView::QueryPinAccess intentionally throws (V2.2.a.4 B+C hybrid)",
       TestRegionQueryPinAccessStillThrows},
      {"PinAccessRef hash captures access flags",
       TestPinAccessRefHashCapturesAccessFlags},
      {"PinAccessRef hash absent flag != false flag",
       TestPinAccessRefHashAbsentVsFalseFlag},
      {"OverlayGeometryView empty deltas pass through",
       TestOverlayPassThroughEmptyDeltas},
      {"OverlayGeometryView adds AddWire to QueryRouteShapes",
       TestOverlayAddsAddWireToQueryRouteShapes},
      {"OverlayGeometryView composes Add with base",
       TestOverlayComposesAddWithBase},
      {"OverlayGeometryView stacks (overlay over overlay)",
       TestOverlayStacksOverlayOverOverlay},
      {"OverlayGeometryView InsertShield composes",
       TestOverlayInsertShieldComposes},
      {"OverlayGeometryView passes guides/markers through",
       TestOverlayPassesGuidesAndMarkersThrough},
      {"V2.2.b.del multiset: base=[A,A] del=[A] -> [A]",
       TestDeleteMultiset_BaseAA_DelA_ResultA},
      {"V2.2.b.del multiset: base=[A] del=[A,A] -> []",
       TestDeleteMultiset_BaseA_DelAA_ResultEmpty},
      {"V2.2.b.del multiset: base=[A,B] del=[A] -> [B]",
       TestDeleteMultiset_BaseAB_DelA_ResultB},
      {"V2.2.b.del multiset: del=[C] absent -> no effect",
       TestDeleteMultiset_DelAbsent_NoEffect},
      {"V2.2.b.del unresolved net_id is silent no-op",
       TestDeleteUnresolvedNetIdIsNoop},
      {"V2.2.b.del shape_kind mismatch does not delete",
       TestDeleteShapeKindMismatchDoesNotMatch},
      {"V2.2.b.del WriteFootprint::unknown reflects resolution",
       TestDeleteWriteFootprintUnknownWhenUnresolved},
      {"V2.2.c.proj eval(AddWire) wirelength_proxy = max(dx,dy)",
       TestEvalAddWireProducesPositiveWirelength},
      {"V2.2.c.proj eval(DeleteWire resolved) wirelength_proxy negative",
       TestEvalDeleteWireProducesNegativeWirelength},
      {"V2.2.c.proj eval(DeleteWire unresolved) UnresolvedFootprint + non-commit",
       TestEvalUnresolvedDeleteIdentitySurfaces},
      {"V2.2.c.proj eval(unresolved) score_even_if_illegal still non-commit",
       TestEvalUnresolvedDeleteScoreOnlyOnRequest},
      {"V2.2.c.proj eval(AddVia) increments via_count",
       TestEvalAddViaIncrementsViaCount},
      {"V2.2.d eval(Snapshot) invalid -> UnresolvedFootprint (was: throws in V2.2.c.proj)",
       TestEvalSnapshotOverloadInvalidSnapshotIsUnresolved},
      {"V2.2.c.bridge AddWire produces 1 Wire candidate",
       TestBridgeAddWireProducesOneCandidateWire},
      {"V2.2.c.bridge AddVia produces ViaCut candidate",
       TestBridgeAddViaProducesViaCutCandidate},
      {"V2.2.c.bridge InsertShield produces Shield candidate",
       TestBridgeInsertShieldProducesShieldCandidate},
      {"V2.2.c.bridge resolved DeleteWire goes to deleted_context",
       TestBridgeResolvedDeleteWireGoesToContext},
      {"V2.2.c.bridge unresolved Delete flags UnresolvedFootprint status",
       TestBridgeUnresolvedDeleteFlagsStatus},
      {"V2.2.c.bridge unsupported delta kind flags UnsupportedDelta status",
       TestBridgeUnsupportedDeltaKindFlagsStatus},
      {"V2.2.c.bridge LegalityMode enum exists, default = StubAssumeLegal",
       TestEvalOptionsLegalityModeDefaultsToStub},
      {"V2.2.c.legality.synthetic AddWire far-from-base is legal (commit-eligible)",
       TestSyntheticOracleAddWireFarFromBaseIsLegal},
      {"V2.2.c.legality.synthetic AddWire overlap base is illegal",
       TestSyntheticOracleAddWireOverlapBaseIsIllegal},
      {"V2.2.c.legality.synthetic different-layer always legal",
       TestSyntheticOracleAddWireOnDifferentLayerIsLegal},
      {"V2.2.c.legality.synthetic spacing violation detected",
       TestSyntheticOracleSpacingViolationDetected},
      {"V2.2.c.legality.synthetic default mode stays Stub (opt-in)",
       TestSyntheticOracleDefaultModeRemainsStub},
      {"V2.2.c.legality.synthetic CpuDrcRealDeck mode falls back to Stub",
       TestSyntheticOracleCpuDrcModeFallsBackToStub},
      {"V2.2.c.legality.synthetic IsTrustedLegalitySource rule",
       TestSyntheticOracleTrustedSourceRule},
      {"V2.2.d try_commit default mode rejects all (Stub)",
       TestTryCommitDefaultModeRejectsAll},
      {"V2.2.d try_commit_with_opts(SyntheticOracle) commits legal delta",
       TestTryCommitWithSyntheticCommitsLegalDelta},
      {"V2.2.d try_commit_with_opts(SyntheticOracle) rejects illegal delta",
       TestTryCommitWithSyntheticRejectsIllegalDelta},
      {"V2.2.d try_commit stale snapshot is flagged",
       TestTryCommitStaleSnapshotIsFlagged},
      {"V2.2.d try_commit unresolved delete is rejected",
       TestTryCommitUnresolvedDeleteIsRejected},
      {"V2.2.d try_commit resolved Delete removes shape",
       TestTryCommitDeleteResolvedRemovesShape},
      {"V2.2.d Snapshot::geometry() returns view (no longer nullptr)",
       TestSnapshotGeometryReturnsView},
      {"V2.2.e proposer emits 1 AddWire ProposedDelta (synthetic)",
       TestProposerEmitsSingleAddWireProposal},
      {"V2.2.e propose -> try_commit end-to-end (Synthetic)",
       TestProposerToTryCommitEndToEnd},
      {"V2.2.e propose overlapping base -> commit rejects",
       TestProposerOverlapsBaseGetsRejected},
      {"V2.2.e proposer DeltaId stable across calls",
       TestProposerStableDeltaIdAcrossCalls},
      {"SnapshotHandle holds GeometryView via shared_ptr",
       TestSnapshotHandleHoldsViewByShared},
      {"HashCanonicalRange order-insensitive",
       TestHashCanonicalRangeOrderInsensitive},
      {"HashCanonicalRange distinguishes content",
       TestHashCanonicalRangeDistinguishesContent},
      {"HashCanonicalRange distinguishes absent from zero",
       TestHashCanonicalRangeDistinguishesAbsentFromZero},
      {"HashCanonicalRange stable within a run",
       TestHashCanonicalRangeStableAcrossRuns},
      {"HashCanonicalRange empty range",
       TestHashCanonicalRangeEmptyRange},
      {"ShapeRef hash order-insensitive",
       TestShapeRefHashOrderInsensitive},
      {"ShapeRef hash distinguishes net_id",
       TestShapeRefHashDistinguishesNet},
      {"ShapeRef hash distinguishes absent from zero",
       TestShapeRefHashDistinguishesAbsentFromZero},
      {"ShapeRef hash distinct from MarkerRef hash for same bbox",
       TestShapeRefHashDifferentEntityFromMarker},
      {"ShadowDump counters increment without env var",
       TestShadowDumpCountersIncrement},
      {"ShadowDump disabled by default writes no file",
       TestShadowDumpDisabledByDefaultWritesNoFile},
      {"ShadowDump enabled writes header + row",
       TestShadowDumpEnabledWritesHeaderAndRow},
      {"ShadowDump bad path is best-effort, never throws",
       TestShadowDumpBadPathDoesNotThrow},
      {"ShadowDump route-shape row has entity + layer columns",
       TestShadowDumpRouteShapeRowHasEntityAndLayer},
      {"ShadowDump per-entity counters diverge correctly",
       TestShadowDumpPerEntityCounters},
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
