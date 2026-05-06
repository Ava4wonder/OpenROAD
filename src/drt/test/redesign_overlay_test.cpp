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
#include "redesign/overlay/Hashing.h"
#include "redesign/overlay/MemoryBackedGeometryView.h"
#include "redesign/overlay/ShadowDump.h"
#include "redesign/overlay/SnapshotHandle.h"

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

bool TestQueryPinAccessThrowsInV21()
{
  ro::MemoryBackedGeometryView view({});
  try {
    (void) view.QueryPinAccess(r::Rect{});
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
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

  // Header should mention the column names; row should contain the
  // bbox coordinates and hashes we passed in.
  const bool header_ok
      = header.find("seqno") != std::string::npos
        && header.find("legacy_hash") != std::string::npos
        && header.find("overlay_hash") != std::string::npos;
  const bool row_ok = row.find(",1,2,3,4,") != std::string::npos
                      && row.find(",17,") != std::string::npos
                      && row.find(",34,") != std::string::npos;
  return header_ok && row_ok;
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

// ===== Compile-time absence of CanonicalTuple for V2.2+ entities =====
//
// V2.1.e shipped MarkerRef CanonicalTuple. V2.2.a.1 added ShapeRef.
// GuideRef / BlockageRef / PinAccessRef CanonicalTuples land in
// V2.2.a.{2,3,4} respectively.
static_assert(ro::has_canonical_tuple<ro::MarkerRef>::value,
              "MarkerRef must have CanonicalTuple");
static_assert(ro::has_canonical_tuple<ro::ShapeRef>::value,
              "ShapeRef must have CanonicalTuple after V2.2.a.1");
static_assert(!ro::has_canonical_tuple<ro::GuideRef>::value,
              "GuideRef CanonicalTuple lands in V2.2.a.2");
static_assert(!ro::has_canonical_tuple<ro::BlockageRef>::value,
              "BlockageRef CanonicalTuple lands in V2.2.a.3");
static_assert(!ro::has_canonical_tuple<ro::PinAccessRef>::value,
              "PinAccessRef CanonicalTuple lands in V2.2.a.4");

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
      {"GeometryView::QueryPinAccess throws in V2.1",
       TestQueryPinAccessThrowsInV21},
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
