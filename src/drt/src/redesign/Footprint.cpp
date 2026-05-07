// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.c — WriteFootprint::Of for the Delta variants whose footprints
// can be derived without a design-side lookup. Variants whose footprint
// needs the backing store (DeleteWire / DeleteVia / ChangeLayerAssignment
// / MoveCell / ChangePinAccess / ResizeCell) return an empty footprint
// in V2.1; V2.2.a fills them in once Snapshot can resolve segment/via/
// inst handles.

#include "Footprint.h"

#include <type_traits>
#include <variant>

namespace drt::redesign {

namespace {

// Default via-enclosure halo in DBU. V2.1 stub — V2.2.a will look up
// the actual enclosure from the via_def. The halo only matters for
// conflict-graph edge construction (V2.4); a too-generous halo means
// more spurious conflicts but never missed conflicts.
constexpr int32_t kDefaultViaEnclosureDbu = 100;

}  // namespace

WriteFootprint WriteFootprint::Of(const Delta& d)
{
  WriteFootprint out;
  std::visit(
      [&](const auto& kind) {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, AddWire>) {
          out.shapes.push_back(kind.bbox);
          out.layers.push_back(kind.layer);
        } else if constexpr (std::is_same_v<T, AddVia>) {
          // Bbox: the via location expanded by a default enclosure halo.
          // Layer: V2.1 records placeholder 0 — V2.2.a resolves the
          // via_def's cut and metal layers. The location-derived bbox
          // and placeholder layer are sound (over-conservative on
          // layer); `unknown` stays false.
          Rect r;
          r.ll.x = kind.location.x - kDefaultViaEnclosureDbu;
          r.ll.y = kind.location.y - kDefaultViaEnclosureDbu;
          r.ur.x = kind.location.x + kDefaultViaEnclosureDbu;
          r.ur.y = kind.location.y + kDefaultViaEnclosureDbu;
          out.shapes.push_back(r);
          out.layers.push_back(0);
        } else if constexpr (std::is_same_v<T, InsertShield>) {
          out.shapes.push_back(kind.coverage);
          out.layers.push_back(kind.layer);
        } else if constexpr (std::is_same_v<T, DeleteWire>) {
          // V2.2.b.del: DeleteWire now carries explicit
          // bbox + layer + resolved_net_id + shape_kind. Footprint
          // is sound iff resolved_net_id AND shape_kind are
          // populated AND bbox is non-empty. Otherwise the delete
          // identity is incomplete and the footprint stays unknown
          // (parallel commit must reject it per the safety
          // invariant).
          const bool resolved
              = kind.resolved_net_id.has_value()
                && kind.shape_kind.has_value()
                && (kind.bbox.ll.x != kind.bbox.ur.x
                    || kind.bbox.ll.y != kind.bbox.ur.y);
          if (resolved) {
            out.shapes.push_back(kind.bbox);
            out.layers.push_back(kind.layer);
          } else {
            out.unknown = true;
          }
        } else if constexpr (std::is_same_v<T, DeleteVia>) {
          // V2.2.b.del: DeleteVia carries explicit
          // bbox + cut_layer + resolved_net_id (shape_kind is
          // intrinsically frcVia and not a separate field).
          const bool resolved
              = kind.resolved_net_id.has_value()
                && (kind.bbox.ll.x != kind.bbox.ur.x
                    || kind.bbox.ll.y != kind.bbox.ur.y);
          if (resolved) {
            out.shapes.push_back(kind.bbox);
            out.layers.push_back(kind.cut_layer);
          } else {
            out.unknown = true;
          }
        } else {
          // MoveCell, ChangePinAccess, ChangeLayerAssignment,
          // ResizeCell: V2.1 cannot produce a sound footprint
          // without a design-side lookup. Set the unknown flag so
          // V2.4's commit path rejects these from the parallel
          // batch — see Footprint.h SAFETY INVARIANT.
          out.unknown = true;
        }
      },
      d);
  return out;
}

}  // namespace drt::redesign
