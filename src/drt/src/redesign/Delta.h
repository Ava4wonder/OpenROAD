// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Phase 1 scaffolding for the DRT redesign.
// See topics/.../drt_redesign_plan.md §1 (sharedDB) and §7 (two-phase commit).
// A Delta is a single proposed mutation to PhysicalState. Workers emit Deltas
// against a frozen Snapshot; the commit engine selects a non-conflicting
// subset and applies them. No worker writes to the DB directly.

#pragma once

#include <cstdint>
#include <optional>
#include <variant>

namespace drt {
class frNet;
class frInst;
class frInstTerm;
class frMaster;
class frViaDef;
}  // namespace drt

namespace drt::redesign {

struct Point {
  int32_t x = 0;
  int32_t y = 0;
};

struct Rect {
  Point ll;
  Point ur;
};

using LayerNum = int16_t;
using SegmentId = uint64_t;
using ViaId = uint64_t;

enum class Orient : uint8_t {
  R0,
  R90,
  R180,
  R270,
  MX,
  MY,
  MX_R90,
  MY_R90
};

struct AddWire {
  Rect bbox;
  LayerNum layer = 0;
  drt::frNet* net = nullptr;
  uint32_t width = 0;
};

// V2.2.b.del — DeleteWire carries explicit identity for exact-match
// removal (NOT geometric area subtraction). Production commit
// REQUIRES bbox + layer + resolved_net_id + shape_kind to be
// populated; OverlayGeometryView refuses to delete on partial
// identity (silent no-op + WriteFootprint::unknown=true). Synthetic
// tests may construct partial identity for negative-path coverage.
struct DeleteWire {
  SegmentId segment_id = 0;
  Rect bbox{};
  LayerNum layer = 0;
  std::optional<uint64_t> resolved_net_id;
  // frBlockObjectEnum cast to u8: frcPathSeg or frcPatchWire.
  // DeleteVia (below) handles frcVia — the kinds are split because
  // their identity sources differ.
  std::optional<uint8_t> shape_kind;
};

struct AddVia {
  Point location;
  drt::frViaDef* via_def = nullptr;
  drt::frNet* net = nullptr;
};

// V2.2.b.del — DeleteVia carries explicit identity. cut_layer is the
// frViaDef::getCutLayerNum() value. shape_kind is intrinsically
// frcVia and is therefore not a separate field.
struct DeleteVia {
  ViaId via_id = 0;
  Rect bbox{};
  LayerNum cut_layer = 0;
  std::optional<uint64_t> resolved_net_id;
};

struct MoveCell {
  drt::frInst* inst = nullptr;
  Point new_origin;
  Orient new_orient = Orient::R0;
};

struct ChangePinAccess {
  drt::frInstTerm* iterm = nullptr;
  int new_access_pattern_index = -1;
};

struct ChangeLayerAssignment {
  SegmentId segment_id = 0;
  LayerNum new_layer = 0;
};

struct InsertShield {
  Rect coverage;
  LayerNum layer = 0;
  drt::frNet* shield_net = nullptr;
};

struct ResizeCell {
  drt::frInst* inst = nullptr;
  drt::frMaster* new_master = nullptr;
};

using Delta = std::variant<AddWire,
                           DeleteWire,
                           AddVia,
                           DeleteVia,
                           MoveCell,
                           ChangePinAccess,
                           ChangeLayerAssignment,
                           InsertShield,
                           ResizeCell>;

enum class DeltaSource : uint8_t {
  PinAccess,
  GlobalRoutePatch,
  DetailedRoutePatch,
  PlacementPerturbation,
  BufferRepair,
  UnknownTest,
};

// ProposedDelta lives in Footprint.h so it can carry DeltaId,
// ReadFootprint, and WriteFootprint by value without a circular
// include. Forward-declared here for consumers that only see the type
// through pointers/references.
struct ProposedDelta;

}  // namespace drt::redesign
