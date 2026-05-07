// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.2 — canonical hashing framework.
//
// **Scope (load-bearing):** HashCanonicalRange is for deterministic
// regression / debug equivalence only. It is NOT a proof of OCC
// validity, NOT a proof of conflict freedom, and NOT a substitute
// for the read/write footprint intersection check (Footprint.h §3
// SAFETY INVARIANT). A non-cryptographic 64-bit checksum cannot
// stand in for set-level reasoning. Future readers tempted to use
// the hash as a cheap "read footprints are equal" proof: do not.
//
// Discipline (per v2 §2.5.1): each entity type — MarkerRef, ShapeRef,
// GuideRef, BlockageRef, PinAccessRef, future ViaRef and CostFieldRef
// — declares its OWN CanonicalTuple overload. The hash framework is
// generic; the canonicalisation is per-entity. A "generic reflective
// hash" that walked struct fields blindly would conflate distinct
// entities (e.g., BlockageRef[layer,bbox,source_inst_id] vs
// ShapeRef[layer,bbox,net_id] differ semantically), and a runtime
// throw on a missing canonicalisation could fail in the middle of a
// long integration run. So:
//
//   - HashCanonicalRange<T> is a template that requires
//     CanonicalTuple(const T&) to exist at instantiation time. If no
//     overload exists for T, the template fails to compile (clean
//     static_assert message). Run-time throw is NOT used here.
//
//   - V2.1.e implements ONLY CanonicalTuple(const MarkerRef&). The
//     other entity types (ShapeRef, GuideRef, BlockageRef,
//     PinAccessRef) have NO CanonicalTuple overload. Attempting
//     HashCanonicalRange on them is a compile error. V2.2.a's
//     per-entity sub-commits add their CanonicalTuple alongside the
//     real query implementation.
//
//   - Pointer-identity prohibition: every CanonicalTuple overload
//     MUST consist of value-comparable fields only. No raw pointers,
//     no v-table addresses, no allocation-derived ids. The framework
//     does not enforce this directly (C++ can't trivially check it),
//     but the per-entity overloads are reviewed for compliance and
//     each carries a comment asserting it.
//
//   - Optional fields: std::optional<T> is hashed with explicit
//     "absent" vs "present-with-value" distinction. The canonical
//     serialiser writes a 1-byte tag + value-bytes for present, just
//     a 0-byte tag for absent.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "GeometryView.h"

namespace drt::redesign::overlay {

// CanonicalTuple — declared per entity. V2.1.e implements MarkerRef
// only.
//
// MarkerRef canonical identity:
//   (layer? , bbox.ll.x, bbox.ll.y, bbox.ur.x, bbox.ur.y,
//    constraint_type_id?, constraint_id?, source_net_id?)
//
// All fields value-comparable. Pointer-identity prohibition: this
// overload contains zero pointer fields.
inline auto CanonicalTuple(const MarkerRef& m)
{
  return std::make_tuple(m.layer,
                         m.bbox.ll.x,
                         m.bbox.ll.y,
                         m.bbox.ur.x,
                         m.bbox.ur.y,
                         m.constraint_type_id,
                         m.constraint_id,
                         m.source_net_id);
}

// V2.2.a.1 — ShapeRef canonical identity:
//   (layer?, bbox.ll.x, bbox.ll.y, bbox.ur.x, bbox.ur.y, net_id?)
//
// Used for route-shape (frPathSeg / frVia / frPatchWire) projection
// from RegionQueryGeometryView::QueryRouteShapes. Pointer-free.
//
// Note on type collision: two distinct route-shape kinds at the same
// (bbox, layer, net) would canonicalize identically. In practice this
// is rare because path-seg and via bboxes have very different shapes
// (long rectangle vs small square). If V2.2.a's integration shows
// collisions, ShapeRef gains an optional shape_kind field then.
inline auto CanonicalTuple(const ShapeRef& s)
{
  return std::make_tuple(s.layer,
                         s.bbox.ll.x,
                         s.bbox.ll.y,
                         s.bbox.ur.x,
                         s.bbox.ur.y,
                         s.net_id);
}

// V2.2.a.2 — GuideRef canonical identity:
//   (begin_layer?, end_layer?, bbox.{ll,ur}.{x,y}, net_id?)
//
// Pointer-free. Both layer endpoints participate so multi-layer
// guides with shared bbox+net are not conflated.
inline auto CanonicalTuple(const GuideRef& g)
{
  return std::make_tuple(g.begin_layer,
                         g.end_layer,
                         g.bbox.ll.x,
                         g.bbox.ll.y,
                         g.bbox.ur.x,
                         g.bbox.ur.y,
                         g.net_id);
}

// V2.2.a.3 — BlockageRef canonical identity:
//   (layer?, bbox.{ll,ur}.{x,y}, source_inst_id?)
//
// source_inst_id distinguishes frInstBlockage from frBlockage at the
// hash level — two blockages at the same (bbox, layer) but different
// origin canonicalize differently, which is the right semantics.
inline auto CanonicalTuple(const BlockageRef& b)
{
  return std::make_tuple(b.layer,
                         b.bbox.ll.x,
                         b.bbox.ll.y,
                         b.bbox.ur.x,
                         b.bbox.ur.y,
                         b.source_inst_id);
}

// V2.2.a.4 — PinAccessRef canonical identity. Pin access is an
// API/projection checkpoint in V2.2.a.4 — no live RegionQuery
// backend yet, no FlexDR shadow site. The hash is exercised by
// MemoryBackedGeometryView synthetic tests. All identity fields are
// optional so subsets populated by future real-backend impls
// canonicalize cleanly.
inline auto CanonicalTuple(const PinAccessRef& p)
{
  return std::make_tuple(p.layer,
                         p.bbox.ll.x,
                         p.bbox.ll.y,
                         p.bbox.ur.x,
                         p.bbox.ur.y,
                         p.iterm_id,
                         p.access_point_id,
                         p.net_id,
                         p.access_pattern_index,
                         p.cost,
                         p.type_low,
                         p.type_high,
                         p.has_planar_access,
                         p.has_up_access,
                         p.has_down_access);
}

// SFINAE detector — has_canonical_tuple<T>::value is true iff
// CanonicalTuple(const T&) exists in scope.
template <typename T, typename = void>
struct has_canonical_tuple : std::false_type
{
};

template <typename T>
struct has_canonical_tuple<
    T,
    std::void_t<decltype(CanonicalTuple(std::declval<const T&>()))>>
    : std::true_type
{
};

namespace detail {

// FNV-1a 64-bit. Deterministic across compilers/platforms — the byte
// stream we hash is canonical (see WriteCanonical*) so the digest is
// reproducible. SHA-1 is overkill for V2.1.e; we may swap in a
// stronger hash if collision risk grows.
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline void HashByte(uint64_t& state, uint8_t byte) noexcept
{
  state ^= byte;
  state *= kFnvPrime;
}

inline void HashBytes(uint64_t& state,
                      const void* data,
                      std::size_t n) noexcept
{
  const auto* p = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    HashByte(state, p[i]);
  }
}

// Canonical serialisers. Each writes a stable, platform-independent
// byte representation. Integer types are little-endian'd by hand. To
// hash an enum entity field, add an explicit overload that casts to
// the underlying type — V2.1.e CanonicalTuple overloads contain no
// enums, so the integer-only base case suffices.
template <typename T>
inline void WriteCanonical(uint64_t& state, T v) noexcept
{
  static_assert(std::is_integral_v<T>,
                "WriteCanonical: integer types only. Add an explicit "
                "overload for enums or other types.");
  if constexpr (std::is_same_v<T, bool>) {
    // make_unsigned<bool> is ill-formed; serialise directly.
    HashByte(state, v ? 1u : 0u);
  } else {
    using U = std::make_unsigned_t<T>;
    U u = static_cast<U>(v);
    // Little-endian byte stream.
    uint8_t buf[sizeof(U)];
    for (std::size_t i = 0; i < sizeof(U); ++i) {
      buf[i] = static_cast<uint8_t>(u & 0xFFu);
      u >>= 8;
    }
    HashBytes(state, buf, sizeof(U));
  }
}

// std::optional<T> with explicit absent/present distinction.
// 0 -> absent. 1 -> present (followed by value bytes).
template <typename T>
inline void WriteCanonical(uint64_t& state,
                           const std::optional<T>& v) noexcept
{
  if (v.has_value()) {
    HashByte(state, 1u);
    WriteCanonical(state, v.value());
  } else {
    HashByte(state, 0u);
  }
}

template <typename Tuple, std::size_t I = 0>
inline void WriteCanonicalTuple(uint64_t& state, const Tuple& t) noexcept
{
  if constexpr (I < std::tuple_size_v<Tuple>) {
    WriteCanonical(state, std::get<I>(t));
    WriteCanonicalTuple<Tuple, I + 1>(state, t);
  }
}

}  // namespace detail

// HashCanonicalRange — compile-error if no CanonicalTuple(const T&)
// overload is in scope for the range's value type.
//
// Algorithm:
//   1. Build per-item canonical tuple via CanonicalTuple(item).
//   2. Sort tuples lexicographically (stable, deterministic order).
//   3. Hash the byte serialisation of each tuple in sorted order.
//
// Returns a 64-bit FNV-1a digest. Two runs over the same multiset
// produce identical digests regardless of insertion order.
template <typename Range>
uint64_t HashCanonicalRange(const Range& items)
{
  using value_type = typename Range::value_type;
  static_assert(
      has_canonical_tuple<value_type>::value,
      "HashCanonicalRange<T> requires CanonicalTuple(const T&) to be "
      "in scope. Define an overload before calling. V2.1.e implements "
      "MarkerRef only; other entities land in V2.2.a per-entity sub-"
      "commits.");

  using tuple_type
      = decltype(CanonicalTuple(std::declval<const value_type&>()));
  std::vector<tuple_type> tuples;
  tuples.reserve(items.size());
  for (const auto& it : items) {
    tuples.push_back(CanonicalTuple(it));
  }
  std::sort(tuples.begin(), tuples.end());

  uint64_t state = detail::kFnvOffset;
  for (const auto& t : tuples) {
    detail::WriteCanonicalTuple(state, t);
    // Per-item separator so [(1,2),(3,4)] and [(1,2,3),(4)] aren't
    // confused.
    detail::HashByte(state, 0xFEu);
  }
  return state;
}

}  // namespace drt::redesign::overlay
