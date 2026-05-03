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

struct DeleteWire {
  SegmentId segment_id = 0;
};

struct AddVia {
  Point location;
  drt::frViaDef* via_def = nullptr;
  drt::frNet* net = nullptr;
};

struct DeleteVia {
  ViaId via_id = 0;
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

struct ProposedDelta {
  Delta delta;
  DeltaSource source = DeltaSource::UnknownTest;
  int worker_id = -1;
  uint64_t snapshot_version = 0;
};

}  // namespace drt::redesign
