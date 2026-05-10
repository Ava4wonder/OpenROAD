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
#include "redesign/BatchSummaryDump.h"
#include "redesign/ConflictGraph.h"
#include "redesign/GreedyPriorityPolicy.h"
#include "redesign/PhysicalState.h"
#include "redesign/ProposalStaging.h"
#include "redesign/Score.h"
#include "redesign/Selection.h"
#include "redesign/CongestionTimingProvider.h"
#include "redesign/DriveGate.h"
#include "redesign/legality/CpuDrcOracle.h"
#include "redesign/legality/Predicates.h"
#include "redesign/overlay/GeometryView.h"
#include "redesign/overlay/Hashing.h"
#include "redesign/overlay/MazeSearchProposer.h"
#include "redesign/overlay/MemoryBackedGeometryView.h"
#include "redesign/overlay/MutableGeometryStore.h"
#include "redesign/overlay/OracleCandidate.h"
#include "redesign/overlay/OverlayGeometryView.h"
#include "redesign/overlay/RegionQueryGeometryView.h"
#include "redesign/overlay/RoutePerturbation.h"
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

// ===== V2.3.a — ProposalSet + batch eval =====

namespace {

// Helper: build a ProposedDelta for a synthetic AddWire with the
// given DeltaId and bbox. snapshot_version=0 (caller can override).
r::ProposedDelta MakeProposalAddWire(const r::DeltaId& id,
                                     int x1, int y1, int x2, int y2,
                                     r::LayerNum layer)
{
  r::AddWire add;
  add.bbox = r::Rect{r::Point{x1, y1}, r::Point{x2, y2}};
  add.layer = layer;
  r::ProposedDelta pd;
  pd.delta = add;
  pd.id = id;
  return pd;
}

}  // namespace

bool TestBatchEvalEmptySet()
{
  ro::MemoryBackedGeometryView base({});
  r::PhysicalState state;
  r::ProposalSet set;
  auto result = state.batch_eval(base, set);
  return result.outcomes.empty()
         && result.outcome_proposal_ids.empty()
         && result.summary.batch_size == 0u
         && result.summary.legal_count == 0u
         && result.summary.commit_eligible_count == 0u;
}

bool TestBatchEvalSingleProposalMatchesSingleEval()
{
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);

  // Single eval reference.
  auto single = state.eval(base, pd, opts);

  // Batch eval over 1.
  r::ProposalSet set;
  set.proposals.push_back(pd);
  auto batch = state.batch_eval(base, set, opts);

  if (batch.outcomes.size() != 1u
      || batch.outcome_proposal_ids.size() != 1u
      || batch.summary.batch_size != 1u) {
    return false;
  }
  return batch.outcomes[0].legality.source == single.legality.source
         && batch.outcomes[0].legality.legal == single.legality.legal
         && batch.outcomes[0].legality.commit_eligible
                == single.legality.commit_eligible;
}

bool TestBatchEvalMixedProposalsCountsCorrectly()
{
  // Mix:
  //  P1: AddWire far from any base shape          → legal
  //  P2: AddWire overlapping base                 → illegal
  //  P3: DeleteWire unresolved                    → UnresolvedFootprint
  //  P4: MoveCell                                 → UnsupportedDelta
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 50, 5, 2, 100, 12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  // Distinct DeltaIds so sort is well-defined.
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 200, 0, 300, 5, 2));  // legal
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 1}, 20, 0, 30, 5, 2));  // illegal
  // P3: DeleteWire unresolved
  r::DeleteWire del;
  del.segment_id = 9;
  // bbox absent / zero-area + no resolved_net_id → unresolved
  r::ProposedDelta p3;
  p3.delta = del;
  p3.id = r::DeltaId{1, 0, 2};
  set.proposals.push_back(p3);
  // P4: MoveCell → UnsupportedDelta
  r::MoveCell mv;
  mv.inst = nullptr;
  r::ProposedDelta p4;
  p4.delta = mv;
  p4.id = r::DeltaId{1, 0, 3};
  set.proposals.push_back(p4);

  auto batch = state.batch_eval(base, set, opts);

  return batch.outcomes.size() == 4u
         && batch.summary.batch_size == 4u
         && batch.summary.legal_count == 1u
         && batch.summary.commit_eligible_count == 1u
         && batch.summary.unresolved_count >= 1u   // p3 + p4 collapse
                                                    // to UnresolvedFootprint
                                                    // at the verdict layer
         && batch.summary.unsupported_count == 1u;  // p4 only
                                                    // at the bridge layer
}

bool TestBatchEvalSortsByDeltaIdRegardlessOfInputOrder()
{
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;

  auto pa = MakeProposalAddWire(r::DeltaId{2, 0, 0}, 0, 0, 10, 5, 2);
  auto pb = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 10, 5, 2);
  auto pc = MakeProposalAddWire(r::DeltaId{3, 0, 0}, 0, 0, 10, 5, 2);

  r::ProposalSet set1;
  set1.proposals = {pa, pb, pc};
  r::ProposalSet set2;
  set2.proposals = {pc, pa, pb};

  auto r1 = state.batch_eval(base, set1);
  auto r2 = state.batch_eval(base, set2);

  // Both must produce DeltaId-sorted output.
  if (r1.outcome_proposal_ids.size() != 3u
      || r2.outcome_proposal_ids.size() != 3u) {
    return false;
  }
  if (!(r1.outcome_proposal_ids[0] == r::DeltaId{1, 0, 0})
      || !(r1.outcome_proposal_ids[1] == r::DeltaId{2, 0, 0})
      || !(r1.outcome_proposal_ids[2] == r::DeltaId{3, 0, 0})) {
    return false;
  }
  return r2.outcome_proposal_ids == r1.outcome_proposal_ids;
}

bool TestBatchEvalTotalTimeNonNegative()
{
  // Coarse sanity: total_eval_time_ns should be >= 0 (steady_clock
  // is monotonic). Not a perf claim — just confirms timing wiring.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 4; ++i) {
    set.proposals.push_back(MakeProposalAddWire(
        r::DeltaId{1, 0, i}, 0, 0, 10, 5, 2));
  }
  auto batch = state.batch_eval(base, set);
  return batch.summary.total_eval_time_ns >= 0;
}

// ===== V2.3.b — SelectBest + RejectionReason =====

bool TestSelectBestEmptyBatchHasNoWinner()
{
  r::BatchEvalResult empty;
  auto sel = r::SelectBest(empty);
  return !sel.has_winner && sel.rejected.empty();
}

bool TestSelectBestSingleCommitEligibleWins()
{
  // One legal proposal under SyntheticOracle. It must be selected
  // and there must be no rejected entries.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  auto batch = state.batch_eval(base, set, opts);
  auto sel = r::SelectBest(batch);

  return sel.has_winner && sel.winner_id == r::DeltaId{1, 0, 0}
         && sel.winner_score.has_value() && sel.rejected.empty();
}

bool TestSelectBestNoCommitEligibleNoWinner()
{
  // All-stub legality (default opts) → nothing is commit_eligible.
  // Expect: no winner; every proposal tagged Unknown
  // (LegalitySource::StubAssumeLegal, legal=true, commit_eligible=false).
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;

  r::ProposalSet set;
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 1}, 0, 0, 100, 5, 2));
  auto batch = state.batch_eval(base, set);  // default = StubAssumeLegal
  auto sel = r::SelectBest(batch);

  if (sel.has_winner || sel.rejected.size() != 2u) {
    return false;
  }
  for (const auto& rj : sel.rejected) {
    if (rj.reason != r::RejectionReason::Unknown) {
      return false;
    }
  }
  return true;
}

bool TestSelectBestPicksHighestAggregateScore()
{
  // Two commit_eligible AddWires of different lengths. The longer
  // one has higher aggregate (delta_wirelength_proxy is positive)
  // and must win; the loser is tagged LowerScore.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  // Short wire (length 50).
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 50, 5, 2));
  // Long wire (length 500), placed far away to stay legal.
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 1}, 1000, 0, 1500, 5, 2));
  auto batch = state.batch_eval(base, set, opts);
  auto sel = r::SelectBest(batch);

  if (!sel.has_winner) {
    return false;
  }
  if (!(sel.winner_id == r::DeltaId{1, 0, 1})) {
    return false;
  }
  if (sel.rejected.size() != 1u) {
    return false;
  }
  return sel.rejected[0].id == r::DeltaId{1, 0, 0}
         && sel.rejected[0].reason == r::RejectionReason::LowerScore;
}

bool TestSelectBestTieBreaksByLowestDeltaId()
{
  // Two identical AddWires placed at different locations (far from
  // base shapes so both legal). Aggregate scores will be equal.
  // Tie-break: lowest DeltaId wins.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{2, 0, 0}, 1000, 0, 1100, 5, 2));
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 2000, 0, 2100, 5, 2));
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{3, 0, 0}, 3000, 0, 3100, 5, 2));
  auto batch = state.batch_eval(base, set, opts);
  auto sel = r::SelectBest(batch);

  if (!sel.has_winner) {
    return false;
  }
  // All three have equal aggregate; lowest DeltaId is {1,0,0}.
  if (!(sel.winner_id == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (sel.rejected.size() != 2u) {
    return false;
  }
  // Rejected list is in DeltaId-sorted order.
  if (!(sel.rejected[0].id == r::DeltaId{2, 0, 0})
      || !(sel.rejected[1].id == r::DeltaId{3, 0, 0})) {
    return false;
  }
  for (const auto& rj : sel.rejected) {
    if (rj.reason != r::RejectionReason::LowerScore) {
      return false;
    }
  }
  return true;
}

bool TestSelectBestClassifiesRejectionReasonsAcrossKinds()
{
  // Mix:
  //   P1: legal AddWire far from base    → winner
  //   P2: AddWire overlapping base       → Illegal (synthetic verdict)
  //   P3: DeleteWire unresolved identity → UnresolvedFootprint
  //   P4: MoveCell                       → UnsupportedDelta
  //   P5: legal AddWire shorter than P1  → LowerScore
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 50, 5, 2, 100, 12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  // P1 — long, legal.
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 1000, 0, 2000, 5, 2));
  // P2 — overlaps base (illegal).
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 1}, 20, 0, 30, 5, 2));
  // P3 — DeleteWire unresolved.
  r::DeleteWire del;
  del.segment_id = 9;
  r::ProposedDelta p3;
  p3.delta = del;
  p3.id = r::DeltaId{1, 0, 2};
  set.proposals.push_back(p3);
  // P4 — MoveCell, adapter UnsupportedDelta.
  r::MoveCell mv;
  mv.inst = nullptr;
  r::ProposedDelta p4;
  p4.delta = mv;
  p4.id = r::DeltaId{1, 0, 3};
  set.proposals.push_back(p4);
  // P5 — legal, shorter than P1.
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 4}, 3000, 0, 3010, 5, 2));

  auto batch = state.batch_eval(base, set, opts);
  auto sel = r::SelectBest(batch);

  if (!sel.has_winner || !(sel.winner_id == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (sel.rejected.size() != 4u) {
    return false;
  }

  // Expected DeltaId-sorted order:
  //   {1,0,1} Illegal
  //   {1,0,2} UnresolvedFootprint
  //   {1,0,3} UnsupportedDelta
  //   {1,0,4} LowerScore
  const std::pair<r::DeltaId, r::RejectionReason> expected[] = {
      {r::DeltaId{1, 0, 1}, r::RejectionReason::Illegal},
      {r::DeltaId{1, 0, 2}, r::RejectionReason::UnresolvedFootprint},
      {r::DeltaId{1, 0, 3}, r::RejectionReason::UnsupportedDelta},
      {r::DeltaId{1, 0, 4}, r::RejectionReason::LowerScore},
  };
  for (std::size_t i = 0; i < 4; ++i) {
    if (!(sel.rejected[i].id == expected[i].first)) {
      return false;
    }
    if (sel.rejected[i].reason != expected[i].second) {
      return false;
    }
  }
  return true;
}

bool TestSelectBestDeterministicAcrossInputPermutations()
{
  // Same proposals in two different input orders — selection
  // results must be identical (winner DeltaId, rejected list, all
  // reasons).
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  auto pa = MakeProposalAddWire(r::DeltaId{2, 0, 0}, 1000, 0, 1500, 5, 2);
  auto pb = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 2000, 0, 2050, 5, 2);
  auto pc = MakeProposalAddWire(r::DeltaId{3, 0, 0}, 3000, 0, 3500, 5, 2);

  r::ProposalSet set1;
  set1.proposals = {pa, pb, pc};
  r::ProposalSet set2;
  set2.proposals = {pc, pa, pb};

  auto sel1 = r::SelectBest(state.batch_eval(base, set1, opts));
  auto sel2 = r::SelectBest(state.batch_eval(base, set2, opts));

  if (sel1.has_winner != sel2.has_winner) {
    return false;
  }
  if (!(sel1.winner_id == sel2.winner_id)) {
    return false;
  }
  if (sel1.rejected.size() != sel2.rejected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < sel1.rejected.size(); ++i) {
    if (!(sel1.rejected[i].id == sel2.rejected[i].id)
        || sel1.rejected[i].reason != sel2.rejected[i].reason) {
      return false;
    }
  }
  return true;
}

// ===== V2.3.b.dump — BatchSummaryDump =====

bool TestBatchSummaryRowFromMixedBatch()
{
  // Build the same mixed-batch as
  // TestSelectBestClassifiesRejectionReasonsAcrossKinds and verify
  // MakeBatchSummaryRow populates the per-reason counters and the
  // winner fields correctly.
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 50, 5, 2, 100, 12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 0}, 1000, 0, 2000, 5, 2));  // win
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 1}, 20, 0, 30, 5, 2));  // illegal
  r::DeleteWire del;
  del.segment_id = 9;
  r::ProposedDelta p3;
  p3.delta = del;
  p3.id = r::DeltaId{1, 0, 2};
  set.proposals.push_back(p3);  // unresolved
  r::MoveCell mv;
  r::ProposedDelta p4;
  p4.delta = mv;
  p4.id = r::DeltaId{1, 0, 3};
  set.proposals.push_back(p4);  // unsupported
  set.proposals.push_back(
      MakeProposalAddWire(r::DeltaId{1, 0, 4}, 3000, 0, 3010, 5, 2));  // lower

  auto batch = state.batch_eval(base, set, opts);
  auto sel = r::SelectBest(batch);

  const std::uint64_t kNetId = 42;
  const std::uint64_t kSeqno = 7;
  auto row = r::MakeBatchSummaryRow(batch, sel, kNetId, kSeqno);

  if (row.net_id != kNetId || row.seqno != kSeqno
      || row.batch_size != 5u) {
    return false;
  }
  if (!row.has_winner || !(row.winner_delta_id == r::DeltaId{1, 0, 0})) {
    return false;
  }
  // Expect commit_eligible_count == 2 (winner + the LowerScore loser).
  if (row.num_commit_eligible != 2u) {
    return false;
  }
  if (row.num_illegal != 1u || row.num_unresolved != 1u
      || row.num_unsupported != 1u || row.num_lower_score != 1u
      || row.num_conflict != 0u) {
    return false;
  }
  // Winner score should be > 0 (Manhattan length proxy for a 1000-unit
  // wire).
  return row.winner_score > 0.0;
}

bool TestBatchSummaryRowEmptyBatchHasNoWinner()
{
  r::BatchEvalResult empty;
  r::SelectionResult sel = r::SelectBest(empty);
  auto row = r::MakeBatchSummaryRow(empty, sel, /*net_id=*/0,
                                    /*seqno=*/1);
  return !row.has_winner && row.batch_size == 0u
         && row.num_commit_eligible == 0u && row.num_illegal == 0u
         && row.num_lower_score == 0u && row.num_conflict == 0u
         && row.EncodeWinnerDeltaId().empty();
}

bool TestBatchSummaryRowEncodeWinnerDeltaId()
{
  r::BatchSummaryRow row;
  row.has_winner = true;
  row.winner_delta_id = r::DeltaId{3, 7, 11};
  return row.EncodeWinnerDeltaId() == "3:7:11";
}

bool TestBatchSummaryDumpCountersUpdateWithoutEnv()
{
  // Counters update regardless of env-var enable.
  r::BatchSummaryDump::ResetForTest();
  r::BatchSummaryRow row;
  row.batch_size = 4;
  row.has_winner = true;
  row.num_illegal = 1;
  row.num_unresolved = 1;
  row.num_lower_score = 1;
  r::BatchSummaryDump::Record(row);

  if (r::BatchSummaryDump::batch_count() != 1u) {
    return false;
  }
  if (r::BatchSummaryDump::batches_with_winner() != 1u) {
    return false;
  }
  if (r::BatchSummaryDump::total_proposals() != 4u) {
    return false;
  }
  if (r::BatchSummaryDump::total_rejection_count_for(
          r::RejectionReason::Illegal)
      != 1u) {
    return false;
  }
  if (r::BatchSummaryDump::total_rejection_count_for(
          r::RejectionReason::LowerScore)
      != 1u) {
    return false;
  }
  return true;
}

bool TestBatchSummaryDumpEnabledWritesHeaderAndRow()
{
  r::BatchSummaryDump::ResetForTest();

  // Direct a unique path so concurrent test runs don't collide.
  std::ostringstream path_os;
  path_os << "/tmp/openroad-redesign-batch-test/" << ::getpid()
          << "_summary.csv";
  const std::string path = path_os.str();
  std::remove(path.c_str());

  ::setenv("OPENROAD_REDESIGN_BATCH_DUMP", "1", /*overwrite=*/1);
  ::setenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH", path.c_str(),
           /*overwrite=*/1);

  r::BatchSummaryRow row;
  row.seqno = 99;
  row.net_id = 12345;
  row.batch_size = 3;
  row.has_winner = true;
  row.winner_delta_id = r::DeltaId{1, 2, 3};
  row.winner_score = 42.0;
  row.num_commit_eligible = 2;
  row.num_lower_score = 1;
  r::BatchSummaryDump::Record(row);

  // Read back.
  std::ifstream f(path);
  if (!f.is_open()) {
    ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP");
    ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH");
    return false;
  }
  std::string header;
  std::getline(f, header);
  std::string data;
  std::getline(f, data);
  f.close();
  ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP");
  ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH");
  std::remove(path.c_str());
  r::BatchSummaryDump::ResetForTest();

  // Header must contain the exact column names from the schema.
  if (header.find("pid") == std::string::npos
      || header.find("net_id") == std::string::npos
      || header.find("winner_delta_id") == std::string::npos
      || header.find("num_conflict") == std::string::npos) {
    return false;
  }
  // Data row must contain net_id, encoded winner DeltaId, and the
  // explicit per-reason columns.
  return data.find(",12345,") != std::string::npos
         && data.find(",1:2:3,") != std::string::npos
         && data.find(",42") != std::string::npos;
}

bool TestBatchSummaryDumpBadPathDoesNotThrow()
{
  r::BatchSummaryDump::ResetForTest();
  ::setenv("OPENROAD_REDESIGN_BATCH_DUMP", "1", /*overwrite=*/1);
  ::setenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH",
           "/etc/hostname/never_writable_subdir/dump.csv",
           /*overwrite=*/1);
  bool threw = false;
  try {
    r::BatchSummaryRow row;
    row.batch_size = 1;
    r::BatchSummaryDump::Record(row);
  } catch (...) {
    threw = true;
  }
  ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP");
  ::unsetenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH");
  r::BatchSummaryDump::ResetForTest();
  return !threw;
}

// ===== V2.4.a — ConflictGraph =====

namespace {

// Build a ProposedDelta around an AddWire whose write_footprint is
// derived from the AddWire bbox + layer (sound, unknown=false). Used
// to drive geometry-conflict tests with controlled overlap patterns.
r::ProposedDelta MakeProposalAddWireWithFootprint(const r::DeltaId& id,
                                                  int x1, int y1,
                                                  int x2, int y2,
                                                  r::LayerNum layer)
{
  auto pd = MakeProposalAddWire(id, x1, y1, x2, y2, layer);
  pd.write_footprint = r::WriteFootprint::Of(pd.delta);
  return pd;
}

}  // namespace

bool TestConflictGraphEmptySetIsEmpty()
{
  r::ProposalSet set;
  auto g = r::BuildConflictGraph(set);
  return g.node_ids.empty() && g.edges.empty();
}

bool TestConflictGraphSingleProposalHasNoEdges()
{
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  auto g = r::BuildConflictGraph(set);
  return g.node_ids.size() == 1u && g.edges.empty();
}

bool TestConflictGraphDisjointProposalsHaveNoEdges()
{
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 1000, 0, 1100, 5, 2));
  auto g = r::BuildConflictGraph(set);
  return g.node_ids.size() == 2u && g.edges.empty();
}

bool TestConflictGraphOverlappingSameLayerHasOneEdge()
{
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 2));  // overlaps in [50,100]
  auto g = r::BuildConflictGraph(set);
  if (g.edges.size() != 1u) {
    return false;
  }
  return g.edges[0].a == 0u && g.edges[0].b == 1u
         && g.edges[0].kind == r::ConflictKind::Geometry;
}

bool TestConflictGraphOverlappingDifferentLayerHasNoEdge()
{
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));  // layer 2
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 3));  // layer 3, same bbox region
  auto g = r::BuildConflictGraph(set);
  return g.edges.empty();
}

bool TestConflictGraphTouchingEdgesCount()
{
  // Mirror SyntheticOracle::BboxOverlap convention — touching counts.
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 100, 0, 200, 5, 2));  // shares x=100 edge
  auto g = r::BuildConflictGraph(set);
  return g.edges.size() == 1u
         && g.edges[0].kind == r::ConflictKind::Geometry;
}

bool TestConflictGraphUnknownFootprintConflictsWithEverything()
{
  // SAFETY INVARIANT: write_footprint.unknown=true MUST conflict with
  // every other proposal. MoveCell yields unknown=true.
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  // P1: legal AddWire far away — by itself no conflict with P0.
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 5000, 0, 5100, 5, 2));
  // P2: MoveCell → write_footprint.unknown=true.
  r::MoveCell mv;
  r::ProposedDelta p2;
  p2.delta = mv;
  p2.id = r::DeltaId{1, 0, 2};
  p2.write_footprint = r::WriteFootprint::Of(p2.delta);
  set.proposals.push_back(p2);

  auto g = r::BuildConflictGraph(set);
  // P2 conflicts with P0 and P1 → 2 edges. P0/P1 do not conflict
  // with each other → 0 edges. Total = 2.
  if (g.edges.size() != 2u) {
    return false;
  }
  // Edges are (a, b) sorted: (0, 2) before (1, 2).
  return g.edges[0].a == 0u && g.edges[0].b == 2u
         && g.edges[1].a == 1u && g.edges[1].b == 2u;
}

bool TestConflictGraphCompleteOverlapKEqualsFour()
{
  // The V2.3.c synthetic K=4 pattern: same bbox shrunk on ur.x.
  // All four pairwise overlap on layer 2 → complete graph K_4 has
  // C(4,2) = 6 edges.
  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 4; ++i) {
    set.proposals.push_back(MakeProposalAddWireWithFootprint(
        r::DeltaId{1, 0, i}, 0, 0,
        1000 - static_cast<int>(i), 5, 2));
  }
  auto g = r::BuildConflictGraph(set);
  if (g.node_ids.size() != 4u || g.edges.size() != 6u) {
    return false;
  }
  // Edges in (a, b) sorted order: (0,1)(0,2)(0,3)(1,2)(1,3)(2,3).
  const std::pair<std::size_t, std::size_t> expected[] = {
      {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
  for (std::size_t k = 0; k < 6; ++k) {
    if (g.edges[k].a != expected[k].first
        || g.edges[k].b != expected[k].second
        || g.edges[k].kind != r::ConflictKind::Geometry) {
      return false;
    }
  }
  return true;
}

bool TestConflictGraphDeterministicAcrossRebuilds()
{
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 2}, 1000, 0, 1100, 5, 2));
  auto g1 = r::BuildConflictGraph(set);
  auto g2 = r::BuildConflictGraph(set);
  if (g1.edges.size() != g2.edges.size()) {
    return false;
  }
  for (std::size_t i = 0; i < g1.edges.size(); ++i) {
    if (!(g1.edges[i] == g2.edges[i])) {
      return false;
    }
  }
  return true;
}

// ===== V2.4.b — Net + ReadWrite conflict =====

bool TestConflictGraphSameNetDifferentBboxNetEdge()
{
  // Two AddWires on the same net but spatially disjoint and on
  // DIFFERENT layers. Geometry test fails (different layer); Net
  // test fires.
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.proposal_net_id = 17;
  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 1000, 0, 1100, 5, 3);
  pb.proposal_net_id = 17;
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.size() == 1u
         && g.edges[0].kind == r::ConflictKind::Net
         && g.edges[0].a == 0u && g.edges[0].b == 1u;
}

bool TestConflictGraphDifferentNetsNoNetEdge()
{
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.proposal_net_id = 17;
  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 1000, 0, 1100, 5, 3);
  pb.proposal_net_id = 99;
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.empty();
}

bool TestConflictGraphZeroNetIdNeverMatches()
{
  // Both proposals have net_id == 0 (synthetic / unset). Must NOT
  // produce a Net edge — otherwise every synthetic test would
  // collide.
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  // pa.proposal_net_id = 0 (default)
  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 1000, 0, 1100, 5, 3);
  // pb.proposal_net_id = 0 (default)
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.empty();
}

bool TestConflictGraphReadOverlapsWriteEdge()
{
  // A reads (10, 0)-(50, 5) layer 2; B writes (30, 0)-(60, 5)
  // layer 2 (placed elsewhere so no write-write collision; use a
  // different layer for the actual write). Different layers for
  // write to keep it clean: B writes (30,0)-(60,5) on layer 3,
  // A reads on layer 2 — so we need to also read from layer 3 to
  // see the conflict.
  //
  // Cleaner setup: A writes layer 2 (and also reads layer 3 via
  // explicit footprint), B writes layer 3 disjoint from A's write.
  // A's read on layer 3 overlaps B's write on layer 3 → ReadWrite.
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.read_footprint.geometry.rects.push_back(
      r::Rect{r::Point{200, 0}, r::Point{400, 5}});
  pa.read_footprint.geometry.layers.push_back(3);

  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 300, 0, 500, 5, 3);
  // No net match (default 0). Write disjoint from A's write
  // (different layers anyway).
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.size() == 1u
         && g.edges[0].kind == r::ConflictKind::ReadWrite;
}

bool TestConflictGraphReadDifferentLayerNoEdge()
{
  // A reads layer 5; B writes layer 3 in the same xy region.
  // Different layer → no ReadWrite edge.
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.read_footprint.geometry.rects.push_back(
      r::Rect{r::Point{200, 0}, r::Point{400, 5}});
  pa.read_footprint.geometry.layers.push_back(5);

  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 300, 0, 500, 5, 3);
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.empty();
}

bool TestConflictGraphPrecedenceGeometryWinsOverNet()
{
  // A and B overlap on layer 2 AND share the same net. Per
  // precedence Geometry > Net, the edge kind must be Geometry.
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.proposal_net_id = 17;
  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 2);
  pb.proposal_net_id = 17;
  set.proposals.push_back(pa);
  set.proposals.push_back(pb);
  auto g = r::BuildConflictGraph(set);
  return g.edges.size() == 1u
         && g.edges[0].kind == r::ConflictKind::Geometry;
}

bool TestConflictGraphMixedThreeKindsInOneBatch()
{
  // A: writes (0,0)-(100,5) layer 2,    net=17, reads layer 4 at (1000, 0)-(1100, 5)
  // B: writes (50,0)-(150,5) layer 2,   net=99               <- (A,B) Geometry
  // C: writes (5000,0)-(5100,5) layer 2, net=17              <- (A,C) Net
  //    (disjoint from A spatially, same net as A)
  // D: writes (1000,0)-(1100,5) layer 4, net=42              <- (A,D) ReadWrite
  //    (disjoint spatially+layer from A's write, but A reads its layer 4 region)
  // (B,C), (B,D), (C,D) — none of those should fire (verify too).
  r::ProposalSet set;
  auto pa = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pa.proposal_net_id = 17;
  pa.read_footprint.geometry.rects.push_back(
      r::Rect{r::Point{1000, 0}, r::Point{1100, 5}});
  pa.read_footprint.geometry.layers.push_back(4);

  auto pb = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 2);
  pb.proposal_net_id = 99;

  auto pc = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 2}, 5000, 0, 5100, 5, 2);
  pc.proposal_net_id = 17;

  auto pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 3}, 1000, 0, 1100, 5, 4);
  pd.proposal_net_id = 42;

  set.proposals = {pa, pb, pc, pd};
  auto g = r::BuildConflictGraph(set);
  if (g.edges.size() != 3u) {
    return false;
  }
  // Edges in (a, b) sorted order: (0,1), (0,2), (0,3).
  return g.edges[0].a == 0u && g.edges[0].b == 1u
         && g.edges[0].kind == r::ConflictKind::Geometry
         && g.edges[1].a == 0u && g.edges[1].b == 2u
         && g.edges[1].kind == r::ConflictKind::Net
         && g.edges[2].a == 0u && g.edges[2].b == 3u
         && g.edges[2].kind == r::ConflictKind::ReadWrite;
}

bool TestProposerSetsProposalNetId()
{
  // Verify MazeSearchProposer propagates Input.net_id to
  // ProposedDelta.proposal_net_id (V2.4.b extension).
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 4242;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{100, 5}};
  in.layer = 2;
  in.delta_id = r::DeltaId{1, 0, 0};
  auto pd = ro::MazeSearchProposer::Propose(base, in);
  return pd.proposal_net_id == 4242u;
}

// ===== V2.4.c — GreedyPriorityPolicy =====

namespace {

// Helper: assemble a ScoredProposal vector inline from
// (DeltaId, aggregate-score) pairs. Other Score fields are zeroed —
// the policy only consumes Score::aggregate.
std::vector<r::ScoredProposal> MakeScored(
    std::initializer_list<std::pair<r::DeltaId, double>> entries)
{
  std::vector<r::ScoredProposal> out;
  out.reserve(entries.size());
  for (const auto& e : entries) {
    r::ScoredProposal sp;
    sp.proposal.id = e.first;
    sp.legality.legal = true;
    sp.legality.commit_eligible = true;
    sp.score.aggregate = e.second;
    out.push_back(sp);
  }
  return out;
}

}  // namespace

bool TestGreedyEmptyInputReturnsEmpty()
{
  r::GreedyPriorityPolicy p;
  auto sel = p.select({}, {});
  return sel.empty();
}

bool TestGreedySingleProposalNoEdgesIsSelected()
{
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 100.0}});
  auto sel = p.select(scored, {});
  return sel.size() == 1u && sel[0] == 0u;
}

bool TestGreedyTwoIndependentBothSelected()
{
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 100.0},
                            {r::DeltaId{1, 0, 1}, 50.0}});
  auto sel = p.select(scored, {});
  return sel.size() == 2u && sel[0] == 0u && sel[1] == 1u;
}

bool TestGreedyOneEdgeHigherScoreWins()
{
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 50.0},
                            {r::DeltaId{1, 0, 1}, 100.0}});
  // Edge between (0, 1) → only one survives; index 1 has higher
  // score and wins.
  auto sel = p.select(scored, {{0, 1}});
  return sel.size() == 1u && sel[0] == 1u;
}

bool TestGreedyEqualScoreLowerDeltaIdWins()
{
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{2, 0, 0}, 50.0},
                            {r::DeltaId{1, 0, 0}, 50.0}});
  // Both score=50. Edge (0,1) → only one wins; tie broken by
  // ascending DeltaId. DeltaId{1,0,0} < {2,0,0}, so index 1 wins.
  auto sel = p.select(scored, {{0, 1}});
  return sel.size() == 1u && sel[0] == 1u;
}

bool TestGreedyChainPicksEnds()
{
  // A-B-C chain (edges (0,1) and (1,2)). Equal scores. Greedy picks
  // by priority order; equal scores break by DeltaId. With
  // DeltaIds {1,0,0}, {1,0,1}, {1,0,2} the priority order is
  // 0, 1, 2. Greedy admits 0, blocks 1 (edge to 0), admits 2 (no
  // edge to remaining selected = {0}).
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 10.0},
                            {r::DeltaId{1, 0, 1}, 10.0},
                            {r::DeltaId{1, 0, 2}, 10.0}});
  auto sel = p.select(scored, {{0, 1}, {1, 2}});
  return sel.size() == 2u && sel[0] == 0u && sel[1] == 2u;
}

bool TestGreedyHighScoreMiddleAlwaysAdmitted()
{
  // Same chain, but the middle node has the highest score. Greedy
  // admits the middle first, blocks both ends.
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 1.0},
                            {r::DeltaId{1, 0, 1}, 100.0},
                            {r::DeltaId{1, 0, 2}, 1.0}});
  auto sel = p.select(scored, {{0, 1}, {1, 2}});
  return sel.size() == 1u && sel[0] == 1u;
}

bool TestGreedyDeterministicAcrossEdgePermutations()
{
  // Same scored vector; edge list given in two different
  // orderings. Output must match.
  r::GreedyPriorityPolicy p;
  auto scored = MakeScored({{r::DeltaId{1, 0, 0}, 30.0},
                            {r::DeltaId{1, 0, 1}, 50.0},
                            {r::DeltaId{1, 0, 2}, 40.0},
                            {r::DeltaId{1, 0, 3}, 10.0}});
  auto sel1 = p.select(scored, {{0, 1}, {1, 2}, {2, 3}});
  auto sel2 = p.select(scored, {{2, 3}, {0, 1}, {1, 2}});
  // Deterministic regardless of edge-list permutation.
  return sel1 == sel2;
}

bool TestGreedyMakeEdgeListFromConflictGraph()
{
  // Round-trip: ConflictGraph → MakeEdgeList → vector<pair> with
  // the same edges in the same canonical order.
  r::ProposalSet set;
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 50, 0, 150, 5, 2));
  set.proposals.push_back(MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 2}, 1000, 0, 1100, 5, 2));
  auto g = r::BuildConflictGraph(set);
  auto edges = r::MakeEdgeList(g);
  return edges.size() == 1u && edges[0].first == 0u
         && edges[0].second == 1u;
}

bool TestGreedyEndToEndConflictGraphPlusSolver()
{
  // Full V2.4.a→b→c chain: build proposals with overlapping
  // bboxes, build ConflictGraph, flatten to edges, run greedy.
  // K=4 mutual overlap + descending scores. Greedy admits proposal
  // 0 (highest score), blocks all the others (complete graph).
  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 4; ++i) {
    set.proposals.push_back(MakeProposalAddWireWithFootprint(
        r::DeltaId{1, 0, i}, 0, 0,
        1000 - static_cast<int>(i), 5, 2));
  }
  auto g = r::BuildConflictGraph(set);
  auto edges = r::MakeEdgeList(g);

  // Score by aggregate = wirelength. Index 0 is widest (1000) so
  // highest aggregate.
  std::vector<r::ScoredProposal> scored;
  for (std::size_t i = 0; i < set.proposals.size(); ++i) {
    r::ScoredProposal sp;
    sp.proposal = set.proposals[i];
    sp.legality.legal = true;
    sp.legality.commit_eligible = true;
    sp.score.aggregate = 1000.0 - static_cast<double>(i);
    scored.push_back(sp);
  }
  r::GreedyPriorityPolicy p;
  auto sel = p.select(scored, edges);
  return sel.size() == 1u && sel[0] == 0u
         && p.kind() == r::PolicyKind::GreedyPriority;
}

// ===== V2.4.d — SelectMis (MIS-based multi-Delta selection) =====

bool TestSelectMisEmptyBatchEmptyResult()
{
  r::BatchEvalResult batch;
  r::ProposalSet set;
  r::ConflictGraph g;
  r::GreedyPriorityPolicy p;
  auto sel = r::SelectMis(batch, set, g, p);
  return sel.selected_ids.empty() && sel.rejected.empty();
}

bool TestSelectMisAllEligibleNoConflictsAllSelected()
{
  // Three disjoint AddWires, all commit_eligible (synthetic
  // oracle), no conflicts. All three survive MIS.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto pd = MakeProposalAddWireWithFootprint(
        r::DeltaId{1, 0, i}, 1000 * static_cast<int>(i + 1),
        0, 1000 * static_cast<int>(i + 1) + 100, 5, 2);
    set.proposals.push_back(pd);
  }
  auto batch = state.batch_eval(base, set, opts);
  auto graph = r::BuildConflictGraph(set);
  r::GreedyPriorityPolicy p;
  auto sel = r::SelectMis(batch, set, graph, p);
  return sel.selected_ids.size() == 3u && sel.rejected.empty();
}

bool TestSelectMisCompleteOverlapKEqualsFourOnlyOneSelected()
{
  // V2.3.c synthetic K=4: same bbox shrunk on ur.x. All overlap
  // pairwise → complete K_4. Only the highest-aggregate (widest)
  // survives; the other 3 are tagged Conflict.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 4; ++i) {
    auto pd = MakeProposalAddWireWithFootprint(
        r::DeltaId{1, 0, i}, 0, 0,
        1000 - static_cast<int>(i), 5, 2);
    set.proposals.push_back(pd);
  }
  auto batch = state.batch_eval(base, set, opts);
  auto graph = r::BuildConflictGraph(set);
  r::GreedyPriorityPolicy p;
  auto sel = r::SelectMis(batch, set, graph, p);

  if (sel.selected_ids.size() != 1u) {
    return false;
  }
  if (!(sel.selected_ids[0] == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (sel.rejected.size() != 3u) {
    return false;
  }
  for (const auto& rj : sel.rejected) {
    if (rj.reason != r::RejectionReason::Conflict) {
      return false;
    }
  }
  return true;
}

bool TestSelectMisIllegalAndConflictMixedReasons()
{
  // Mix:
  //   P0: legal AddWire far from base, no neighbour conflict      → selected
  //   P1: legal AddWire overlapping P0 spatially                   → Conflict (loses MIS to P0)
  //   P2: illegal AddWire (overlaps base shape)                    → Illegal
  //   P3: MoveCell (UnsupportedDelta)                              → UnsupportedDelta
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 50, 5, 2, 100, 12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  // P0 — long, legal, far from base.
  auto p0 = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 1000, 0, 2000, 5, 2);
  set.proposals.push_back(p0);
  // P1 — long, legal, overlaps P0 spatially → Conflict.
  auto p1 = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 1}, 1500, 0, 2500, 5, 2);
  set.proposals.push_back(p1);
  // P2 — overlaps base (illegal).
  auto p2 = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 2}, 20, 0, 30, 5, 2);
  set.proposals.push_back(p2);
  // P3 — MoveCell, UnsupportedDelta.
  r::MoveCell mv;
  r::ProposedDelta p3;
  p3.delta = mv;
  p3.id = r::DeltaId{1, 0, 3};
  p3.write_footprint = r::WriteFootprint::Of(p3.delta);
  set.proposals.push_back(p3);

  auto batch = state.batch_eval(base, set, opts);
  auto graph = r::BuildConflictGraph(set);
  r::GreedyPriorityPolicy p;
  auto sel = r::SelectMis(batch, set, graph, p);

  // Wait — P0 and P1 score the same (both 1000-wide). Tie-break
  // by lower DeltaId → P0 wins.
  if (sel.selected_ids.size() != 1u
      || !(sel.selected_ids[0] == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (sel.rejected.size() != 3u) {
    return false;
  }
  // Expected DeltaId-sorted rejected order:
  //   {1,0,1} Conflict
  //   {1,0,2} Illegal
  //   {1,0,3} UnresolvedFootprint  (P3 is UnsupportedDelta
  //                                   adapter-side; verdict source
  //                                   collapses to UnresolvedFootprint
  //                                   which is one of the "first-tag"
  //                                   reasons — see ClassifyNonEligible
  //                                   precedence: adapter_unsupported
  //                                   wins, so this is UnsupportedDelta)
  const std::pair<r::DeltaId, r::RejectionReason> expected[] = {
      {r::DeltaId{1, 0, 1}, r::RejectionReason::Conflict},
      {r::DeltaId{1, 0, 2}, r::RejectionReason::Illegal},
      {r::DeltaId{1, 0, 3}, r::RejectionReason::UnsupportedDelta},
  };
  for (std::size_t i = 0; i < 3; ++i) {
    if (!(sel.rejected[i].id == expected[i].first)
        || sel.rejected[i].reason != expected[i].second) {
      return false;
    }
  }
  return true;
}

bool TestSelectMisDeterministicAcrossPolicyInstances()
{
  // Two GreedyPriorityPolicy instances on the same batch + graph
  // produce identical results.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  for (std::uint32_t i = 0; i < 4; ++i) {
    set.proposals.push_back(MakeProposalAddWireWithFootprint(
        r::DeltaId{1, 0, i}, 0, 0,
        1000 - static_cast<int>(i), 5, 2));
  }
  auto batch = state.batch_eval(base, set, opts);
  auto graph = r::BuildConflictGraph(set);
  r::GreedyPriorityPolicy p1, p2;
  auto s1 = r::SelectMis(batch, set, graph, p1);
  auto s2 = r::SelectMis(batch, set, graph, p2);
  if (s1.selected_ids.size() != s2.selected_ids.size()) {
    return false;
  }
  for (std::size_t i = 0; i < s1.selected_ids.size(); ++i) {
    if (!(s1.selected_ids[i] == s2.selected_ids[i])) {
      return false;
    }
  }
  if (s1.rejected.size() != s2.rejected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < s1.rejected.size(); ++i) {
    if (!(s1.rejected[i].id == s2.rejected[i].id)
        || s1.rejected[i].reason != s2.rejected[i].reason) {
      return false;
    }
  }
  return true;
}

// ===== V2.4.e — ProposalStaging cross-worker barrier =====

namespace {

// Helper: build a StagedProposal from a synthetic AddWire +
// commit_eligible SyntheticOracle outcome. region_id varies per
// "worker" so DeltaIds are globally unique.
r::StagedProposal MakeStagedAddWire(std::uint32_t region_id,
                                    std::uint32_t attempt_index,
                                    int x1, int y1, int x2, int y2,
                                    r::LayerNum layer,
                                    bool commit_eligible)
{
  r::ProposedDelta pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{region_id, 0, attempt_index}, x1, y1, x2, y2, layer);
  r::StagedProposal sp;
  sp.proposal = pd;
  sp.outcome.legality.legal = commit_eligible;
  sp.outcome.legality.commit_eligible = commit_eligible;
  sp.outcome.legality.source = commit_eligible
                                   ? r::LegalitySource::SyntheticOracle
                                   : r::LegalitySource::SyntheticOracle;
  r::Score s;
  s.aggregate = static_cast<double>(x2 - x1);  // wirelength proxy
  sp.outcome.score = s;
  sp.adapter_unsupported = false;
  return sp;
}

}  // namespace

bool TestStagingDrainEmpty()
{
  r::ProposalStaging::Instance().ResetForTest();
  auto drained = r::ProposalStaging::Instance().Drain();
  return drained.empty()
         && r::ProposalStaging::Instance().size() == 0u;
}

bool TestStagingPushDrainSize()
{
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 100, 5, 2, /*commit_eligible=*/true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 1000, 0, 1100, 5, 2, /*commit_eligible=*/true));
  if (r::ProposalStaging::Instance().size() != 2u) {
    return false;
  }
  auto drained = r::ProposalStaging::Instance().Drain();
  if (r::ProposalStaging::Instance().size() != 0u) {
    return false;
  }
  return drained.size() == 2u;
}

bool TestStagingDrainSortsByDeltaId()
{
  // Push out of DeltaId order; Drain must DeltaId-sort.
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      3, 0, 0, 0, 100, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 100, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 0, 0, 100, 5, 2, true));
  auto drained = r::ProposalStaging::Instance().Drain();
  if (drained.size() != 3u) {
    return false;
  }
  return drained[0].proposal.id == r::DeltaId{1, 0, 0}
         && drained[1].proposal.id == r::DeltaId{2, 0, 0}
         && drained[2].proposal.id == r::DeltaId{3, 0, 0};
}

bool TestResolveStagedAllDisjointAllSelected()
{
  // Three workers each stage one disjoint proposal. No conflicts.
  // All three survive the cross-worker MIS.
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 100, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 1000, 0, 1100, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      3, 0, 2000, 0, 2100, 5, 2, true));

  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);
  return resolve.batch.summary.batch_size == 3u
         && resolve.selection.selected_ids.size() == 3u
         && resolve.selection.rejected.empty();
}

bool TestResolveStagedCrossWorkerOverlapEmitsConflict()
{
  // Worker A stages a long wire at (0..1000) layer 2.
  // Worker B stages a wire at (500..1500) layer 2 — overlaps A.
  // Cross-worker MIS picks the higher-aggregate (longer wirelength
  // = wider bbox); the loser is tagged Conflict.
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      /*worker=*/1, 0, 0, 0, 1000, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      /*worker=*/2, 0, 500, 0, 1500, 5, 2, true));
  // Both have wirelength 1000 → tie → lower DeltaId wins.

  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);

  if (resolve.selection.selected_ids.size() != 1u) {
    return false;
  }
  if (!(resolve.selection.selected_ids[0] == r::DeltaId{1, 0, 0})) {
    return false;
  }
  if (resolve.selection.rejected.size() != 1u) {
    return false;
  }
  return resolve.selection.rejected[0].id == r::DeltaId{2, 0, 0}
         && resolve.selection.rejected[0].reason
                == r::RejectionReason::Conflict;
}

bool TestResolveStagedDrainsTheStagingArea()
{
  // After ResolveStagedProposals, the staging area must be empty.
  r::ProposalStaging::Instance().ResetForTest();
  for (std::uint32_t w = 1; w <= 4; ++w) {
    r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
        w, 0, static_cast<int>(w) * 1000, 0,
        static_cast<int>(w) * 1000 + 100, 5, 2, true));
  }
  if (r::ProposalStaging::Instance().size() != 4u) {
    return false;
  }
  r::GreedyPriorityPolicy p;
  (void) r::ResolveStagedProposals(p);
  return r::ProposalStaging::Instance().size() == 0u;
}

bool TestResolveStagedSyntheticBatchSummaryFieldsCorrect()
{
  // 4 staged proposals: 2 commit_eligible disjoint, 1
  // commit_eligible overlapping one of the first two, 1
  // non-eligible. Resolve should populate batch_size=4,
  // legal_count=3, commit_eligible_count=3,
  // unresolved_count = (the non-eligible one if its source is
  // UnresolvedFootprint).
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 100, 5, 2, /*commit_eligible=*/true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 1000, 0, 1100, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      3, 0, 50, 0, 150, 5, 2, true));  // overlaps proposal 1
  // Non-eligible: synthetic proposal whose outcome we hand-craft
  // with commit_eligible=false.
  r::StagedProposal nono = MakeStagedAddWire(
      4, 0, 5000, 0, 5100, 5, 2, /*commit_eligible=*/false);
  nono.outcome.legality.legal = false;
  nono.outcome.legality.source = r::LegalitySource::UnresolvedFootprint;
  r::ProposalStaging::Instance().Stage(nono);

  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);

  if (resolve.batch.summary.batch_size != 4u) {
    return false;
  }
  if (resolve.batch.summary.legal_count != 3u
      || resolve.batch.summary.commit_eligible_count != 3u
      || resolve.batch.summary.unresolved_count != 1u) {
    return false;
  }
  // Cross-worker MIS: workers 1 and 3 conflict (overlap), worker
  // 1 wins (lower DeltaId, equal scores). 2 also selected
  // (disjoint). 4 was non-eligible (UnresolvedFootprint).
  // selected = {1, 2}; rejected = {3:Conflict, 4:Unresolved}.
  if (resolve.selection.selected_ids.size() != 2u) {
    return false;
  }
  if (!(resolve.selection.selected_ids[0] == r::DeltaId{1, 0, 0})
      || !(resolve.selection.selected_ids[1] == r::DeltaId{2, 0, 0})) {
    return false;
  }
  if (resolve.selection.rejected.size() != 2u) {
    return false;
  }
  return resolve.selection.rejected[0].id == r::DeltaId{3, 0, 0}
         && resolve.selection.rejected[0].reason
                == r::RejectionReason::Conflict
         && resolve.selection.rejected[1].id == r::DeltaId{4, 0, 0}
         && resolve.selection.rejected[1].reason
                == r::RejectionReason::UnresolvedFootprint;
}

// ===== V2.4.f — MakeCrossWorkerSummaryRow + integration =====

bool TestCrossWorkerRowSentinelNetIdAndNoSingleWinner()
{
  // Two staged proposals, one wins MIS (lower DeltaId on tie),
  // one rejected as Conflict. Cross-worker row should have
  // net_id=0, has_winner=false, num_conflict=1.
  r::ProposalStaging::Instance().ResetForTest();
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 1000, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 500, 0, 1500, 5, 2, true));
  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);
  auto row = r::MakeCrossWorkerSummaryRow(
      resolve.batch, resolve.selection, /*seqno=*/77);

  return row.seqno == 77u && row.net_id == 0u
         && row.batch_size == 2u && !row.has_winner
         && row.num_commit_eligible == 2u
         && row.num_conflict == 1u && row.num_illegal == 0u
         && row.num_unresolved == 0u && row.num_unsupported == 0u
         && row.num_lower_score == 0u
         && row.EncodeWinnerDeltaId().empty();
}

bool TestCrossWorkerRowAllDisjointZeroConflict()
{
  // Three disjoint staged proposals → all selected, num_conflict=0.
  r::ProposalStaging::Instance().ResetForTest();
  for (std::uint32_t w = 1; w <= 3; ++w) {
    r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
        w, 0, static_cast<int>(w) * 1000, 0,
        static_cast<int>(w) * 1000 + 100, 5, 2, true));
  }
  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);
  auto row = r::MakeCrossWorkerSummaryRow(
      resolve.batch, resolve.selection, /*seqno=*/0);

  return row.batch_size == 3u && row.num_commit_eligible == 3u
         && row.num_conflict == 0u && !row.has_winner;
}

bool TestCrossWorkerRowFeedsBatchSummaryDumpCorrectly()
{
  // End-to-end: cross-worker resolve → MakeCrossWorkerSummaryRow
  // → BatchSummaryDump::Record. Verify the dump's process-wide
  // counters reflect this. (Reset both first.)
  r::BatchSummaryDump::ResetForTest();
  r::ProposalStaging::Instance().ResetForTest();

  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      1, 0, 0, 0, 1000, 5, 2, true));
  r::ProposalStaging::Instance().Stage(MakeStagedAddWire(
      2, 0, 500, 0, 1500, 5, 2, true));

  r::GreedyPriorityPolicy p;
  auto resolve = r::ResolveStagedProposals(p);
  auto row = r::MakeCrossWorkerSummaryRow(
      resolve.batch, resolve.selection, /*seqno=*/0);
  r::BatchSummaryDump::Record(row);

  return r::BatchSummaryDump::batch_count() == 1u
         && r::BatchSummaryDump::batches_with_winner() == 0u
         && r::BatchSummaryDump::total_proposals() == 2u
         && r::BatchSummaryDump::total_rejection_count_for(
                r::RejectionReason::Conflict)
                == 1u;
}

// ===== V2.5.a — EvalOptions::CostWeights seam =====

bool TestCostWeightsDefaultsAreUnity()
{
  r::EvalOptions opts;
  return opts.cost_weights.drc == 1.0
         && opts.cost_weights.marker == 1.0
         && opts.cost_weights.fixed_shape == 1.0
         && opts.cost_weights.marker_decay == 1.0;
}

bool TestCostWeightsDefaultsPreserveV24Aggregate()
{
  // With default CostWeights{1,1,1,1} the aggregate must equal
  // V2.4's wirelength + 100*via_count formula. Today's synthetic
  // AddWire score has delta_history_cost=0 and
  // delta_marker_reduction=0, so the new terms vanish at default
  // weights and the aggregate is byte-identical to V2.4.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  // Default cost_weights.

  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 1000, 5, 2);
  auto out = state.eval(base, pd, opts);
  if (!out.score.has_value()) {
    return false;
  }
  // Wirelength proxy = max(dx, dy) of bbox = max(1000, 5) = 1000.
  // delta_via_count = 0 for AddWire.
  // Aggregate = 1000 + 100*0*1.0 + 1.0*0 - 1.0*0 = 1000.
  return out.score->aggregate == 1000.0;
}

bool TestCostWeightsFixedShapeBiasesViaTerm()
{
  // AddVia gives delta_via_count = +1. With fixed_shape=2.0 the
  // via term doubles → aggregate goes from 100 (default) to 200.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  opts.cost_weights.fixed_shape = 2.0;

  r::AddVia av;
  av.location = r::Point{500, 500};
  av.via_def = nullptr;
  r::ProposedDelta pd;
  pd.delta = av;
  pd.id = r::DeltaId{1, 0, 0};
  pd.write_footprint = r::WriteFootprint::Of(pd.delta);

  auto out = state.eval(base, pd, opts);
  if (!out.score.has_value()) {
    return false;
  }
  // delta_wirelength_proxy = 0 (AddVia adds no wire), delta_via_count
  // = 1, fixed_shape = 2.0 → aggregate = 0 + 100 * 1 * 2.0 = 200.
  return out.score->aggregate == 200.0;
}

bool TestCostWeightsCarriedThroughBatchEval()
{
  // batch_eval forwards the same EvalOptions to each per-proposal
  // eval(). Verify that non-default weights affect every
  // outcome's score.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts_default;
  opts_default.legality_mode = r::LegalityMode::SyntheticOracle;
  r::EvalOptions opts_biased;
  opts_biased.legality_mode = r::LegalityMode::SyntheticOracle;
  opts_biased.cost_weights.fixed_shape = 5.0;

  r::ProposalSet set;
  // Single proposal, AddVia, so the via term shows up.
  r::AddVia av;
  av.location = r::Point{500, 500};
  av.via_def = nullptr;
  r::ProposedDelta pd;
  pd.delta = av;
  pd.id = r::DeltaId{1, 0, 0};
  pd.write_footprint = r::WriteFootprint::Of(pd.delta);
  set.proposals.push_back(pd);

  auto batch_default = state.batch_eval(base, set, opts_default);
  auto batch_biased = state.batch_eval(base, set, opts_biased);

  if (!batch_default.outcomes[0].score.has_value()
      || !batch_biased.outcomes[0].score.has_value()) {
    return false;
  }
  // Default = 100 (1 via × 100 × 1.0). Biased = 500 (1 via × 100 × 5.0).
  return batch_default.outcomes[0].score->aggregate == 100.0
         && batch_biased.outcomes[0].score->aggregate == 500.0;
}

// ===== V2.5.b — CpuDrcOracleRealDeck dispatch =====

namespace lg = drt::redesign::legality;

namespace {

// Helper: construct a MetalShort-only RuleEntry vector backed by
// stable storage owned by the test (config + rules vectors).
struct MetalShortDeckOwning
{
  lg::MetalShortConfig config;
  std::vector<lg::RuleEntry> rules;

  MetalShortDeckOwning()
  {
    lg::RuleEntry e;
    e.type = lg::RuleType::MetalShort;
    e.halo = 0;
    e.predicate = &lg::MetalShortReference;
    e.opaque = &config;
    rules.push_back(e);
  }
};

}  // namespace

bool TestCpuDrcOracleRealDeckNoOracleAttachedFallsToStub()
{
  // No SetCpuDrcOracle call → request CpuDrcOracleRealDeck →
  // current behaviour preserved (legal=true, source=Stub,
  // !commit_eligible).
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;

  auto pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  auto out = state.eval(base, pd, opts);

  return out.legality.legal
         && out.legality.source == r::LegalitySource::StubAssumeLegal
         && !out.legality.commit_eligible;
}

bool TestCpuDrcOracleRealDeckLegalAddWireIsCommitEligible()
{
  // Empty base, MetalShort deck attached → AddWire has no
  // overlapping context → verdict legal, source CpuDrcOracle,
  // commit_eligible TRUE (CpuDrcOracle is trusted).
  ro::MemoryBackedGeometryView base({}, {});
  lg::CpuDrcOracle oracle;
  MetalShortDeckOwning deck;

  r::PhysicalState state;
  state.SetCpuDrcOracle(&oracle, deck.rules);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;

  auto pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  auto out = state.eval(base, pd, opts);

  return out.legality.legal
         && out.legality.source == r::LegalitySource::CpuDrcOracle
         && out.legality.commit_eligible;
}

bool TestCpuDrcOracleRealDeckOverlappingShapeIsIllegal()
{
  // Base contains a shape on layer 2; AddWire overlaps it →
  // MetalShort rule fires → verdict illegal, source CpuDrcOracle,
  // !commit_eligible.
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 100, 5, 2, /*net_id=*/100,
                    /*shape_kind=*/12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  lg::CpuDrcOracle oracle;
  MetalShortDeckOwning deck;

  r::PhysicalState state;
  state.SetCpuDrcOracle(&oracle, deck.rules);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;

  // Candidate at (50,0)-(150,5) overlaps base shape (0,0)-(100,5).
  auto pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 50, 0, 150, 5, 2);
  // Candidate is on a different net → MetalShort fires.
  pd.proposal_net_id = 200;
  auto out = state.eval(base, pd, opts);

  return !out.legality.legal
         && out.legality.source == r::LegalitySource::CpuDrcOracle
         && !out.legality.commit_eligible;
}

bool TestCpuDrcOracleRealDeckSameNetSelfOverlapIsLegal()
{
  // Same-net same-bbox self-overlap is filtered by the adapter
  // (re-route of the same net, not a violation).
  std::vector<ro::ShapeRef> base_shapes{
      MakeFullShape(0, 0, 100, 5, 2, /*net_id=*/200,
                    /*shape_kind=*/12)};
  ro::MemoryBackedGeometryView base({}, base_shapes);
  lg::CpuDrcOracle oracle;
  MetalShortDeckOwning deck;

  r::PhysicalState state;
  state.SetCpuDrcOracle(&oracle, deck.rules);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;

  // Identical-bbox candidate on the SAME net.
  auto pd = MakeProposalAddWireWithFootprint(
      r::DeltaId{1, 0, 0}, 0, 0, 100, 5, 2);
  pd.proposal_net_id = 200;
  auto out = state.eval(base, pd, opts);

  return out.legality.legal
         && out.legality.source == r::LegalitySource::CpuDrcOracle
         && out.legality.commit_eligible;
}

bool TestCpuDrcOracleRealDeckUnknownFootprintIsUnresolved()
{
  // MoveCell yields write_footprint.unknown=true → adapter rejects
  // before invoking the oracle, returns UnresolvedFootprint.
  ro::MemoryBackedGeometryView base({}, {});
  lg::CpuDrcOracle oracle;
  MetalShortDeckOwning deck;

  r::PhysicalState state;
  state.SetCpuDrcOracle(&oracle, deck.rules);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::CpuDrcOracleRealDeck;

  r::MoveCell mv;
  r::ProposedDelta pd;
  pd.delta = mv;
  pd.id = r::DeltaId{1, 0, 0};
  pd.write_footprint = r::WriteFootprint::Of(pd.delta);
  auto out = state.eval(base, pd, opts);

  return !out.legality.legal
         && out.legality.source == r::LegalitySource::UnresolvedFootprint
         && !out.legality.commit_eligible;
}

// ===== V2.5.c — CongestionTimingProvider seam =====

namespace {

// Test stub: returns fixed (congestion, timing) deltas regardless
// of input. Used to verify that the seam plumbs values through
// to Score::aggregate. A real provider would inspect base
// congestion estimates + STA criticality.
class StubCtProvider : public r::CongestionTimingProvider
{
 public:
  StubCtProvider(double c, double t) : c_(c), t_(t) {}
  r::CongestionTimingDelta Query(
      const ro::GeometryView& base,
      const r::ProposedDelta& delta) const override
  {
    (void) base;
    (void) delta;
    return r::CongestionTimingDelta{c_, t_};
  }

 private:
  double c_;
  double t_;
};

}  // namespace

bool TestCtProviderDefaultsAreUnityForBackwardCompat()
{
  // V2.5.a TestCostWeightsDefaultsAreUnity covers drc/marker/
  // fixed_shape/marker_decay; this verifies congestion/timing also
  // default to 1.0 so V2.5.c is backward-compat at the seam level.
  r::EvalOptions opts;
  return opts.cost_weights.congestion == 1.0
         && opts.cost_weights.timing == 1.0;
}

bool TestCtProviderAbsentLeavesScoreAtV24Behavior()
{
  // No provider attached → delta_congestion + delta_timing stay 0
  // → aggregate matches V2.5.a's V2.4-equivalent behavior.
  ro::MemoryBackedGeometryView base({}, {});
  r::PhysicalState state;
  // Note: NOT calling SetCongestionTimingProvider.
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 1000, 5, 2);
  auto out = state.eval(base, pd, opts);
  return out.score.has_value() && out.score->aggregate == 1000.0
         && out.score->delta_congestion == 0.0
         && out.score->delta_timing == 0.0;
}

bool TestCtProviderPopulatesScoreFields()
{
  // Provider returns fixed (congestion=2.5, timing=3.5). Verify
  // the values land in Score::delta_congestion + delta_timing,
  // and aggregate reflects them with default weights (1.0 each).
  ro::MemoryBackedGeometryView base({}, {});
  StubCtProvider provider(2.5, 3.5);
  r::PhysicalState state;
  state.SetCongestionTimingProvider(&provider);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 1000, 5, 2);
  auto out = state.eval(base, pd, opts);
  if (!out.score.has_value()) {
    return false;
  }
  // Aggregate = 1000 (wirelength) + 2.5 (congestion) + 3.5 (timing) = 1006.0
  return out.score->delta_congestion == 2.5
         && out.score->delta_timing == 3.5
         && out.score->aggregate == 1006.0;
}

bool TestCtProviderCostWeightsBiasCongestionAndTiming()
{
  // Same provider, different weights: congestion=10, timing=20 →
  // aggregate = 1000 + 10*2.5 + 20*3.5 = 1000 + 25 + 70 = 1095.
  ro::MemoryBackedGeometryView base({}, {});
  StubCtProvider provider(2.5, 3.5);
  r::PhysicalState state;
  state.SetCongestionTimingProvider(&provider);
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  opts.cost_weights.congestion = 10.0;
  opts.cost_weights.timing = 20.0;
  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 1000, 5, 2);
  auto out = state.eval(base, pd, opts);
  return out.score.has_value() && out.score->aggregate == 1095.0;
}

bool TestCtProviderDetachReturnsToZero()
{
  // Attach then detach → delta_congestion + delta_timing back to 0.
  ro::MemoryBackedGeometryView base({}, {});
  StubCtProvider provider(7.0, 11.0);
  r::PhysicalState state;
  state.SetCongestionTimingProvider(&provider);
  state.SetCongestionTimingProvider(nullptr);  // detach
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;
  auto pd = MakeProposalAddWire(r::DeltaId{1, 0, 0}, 0, 0, 1000, 5, 2);
  auto out = state.eval(base, pd, opts);
  return out.score.has_value()
         && out.score->delta_congestion == 0.0
         && out.score->delta_timing == 0.0
         && out.score->aggregate == 1000.0;
}

// ===== V2.6.a — DriveGate =====

bool TestDriveGateBuildFlagOffAlwaysReturnsFalse()
{
  // The build flag governs all behavior. When OFF, no env var
  // setting can flip ShouldDriveAndCount to true. Below we set
  // env vars that WOULD admit (N=10, ITER_LIMIT=10) but expect
  // false because the binary was built without
  // ENABLE_DRT_REDESIGN_DRIVE.
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "10", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "10", 1);
  r::DriveGate::ResetForTest();

  bool any_admit = false;
  for (int i = 0; i < 5; ++i) {
    if (r::DriveGate::ShouldDriveAndCount(/*iter=*/0)) {
      any_admit = true;
      break;
    }
  }

  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  r::DriveGate::ResetForTest();

  if (r::DriveGate::BuildFlagEnabled()) {
    // Build flag ON: with N=10/ITER_LIMIT=10 the gate admits;
    // any_admit must be true.
    return any_admit;
  }
  // Build flag OFF: gate ALWAYS returns false; counter stays 0
  // regardless of env vars.
  return !any_admit && r::DriveGate::worker_count() == 0;
}

bool TestDriveGateDefaultEnvNoAdmits()
{
  // No env vars set → defaults (N=0, ITER_LIMIT=0). Even iter 0
  // is excluded because N=0 means no drive at all.
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  r::DriveGate::ResetForTest();

  bool any_admit = false;
  for (int i = 0; i < 10; ++i) {
    if (r::DriveGate::ShouldDriveAndCount(/*iter=*/0)) {
      any_admit = true;
      break;
    }
  }
  r::DriveGate::ResetForTest();
  return !any_admit;
}

bool TestDriveGateNFirstCallsAdmit()
{
  if (!r::DriveGate::BuildFlagEnabled()) {
    // Build flag off → cannot exercise admit-path. Skip-as-pass.
    return true;
  }
  {
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "3", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "0", 1);
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  r::DriveGate::ResetForTest();

  // First 3 calls at iter 0 admit; subsequent at iter 0 do not.
  bool admits[6];
  for (int i = 0; i < 6; ++i) {
    admits[i] = r::DriveGate::ShouldDriveAndCount(/*iter=*/0);
  }

  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  r::DriveGate::ResetForTest();

  return admits[0] && admits[1] && admits[2]
         && !admits[3] && !admits[4] && !admits[5];
  }
}

bool TestDriveGateIterLimitGatesIterIndex()
{
  if (!r::DriveGate::BuildFlagEnabled()) {
    return true;
  }
  {
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "100", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "0", 1);
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  r::DriveGate::ResetForTest();

  bool iter0_admit = r::DriveGate::ShouldDriveAndCount(/*iter=*/0);
  bool iter1_reject = !r::DriveGate::ShouldDriveAndCount(/*iter=*/1);
  bool iter5_reject = !r::DriveGate::ShouldDriveAndCount(/*iter=*/5);

  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  r::DriveGate::ResetForTest();
  return iter0_admit && iter1_reject && iter5_reject;
  }
}

bool TestDriveGateKillSwitchOverridesEverything()
{
  if (!r::DriveGate::BuildFlagEnabled()) {
    return true;
  }
  {
  // N=100, ITER_LIMIT=10 (very permissive) — kill switch must
  // still suppress all admits.
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "100", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "10", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE", "1", 1);
  r::DriveGate::ResetForTest();

  bool any_admit = false;
  for (int iter = 0; iter <= 5; ++iter) {
    for (int i = 0; i < 5; ++i) {
      if (r::DriveGate::ShouldDriveAndCount(iter)) {
        any_admit = true;
        break;
      }
    }
  }

  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  r::DriveGate::ResetForTest();
  return !any_admit;
  }
}

bool TestDriveGateWorkerCountReflectsAdmits()
{
  if (!r::DriveGate::BuildFlagEnabled()) {
    return true;
  }
  {
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "5", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "0", 1);
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  r::DriveGate::ResetForTest();
  for (int i = 0; i < 8; ++i) {
    (void) r::DriveGate::ShouldDriveAndCount(/*iter=*/0);
  }
  // Exactly 5 admits → counter == 5.
  std::int64_t got = r::DriveGate::worker_count();
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  r::DriveGate::ResetForTest();
  return got == 5;
  }
}

bool TestDriveGateResetForTestClearsCounter()
{
  if (!r::DriveGate::BuildFlagEnabled()) {
    return true;
  }
  {
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_N", "10", 1);
  ::setenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", "0", 1);
  r::DriveGate::ResetForTest();
  for (int i = 0; i < 3; ++i) {
    (void) r::DriveGate::ShouldDriveAndCount(/*iter=*/0);
  }
  if (r::DriveGate::worker_count() != 3) {
    return false;
  }
  r::DriveGate::ResetForTest();
  bool cleared = r::DriveGate::worker_count() == 0;
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_N");
  ::unsetenv("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT");
  r::DriveGate::ResetForTest();
  return cleared;
  }
}

// ===== V2.2.e.real — ProposeFromCaptured =====

bool TestProposeFromCapturedEmptyReturnsEmpty()
{
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 42;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{100, 100}};
  in.layer = 2;
  in.delta_id = r::DeltaId{42, 0, 0};
  auto out = ro::MazeSearchProposer::ProposeFromCaptured(base, {}, in);
  return out.empty();
}

bool TestProposeFromCapturedSinglePathSegIsAddWire()
{
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 42;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{1000, 1000}};
  in.layer = 2;
  in.delta_id = r::DeltaId{42, 0, 0};

  std::vector<ro::CapturedConnFig> captured;
  ro::CapturedConnFig c;
  c.kind = ro::CapturedConnFig::Kind::PathSeg;
  c.bbox = r::Rect{r::Point{100, 50}, r::Point{900, 55}};
  c.layer = 2;
  captured.push_back(c);

  auto out = ro::MazeSearchProposer::ProposeFromCaptured(base, captured, in);
  if (out.size() != 1u) {
    return false;
  }
  if (out[0].proposal_net_id != 42u) {
    return false;
  }
  if (!(out[0].id == r::DeltaId{42, 0, 0})) {
    return false;
  }
  // Delta variant must be AddWire with the captured bbox + layer.
  const r::AddWire* add = std::get_if<r::AddWire>(&out[0].delta);
  if (add == nullptr) {
    return false;
  }
  return add->bbox.ll.x == 100 && add->bbox.ur.x == 900
         && add->layer == 2 && !out[0].write_footprint.unknown;
}

bool TestProposeFromCapturedViaIsAddVia()
{
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 7;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{1000, 1000}};
  in.layer = 3;
  in.delta_id = r::DeltaId{7, 0, 0};

  std::vector<ro::CapturedConnFig> captured;
  ro::CapturedConnFig v;
  v.kind = ro::CapturedConnFig::Kind::Via;
  v.bbox = r::Rect{r::Point{490, 490}, r::Point{510, 510}};
  v.layer = 3;
  v.via_origin = r::Point{500, 500};
  captured.push_back(v);

  auto out = ro::MazeSearchProposer::ProposeFromCaptured(base, captured, in);
  if (out.size() != 1u) {
    return false;
  }
  const r::AddVia* av = std::get_if<r::AddVia>(&out[0].delta);
  if (av == nullptr) {
    return false;
  }
  return av->location.x == 500 && av->location.y == 500
         && out[0].proposal_net_id == 7u;
}

bool TestProposeFromCapturedPatchWireIsInsertShield()
{
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 99;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{1000, 1000}};
  in.layer = 4;
  in.delta_id = r::DeltaId{99, 0, 0};

  std::vector<ro::CapturedConnFig> captured;
  ro::CapturedConnFig pw;
  pw.kind = ro::CapturedConnFig::Kind::PatchWire;
  pw.bbox = r::Rect{r::Point{200, 200}, r::Point{220, 250}};
  pw.layer = 4;
  captured.push_back(pw);

  auto out = ro::MazeSearchProposer::ProposeFromCaptured(base, captured, in);
  if (out.size() != 1u) {
    return false;
  }
  const r::InsertShield* is = std::get_if<r::InsertShield>(&out[0].delta);
  if (is == nullptr) {
    return false;
  }
  return is->coverage.ll.x == 200 && is->coverage.ur.x == 220
         && is->layer == 4 && out[0].proposal_net_id == 99u;
}

bool TestProposeFromCapturedMultiShapeMonotonicAttemptIds()
{
  // 3 shapes (PathSeg + Via + PatchWire) → 3 Deltas with
  // attempt_index 0, 1, 2 and same region_id/proposer_id.
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 17;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{1000, 1000}};
  in.layer = 2;
  in.delta_id = r::DeltaId{17, 0, 99};  // proposer_id is preserved

  std::vector<ro::CapturedConnFig> captured;

  ro::CapturedConnFig seg;
  seg.kind = ro::CapturedConnFig::Kind::PathSeg;
  seg.bbox = r::Rect{r::Point{0, 0}, r::Point{500, 5}};
  seg.layer = 2;
  captured.push_back(seg);

  ro::CapturedConnFig via;
  via.kind = ro::CapturedConnFig::Kind::Via;
  via.bbox = r::Rect{r::Point{490, 0}, r::Point{510, 5}};
  via.layer = 2;
  via.via_origin = r::Point{500, 2};
  captured.push_back(via);

  ro::CapturedConnFig pw;
  pw.kind = ro::CapturedConnFig::Kind::PatchWire;
  pw.bbox = r::Rect{r::Point{500, 0}, r::Point{1000, 5}};
  pw.layer = 3;
  captured.push_back(pw);

  auto out = ro::MazeSearchProposer::ProposeFromCaptured(base, captured, in);
  if (out.size() != 3u) {
    return false;
  }
  // attempt_index walks 0..2; region_id/proposer_id stable.
  for (std::size_t i = 0; i < 3; ++i) {
    if (out[i].id.region_id != 17u) return false;
    if (out[i].id.proposer_id != 0u) return false;
    if (out[i].id.attempt_index != static_cast<std::uint32_t>(i))
      return false;
    if (out[i].proposal_net_id != 17u) return false;
  }
  // Delta variants in order.
  return std::holds_alternative<r::AddWire>(out[0].delta)
         && std::holds_alternative<r::AddVia>(out[1].delta)
         && std::holds_alternative<r::InsertShield>(out[2].delta);
}

bool TestProposeFromCapturedDeltasAreEvaluable()
{
  // End-to-end: captured shapes → ProposedDeltas → batch_eval
  // under SyntheticOracle. Each Delta should yield a sound score
  // (not throw, returns optional<Score>).
  ro::MemoryBackedGeometryView base({}, {});
  ro::MazeSearchProposer::Input in;
  in.net_id = 1;
  in.route_box = r::Rect{r::Point{0, 0}, r::Point{1000, 100}};
  in.layer = 2;
  in.delta_id = r::DeltaId{1, 0, 0};

  std::vector<ro::CapturedConnFig> captured;
  ro::CapturedConnFig seg;
  seg.kind = ro::CapturedConnFig::Kind::PathSeg;
  seg.bbox = r::Rect{r::Point{100, 10}, r::Point{500, 15}};
  seg.layer = 2;
  captured.push_back(seg);
  ro::CapturedConnFig via;
  via.kind = ro::CapturedConnFig::Kind::Via;
  via.bbox = r::Rect{r::Point{490, 10}, r::Point{510, 15}};
  via.layer = 2;
  via.via_origin = r::Point{500, 12};
  captured.push_back(via);

  auto deltas = ro::MazeSearchProposer::ProposeFromCaptured(
      base, captured, in);

  r::PhysicalState state;
  r::EvalOptions opts;
  opts.legality_mode = r::LegalityMode::SyntheticOracle;

  r::ProposalSet set;
  set.proposals = deltas;
  auto batch = state.batch_eval(base, set, opts);
  if (batch.outcomes.size() != 2u) {
    return false;
  }
  // Both must have a score (legal under SyntheticOracle on empty
  // base for AddWire; AddVia is also legal — synthetic oracle
  // checks overlap on the same layer).
  return batch.outcomes[0].score.has_value()
         && batch.outcomes[1].score.has_value();
}

// ===== V2.6.d.a — RoutePerturbation =====

namespace {

// Build a synthetic 2-segment route for perturbation testing.
std::vector<ro::CapturedConnFig> SyntheticTwoSegRoute()
{
  std::vector<ro::CapturedConnFig> out;
  ro::CapturedConnFig seg;
  seg.kind = ro::CapturedConnFig::Kind::PathSeg;
  seg.bbox = r::Rect{r::Point{100, 200}, r::Point{500, 205}};
  seg.layer = 2;
  out.push_back(seg);

  ro::CapturedConnFig via;
  via.kind = ro::CapturedConnFig::Kind::Via;
  via.bbox = r::Rect{r::Point{490, 200}, r::Point{510, 205}};
  via.layer = 2;
  via.via_origin = r::Point{500, 202};
  out.push_back(via);
  return out;
}

}  // namespace

bool TestPerturbationIdentityIsBitwiseCopy()
{
  auto orig = SyntheticTwoSegRoute();
  ro::PerturbationParams id{};  // (0, 0)
  auto cp = ro::ApplyPerturbation(orig, id);
  if (cp.size() != orig.size()) {
    return false;
  }
  for (std::size_t i = 0; i < orig.size(); ++i) {
    if (cp[i].kind != orig[i].kind
        || cp[i].layer != orig[i].layer
        || cp[i].bbox.ll.x != orig[i].bbox.ll.x
        || cp[i].bbox.ur.x != orig[i].bbox.ur.x
        || cp[i].bbox.ll.y != orig[i].bbox.ll.y
        || cp[i].bbox.ur.y != orig[i].bbox.ur.y
        || cp[i].via_origin.x != orig[i].via_origin.x
        || cp[i].via_origin.y != orig[i].via_origin.y) {
      return false;
    }
  }
  return true;
}

bool TestPerturbationShiftAppliesToAllShapes()
{
  auto orig = SyntheticTwoSegRoute();
  ro::PerturbationParams p;
  p.shift_x_dbu = 50;
  p.shift_y_dbu = -30;
  auto out = ro::ApplyPerturbation(orig, p);

  // Both shapes should shift by (+50, -30). Layers/kinds preserved.
  return out[0].bbox.ll.x == 150 && out[0].bbox.ll.y == 170
         && out[0].bbox.ur.x == 550 && out[0].bbox.ur.y == 175
         && out[0].layer == 2
         && out[1].bbox.ll.x == 540 && out[1].bbox.ur.x == 560
         && out[1].via_origin.x == 550
         && out[1].via_origin.y == 172;
}

bool TestGenerateKPerturbationsKZeroIsEmpty()
{
  auto orig = SyntheticTwoSegRoute();
  auto out = ro::GenerateKPerturbations(orig, /*k=*/0);
  return out.empty();
}

bool TestGenerateKPerturbationsVariantZeroIsIdentity()
{
  auto orig = SyntheticTwoSegRoute();
  auto out = ro::GenerateKPerturbations(orig, /*k=*/4);
  if (out.size() != 4u) {
    return false;
  }
  // Variant 0 must be identity (bit-for-bit).
  if (out[0].size() != orig.size()) {
    return false;
  }
  for (std::size_t i = 0; i < orig.size(); ++i) {
    if (out[0][i].bbox.ll.x != orig[i].bbox.ll.x
        || out[0][i].bbox.ur.x != orig[i].bbox.ur.x) {
      return false;
    }
  }
  return true;
}

bool TestGenerateKPerturbationsAllVariantsDistinct()
{
  // K=4 produces 4 variants (identity + 3 shifted). All four
  // routes' first PathSeg should have distinct bbox.ll.x values
  // because the schedule rotates through ±x and ±y shifts.
  auto orig = SyntheticTwoSegRoute();
  auto out = ro::GenerateKPerturbations(orig, /*k=*/4,
                                        /*track_pitch=*/100);
  if (out.size() != 4u) {
    return false;
  }
  // Variant 0: identity (ll.x = 100)
  // Variant 1: +x → ll.x = 200
  // Variant 2: -x → ll.x = 0
  // Variant 3: +y → ll.x stays = 100, ll.y = 300
  if (out[0][0].bbox.ll.x != 100) return false;
  if (out[1][0].bbox.ll.x != 200) return false;
  if (out[2][0].bbox.ll.x != 0) return false;
  if (out[3][0].bbox.ll.x != 100) return false;
  if (out[3][0].bbox.ll.y != 300) return false;
  return true;
}

bool TestGenerateKPerturbationsTrackPitchHintScales()
{
  auto orig = SyntheticTwoSegRoute();
  auto out_default = ro::GenerateKPerturbations(orig, /*k=*/2,
                                                /*track_pitch=*/100);
  auto out_doubled = ro::GenerateKPerturbations(orig, /*k=*/2,
                                                /*track_pitch=*/200);
  // Variant 1 = +x shift; with pitch=100 it's +100, with 200 it's +200.
  return out_default[1][0].bbox.ll.x == 200
         && out_doubled[1][0].bbox.ll.x == 300;
}

bool TestGenerateKPerturbationsK1IsIdentityOnly()
{
  auto orig = SyntheticTwoSegRoute();
  auto out = ro::GenerateKPerturbations(orig, /*k=*/1);
  return out.size() == 1u
         && out[0][0].bbox.ll.x == orig[0].bbox.ll.x;
}

bool TestPerturbationWithEmptyRoute()
{
  std::vector<ro::CapturedConnFig> empty;
  ro::PerturbationParams p;
  p.shift_x_dbu = 50;
  auto out = ro::ApplyPerturbation(empty, p);
  if (!out.empty()) {
    return false;
  }
  auto kvars = ro::GenerateKPerturbations(empty, /*k=*/3);
  return kvars.size() == 3u && kvars[0].empty()
         && kvars[1].empty() && kvars[2].empty();
}

// V2.4.a — Net and ReadWrite enum slots are reserved for V2.4.b.
// Lock in the encoding now so consumers (the BatchSummaryDump
// num_conflict column, the future GreedyPriority MIS solver) don't
// need renumbering when V2.4.b lights them up.
static_assert(static_cast<int>(r::ConflictKind::Net)
                  != static_cast<int>(r::ConflictKind::Geometry),
              "Net must be a distinct ConflictKind value");
static_assert(static_cast<int>(r::ConflictKind::ReadWrite)
                  != static_cast<int>(r::ConflictKind::Geometry),
              "ReadWrite must be a distinct ConflictKind value");
static_assert(static_cast<int>(r::ConflictKind::ReadWrite)
                  != static_cast<int>(r::ConflictKind::Net),
              "ReadWrite must be a distinct ConflictKind value");

// V2.3.b — Conflict reason is reserved (V2.4 will emit it). Verify
// the enum value exists and is distinct from every other reason so
// downstream call sites (shadow dump column, log readers) can lock
// in the encoding now and not have to renumber when V2.4 lights up
// the conflict path.
static_assert(static_cast<int>(r::RejectionReason::Conflict)
                  != static_cast<int>(r::RejectionReason::Illegal),
              "Conflict must be a distinct enum value");
static_assert(static_cast<int>(r::RejectionReason::Conflict)
                  != static_cast<int>(r::RejectionReason::LowerScore),
              "Conflict must be a distinct enum value");
static_assert(static_cast<int>(r::RejectionReason::Conflict)
                  != static_cast<int>(r::RejectionReason::Unknown),
              "Conflict must be a distinct enum value");

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
      {"V2.3.a batch_eval empty set",
       TestBatchEvalEmptySet},
      {"V2.3.a batch_eval single proposal matches single eval",
       TestBatchEvalSingleProposalMatchesSingleEval},
      {"V2.3.a batch_eval mixed proposals (legal/illegal/unresolved/unsupported)",
       TestBatchEvalMixedProposalsCountsCorrectly},
      {"V2.3.a batch_eval sorts outputs by DeltaId regardless of input order",
       TestBatchEvalSortsByDeltaIdRegardlessOfInputOrder},
      {"V2.3.a batch_eval total_eval_time_ns >= 0",
       TestBatchEvalTotalTimeNonNegative},
      {"V2.3.b SelectBest empty batch -> no winner",
       TestSelectBestEmptyBatchHasNoWinner},
      {"V2.3.b SelectBest single commit_eligible proposal wins",
       TestSelectBestSingleCommitEligibleWins},
      {"V2.3.b SelectBest no commit_eligible -> no winner, all Unknown",
       TestSelectBestNoCommitEligibleNoWinner},
      {"V2.3.b SelectBest picks highest aggregate score",
       TestSelectBestPicksHighestAggregateScore},
      {"V2.3.b SelectBest tie-breaks by lowest DeltaId",
       TestSelectBestTieBreaksByLowestDeltaId},
      {"V2.3.b SelectBest classifies Illegal/Unresolved/Unsupported/LowerScore",
       TestSelectBestClassifiesRejectionReasonsAcrossKinds},
      {"V2.3.b SelectBest deterministic across input permutations",
       TestSelectBestDeterministicAcrossInputPermutations},
      {"V2.3.b.dump BatchSummaryRow from mixed batch is correct",
       TestBatchSummaryRowFromMixedBatch},
      {"V2.3.b.dump BatchSummaryRow on empty batch has no winner",
       TestBatchSummaryRowEmptyBatchHasNoWinner},
      {"V2.3.b.dump EncodeWinnerDeltaId formats region:proposer:attempt",
       TestBatchSummaryRowEncodeWinnerDeltaId},
      {"V2.3.b.dump counters update without env var",
       TestBatchSummaryDumpCountersUpdateWithoutEnv},
      {"V2.3.b.dump enabled writes header + row",
       TestBatchSummaryDumpEnabledWritesHeaderAndRow},
      {"V2.3.b.dump bad path is best-effort, never throws",
       TestBatchSummaryDumpBadPathDoesNotThrow},
      {"V2.4.a ConflictGraph empty set -> empty graph",
       TestConflictGraphEmptySetIsEmpty},
      {"V2.4.a ConflictGraph single proposal -> 0 edges",
       TestConflictGraphSingleProposalHasNoEdges},
      {"V2.4.a ConflictGraph disjoint proposals -> 0 edges",
       TestConflictGraphDisjointProposalsHaveNoEdges},
      {"V2.4.a ConflictGraph overlapping same-layer -> 1 Geometry edge",
       TestConflictGraphOverlappingSameLayerHasOneEdge},
      {"V2.4.a ConflictGraph overlapping different-layer -> 0 edges",
       TestConflictGraphOverlappingDifferentLayerHasNoEdge},
      {"V2.4.a ConflictGraph touching edges count as overlap",
       TestConflictGraphTouchingEdgesCount},
      {"V2.4.a ConflictGraph unknown footprint conflicts with everything",
       TestConflictGraphUnknownFootprintConflictsWithEverything},
      {"V2.4.a ConflictGraph K=4 mutual overlap -> 6 edges (complete)",
       TestConflictGraphCompleteOverlapKEqualsFour},
      {"V2.4.a ConflictGraph deterministic across rebuilds",
       TestConflictGraphDeterministicAcrossRebuilds},
      {"V2.4.b ConflictGraph same-net different-layer -> Net edge",
       TestConflictGraphSameNetDifferentBboxNetEdge},
      {"V2.4.b ConflictGraph different nets -> no Net edge",
       TestConflictGraphDifferentNetsNoNetEdge},
      {"V2.4.b ConflictGraph zero net_id never matches",
       TestConflictGraphZeroNetIdNeverMatches},
      {"V2.4.b ConflictGraph A reads B's write same layer -> ReadWrite edge",
       TestConflictGraphReadOverlapsWriteEdge},
      {"V2.4.b ConflictGraph read different layer -> no edge",
       TestConflictGraphReadDifferentLayerNoEdge},
      {"V2.4.b ConflictGraph precedence Geometry > Net",
       TestConflictGraphPrecedenceGeometryWinsOverNet},
      {"V2.4.b ConflictGraph mixed three-kind batch",
       TestConflictGraphMixedThreeKindsInOneBatch},
      {"V2.4.b MazeSearchProposer propagates net_id to proposal_net_id",
       TestProposerSetsProposalNetId},
      {"V2.4.c GreedyPriority empty input -> empty selection",
       TestGreedyEmptyInputReturnsEmpty},
      {"V2.4.c GreedyPriority single proposal no edges -> selected",
       TestGreedySingleProposalNoEdgesIsSelected},
      {"V2.4.c GreedyPriority two independent -> both selected",
       TestGreedyTwoIndependentBothSelected},
      {"V2.4.c GreedyPriority one edge higher score wins",
       TestGreedyOneEdgeHigherScoreWins},
      {"V2.4.c GreedyPriority equal score lower DeltaId wins",
       TestGreedyEqualScoreLowerDeltaIdWins},
      {"V2.4.c GreedyPriority chain picks endpoints",
       TestGreedyChainPicksEnds},
      {"V2.4.c GreedyPriority high-score middle blocks both ends",
       TestGreedyHighScoreMiddleAlwaysAdmitted},
      {"V2.4.c GreedyPriority deterministic across edge permutations",
       TestGreedyDeterministicAcrossEdgePermutations},
      {"V2.4.c MakeEdgeList round-trips ConflictGraph",
       TestGreedyMakeEdgeListFromConflictGraph},
      {"V2.4.c End-to-end ConflictGraph + GreedyPriority on K=4 complete",
       TestGreedyEndToEndConflictGraphPlusSolver},
      {"V2.4.d SelectMis empty batch -> empty result",
       TestSelectMisEmptyBatchEmptyResult},
      {"V2.4.d SelectMis all eligible no conflicts -> all selected",
       TestSelectMisAllEligibleNoConflictsAllSelected},
      {"V2.4.d SelectMis K=4 complete overlap -> only highest aggregate",
       TestSelectMisCompleteOverlapKEqualsFourOnlyOneSelected},
      {"V2.4.d SelectMis mixed Conflict/Illegal/UnsupportedDelta",
       TestSelectMisIllegalAndConflictMixedReasons},
      {"V2.4.d SelectMis deterministic across policy instances",
       TestSelectMisDeterministicAcrossPolicyInstances},
      {"V2.4.e ProposalStaging drain empty",
       TestStagingDrainEmpty},
      {"V2.4.e ProposalStaging push then drain returns N",
       TestStagingPushDrainSize},
      {"V2.4.e ProposalStaging Drain DeltaId-sorts the output",
       TestStagingDrainSortsByDeltaId},
      {"V2.4.e Resolve cross-worker disjoint -> all selected",
       TestResolveStagedAllDisjointAllSelected},
      {"V2.4.e Resolve cross-worker overlap -> Conflict reason emitted",
       TestResolveStagedCrossWorkerOverlapEmitsConflict},
      {"V2.4.e Resolve drains the staging area",
       TestResolveStagedDrainsTheStagingArea},
      {"V2.4.e Resolve synthetic batch summary fields are correct",
       TestResolveStagedSyntheticBatchSummaryFieldsCorrect},
      {"V2.4.f cross-worker row uses sentinel net_id=0, no single winner",
       TestCrossWorkerRowSentinelNetIdAndNoSingleWinner},
      {"V2.4.f cross-worker row all-disjoint -> num_conflict=0",
       TestCrossWorkerRowAllDisjointZeroConflict},
      {"V2.4.f cross-worker row feeds BatchSummaryDump correctly",
       TestCrossWorkerRowFeedsBatchSummaryDumpCorrectly},
      {"V2.5.a CostWeights defaults are 1.0 each",
       TestCostWeightsDefaultsAreUnity},
      {"V2.5.a CostWeights defaults preserve V2.4 aggregate formula",
       TestCostWeightsDefaultsPreserveV24Aggregate},
      {"V2.5.a CostWeights.fixed_shape biases via term",
       TestCostWeightsFixedShapeBiasesViaTerm},
      {"V2.5.a CostWeights carried through batch_eval",
       TestCostWeightsCarriedThroughBatchEval},
      {"V2.5.b RealDeck no oracle attached -> stub fallthrough",
       TestCpuDrcOracleRealDeckNoOracleAttachedFallsToStub},
      {"V2.5.b RealDeck legal AddWire -> CpuDrcOracle source, commit_eligible",
       TestCpuDrcOracleRealDeckLegalAddWireIsCommitEligible},
      {"V2.5.b RealDeck overlapping shape (diff net) -> illegal",
       TestCpuDrcOracleRealDeckOverlappingShapeIsIllegal},
      {"V2.5.b RealDeck same-net self-overlap -> legal (filtered)",
       TestCpuDrcOracleRealDeckSameNetSelfOverlapIsLegal},
      {"V2.5.b RealDeck unknown footprint -> UnresolvedFootprint",
       TestCpuDrcOracleRealDeckUnknownFootprintIsUnresolved},
      {"V2.5.c CostWeights congestion + timing default to 1.0",
       TestCtProviderDefaultsAreUnityForBackwardCompat},
      {"V2.5.c No CT provider -> score matches V2.4 behavior",
       TestCtProviderAbsentLeavesScoreAtV24Behavior},
      {"V2.5.c CT provider populates Score::delta_{congestion,timing}",
       TestCtProviderPopulatesScoreFields},
      {"V2.5.c CostWeights bias congestion + timing terms",
       TestCtProviderCostWeightsBiasCongestionAndTiming},
      {"V2.5.c Detach CT provider returns deltas to zero",
       TestCtProviderDetachReturnsToZero},
      {"V2.6.a DriveGate: build flag OFF -> always false",
       TestDriveGateBuildFlagOffAlwaysReturnsFalse},
      {"V2.6.a DriveGate: default env -> no admits",
       TestDriveGateDefaultEnvNoAdmits},
      {"V2.6.a DriveGate: N first calls admit, rest reject",
       TestDriveGateNFirstCallsAdmit},
      {"V2.6.a DriveGate: ITER_LIMIT gates iter index",
       TestDriveGateIterLimitGatesIterIndex},
      {"V2.6.a DriveGate: kill switch overrides everything",
       TestDriveGateKillSwitchOverridesEverything},
      {"V2.6.a DriveGate: worker_count() reflects admits",
       TestDriveGateWorkerCountReflectsAdmits},
      {"V2.6.a DriveGate: ResetForTest clears counter",
       TestDriveGateResetForTestClearsCounter},
      {"V2.2.e.real ProposeFromCaptured empty -> empty",
       TestProposeFromCapturedEmptyReturnsEmpty},
      {"V2.2.e.real PathSeg -> AddWire",
       TestProposeFromCapturedSinglePathSegIsAddWire},
      {"V2.2.e.real Via -> AddVia",
       TestProposeFromCapturedViaIsAddVia},
      {"V2.2.e.real PatchWire -> InsertShield",
       TestProposeFromCapturedPatchWireIsInsertShield},
      {"V2.2.e.real multi-shape -> monotonic attempt_index",
       TestProposeFromCapturedMultiShapeMonotonicAttemptIds},
      {"V2.2.e.real captured Deltas evaluate cleanly through batch_eval",
       TestProposeFromCapturedDeltasAreEvaluable},
      {"V2.6.d.a Perturbation identity is bitwise copy",
       TestPerturbationIdentityIsBitwiseCopy},
      {"V2.6.d.a Perturbation shift applies to all shapes (incl via_origin)",
       TestPerturbationShiftAppliesToAllShapes},
      {"V2.6.d.a GenerateKPerturbations K=0 -> empty",
       TestGenerateKPerturbationsKZeroIsEmpty},
      {"V2.6.d.a GenerateKPerturbations variant 0 is identity",
       TestGenerateKPerturbationsVariantZeroIsIdentity},
      {"V2.6.d.a GenerateKPerturbations all 4 variants distinct",
       TestGenerateKPerturbationsAllVariantsDistinct},
      {"V2.6.d.a GenerateKPerturbations track_pitch_hint scales",
       TestGenerateKPerturbationsTrackPitchHintScales},
      {"V2.6.d.a GenerateKPerturbations K=1 -> identity only",
       TestGenerateKPerturbationsK1IsIdentityOnly},
      {"V2.6.d.a Perturbation handles empty input route",
       TestPerturbationWithEmptyRoute},
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
