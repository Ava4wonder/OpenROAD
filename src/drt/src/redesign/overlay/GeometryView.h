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

class GeometryView
{
 public:
  virtual ~GeometryView() = default;

  // V2.1-supported. Returns by value: each call allocates a fresh
  // vector. No clear-or-append ambiguity because the caller is given
  // a freshly-constructed result. If the V2.4 hot path needs an
  // out-parameter overload to avoid allocations, it will be
  // introduced as a sibling method then.
  virtual std::vector<MarkerRef> QueryMarkers(const Rect& box) const = 0;

  // V2.2+ supported. V2.1 implementations throw
  // std::logic_error("GeometryView: <method> not supported in V2.1").
  // Callers MUST NOT touch these methods in V2.1 — exercised by
  // negative tests in redesign_overlay_test.cpp.
  virtual std::vector<ShapeRef> QueryRouteShapes(const Rect& box,
                                                 LayerNum layer) const = 0;
  virtual std::vector<ShapeRef> QueryGuides(const Rect& box) const = 0;
  virtual std::vector<ShapeRef> QueryBlockages(const Rect& box,
                                               LayerNum layer) const = 0;
};

}  // namespace drt::redesign::overlay
