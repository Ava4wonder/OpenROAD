// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.d — typed read-view interface over physical-state geometry.
//
// V2.1 only: this interface and its stub implementations are
// read-through views over live FlexDR/FlexGridGraph state. They
// provide API compatibility, not multi-version snapshot isolation.
// V2.4 replaces the V2.1 stub with a persistent CoW-backed
// implementation.
//
// V2.1 supports QueryMarkers only; the V2.1.e read-site at
// FlexDR_init.cpp:3227 is the only consumer this phase. The
// QueryRouteShapes / QueryGuides / QueryBlockages methods are
// declared so V2.2+ proposers can fill them in without changing the
// abstract interface; V2.1 implementations throw std::logic_error
// from these methods, and tests assert that throw is in place.
//
// Identity rule: MarkerRef and ShapeRef are VALUE types. They never
// hold raw pointers (frMarker*, frConstraint*, etc.). All identity
// fields are by-value with std::optional for nullable metadata so
// canonical hashing (ShapeSetHash, V2.1.e) can distinguish "absent"
// from "present with value 0".

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "../Delta.h"      // Rect, Point, LayerNum
#include "../Footprint.h"  // NetId

namespace drt::redesign::overlay {

using drt::redesign::LayerNum;
using drt::redesign::NetId;
using drt::redesign::Rect;

// Value-type marker identity. No pointers. Optional fields are absent
// when the source data does not carry them; canonical serialization
// MUST distinguish absent from value 0.
struct MarkerRef
{
  Rect bbox{};
  std::optional<LayerNum> layer;
  std::optional<uint32_t> constraint_type_id;  // frConstraintTypeEnum
  std::optional<uint64_t> constraint_id;       // stable per-process id
  std::optional<NetId> source_net_id;
};

// Value-type shape identity. No pointers. Same nullable-field rule as
// MarkerRef.
struct ShapeRef
{
  Rect bbox{};
  std::optional<LayerNum> layer;
  std::optional<NetId> net_id;
};

// V2.2.a.2 — guides are 3D corridors spanning a layer range. The
// canonical identity captures both endpoints; either may be absent
// for synthetic data, but real frGuide projection always populates
// both. Compressing to a single layer would conflate distinct
// multi-layer guides.
struct GuideRef
{
  Rect bbox{};
  std::optional<LayerNum> begin_layer;
  std::optional<LayerNum> end_layer;
  std::optional<NetId> net_id;
};

// V2.2.a.3 — blockages come in two flavours: frBlockage (PDK / block-
// level) and frInstBlockage (instance-derived). Both carry a bbox
// from the regionQuery index and a layer from the query parameter.
// `source_inst_id` is present iff the blockage was an frInstBlockage,
// which is what distinguishes the two flavours canonically.
struct BlockageRef
{
  Rect bbox{};
  std::optional<LayerNum> layer;
  std::optional<std::uint64_t> source_inst_id;
};

struct PinAccessRef
{
  Rect bbox{};
  std::optional<LayerNum> layer;
  std::optional<uint64_t> iterm_id;
  std::optional<int32_t> access_pattern_index;
};

// QueryCost / CostFieldView intentionally NOT on the V2.1.e abstract
// interface. Adding `unique_ptr<CostFieldView>` as a return type
// requires CostFieldView to be complete at every call site of the
// virtual destructor (libstdc++ instantiates sizeof(T) inside
// unique_ptr's deleter). Forward-declaration alone is not enough,
// and a fake empty struct would create false confidence ahead of
// the V2.2.a.5 cost-field design. Per v2 §2.5, the QueryCost slot
// will be added to GeometryView in V2.2.a.5 alongside the real
// CostFieldView definition. Until then, no V2.1 path needs it.

// Lightweight aliases. Documents intent ("this is a query result")
// without committing to a QueryResult<T> wrapper. V2.2 may revisit
// when there are multiple real query families with status semantics.
using MarkerQueryResult = std::vector<MarkerRef>;
using ShapeQueryResult = std::vector<ShapeRef>;
using GuideQueryResult = std::vector<GuideRef>;
using BlockageQueryResult = std::vector<BlockageRef>;
using PinAccessQueryResult = std::vector<PinAccessRef>;

class GeometryView
{
 public:
  virtual ~GeometryView() = default;

  // V2.1-supported. Returns by value: each call allocates a fresh
  // vector. No clear-or-append ambiguity because the caller is given
  // a freshly-constructed result. If the V2.4 hot path needs an
  // out-parameter overload to avoid allocations, it will be
  // introduced as a sibling method then.
  virtual MarkerQueryResult QueryMarkers(const Rect& box) const = 0;

  // V2.2+ supported. V2.1 implementations throw
  // std::logic_error("GeometryView: <method> not supported in V2.1").
  // Callers MUST NOT touch these methods in V2.1 — exercised by
  // negative tests in redesign_overlay_test.cpp.
  virtual ShapeQueryResult QueryRouteShapes(const Rect& box,
                                            LayerNum layer) const = 0;
  virtual GuideQueryResult QueryGuides(const Rect& box) const = 0;
  virtual BlockageQueryResult QueryBlockages(const Rect& box,
                                             LayerNum layer) const = 0;
  virtual PinAccessQueryResult QueryPinAccess(const Rect& box) const = 0;
};

}  // namespace drt::redesign::overlay
