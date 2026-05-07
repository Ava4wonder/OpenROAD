// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.2.c.bridge — DeltaToOracleInput adapter implementation.

#include "OracleCandidate.h"

#include <type_traits>
#include <variant>

namespace drt::redesign::overlay {

namespace {

// V2.2.b stub default — mirrors WriteFootprint::Of(AddVia) and
// OverlayGeometryView's via halo. Replaced by real via_def lookup
// when V2.2.c.legality.realpdk wires the production oracle.
constexpr int32_t kDefaultViaEnclosureDbu = 100;

bool BboxIsZeroArea(const Rect& r) noexcept
{
  return r.ll.x == r.ur.x && r.ll.y == r.ur.y;
}

}  // namespace

OracleCandidateBatch DeltaToOracleInput(const GeometryView& view,
                                        const ProposedDelta& delta)
{
  // V2.2.c.bridge does not query `view`; the parameter is reserved
  // for V2.2.c.legality.* which may need contextual queries. Touch
  // it once to silence unused-parameter warnings without renaming
  // the API.
  (void) view;

  OracleCandidateBatch batch;

  std::visit(
      [&](const auto& kind) {
        using T = std::decay_t<decltype(kind)>;

        if constexpr (std::is_same_v<T, AddWire>) {
          OracleCandidateShape c;
          c.kind = OracleCandidateShape::Kind::Wire;
          c.bbox = kind.bbox;
          c.layer = kind.layer;
          // V2.2.b NET-ID LIMITATION: AddWire carries frNet* but the
          // bridge cannot dereference it (include discipline). Net
          // resolution happens at the proposer layer in a later
          // sub-commit. Synthetic-oracle tests can construct
          // candidates with explicit net_id directly via
          // OracleCandidateShape — bypassing this bridge path.
          batch.added.push_back(c);
        } else if constexpr (std::is_same_v<T, AddVia>) {
          // Single via cut. Bbox is location ± default enclosure
          // halo (placeholder). Layer is a stub 0 — V2.2.c.legality.
          // realpdk resolves the cut layer from kind.via_def.
          OracleCandidateShape c;
          c.kind = OracleCandidateShape::Kind::ViaCut;
          c.bbox.ll.x = kind.location.x - kDefaultViaEnclosureDbu;
          c.bbox.ll.y = kind.location.y - kDefaultViaEnclosureDbu;
          c.bbox.ur.x = kind.location.x + kDefaultViaEnclosureDbu;
          c.bbox.ur.y = kind.location.y + kDefaultViaEnclosureDbu;
          c.layer = 0;
          batch.added.push_back(c);
        } else if constexpr (std::is_same_v<T, InsertShield>) {
          OracleCandidateShape c;
          c.kind = OracleCandidateShape::Kind::Shield;
          c.bbox = kind.coverage;
          c.layer = kind.layer;
          batch.added.push_back(c);
        } else if constexpr (std::is_same_v<T, DeleteWire>) {
          // Resolved → record in deleted_context (oracle uses it to
          // build base ∖ deleted). Unresolved → loud
          // UnresolvedFootprint status.
          if (!kind.resolved_net_id.has_value()
              || !kind.shape_kind.has_value()
              || BboxIsZeroArea(kind.bbox)) {
            batch.status
                = OracleCandidateBatch::Status::UnresolvedFootprint;
            return;
          }
          OracleCandidateShape c;
          // Map the V2.2.b.del shape_kind (frBlockObjectEnum cast)
          // into Wire vs PatchWire. frcPathSeg=12, frcPatchWire=22
          // per src/drt/src/frBaseTypes.h — values used as raw bytes
          // in V2.1.e+ ProjectRouteShape. Synthetic-oracle tests can
          // bypass this mapping by populating OracleCandidateShape
          // directly.
          if (kind.shape_kind.value() == 22u) {
            c.kind = OracleCandidateShape::Kind::PatchWire;
          } else {
            c.kind = OracleCandidateShape::Kind::Wire;
          }
          c.bbox = kind.bbox;
          c.layer = kind.layer;
          c.net_id = static_cast<NetId>(kind.resolved_net_id.value());
          batch.deleted_context.push_back(c);
        } else if constexpr (std::is_same_v<T, DeleteVia>) {
          if (!kind.resolved_net_id.has_value()
              || BboxIsZeroArea(kind.bbox)) {
            batch.status
                = OracleCandidateBatch::Status::UnresolvedFootprint;
            return;
          }
          OracleCandidateShape c;
          c.kind = OracleCandidateShape::Kind::ViaCut;
          c.bbox = kind.bbox;
          c.layer = kind.cut_layer;
          c.net_id = static_cast<NetId>(kind.resolved_net_id.value());
          batch.deleted_context.push_back(c);
        } else {
          // MoveCell / ChangePinAccess / ChangeLayerAssignment /
          // ResizeCell: V2.2.c.bridge does not adapt these. Eval
          // marks proposals containing them as non-committable.
          batch.status = OracleCandidateBatch::Status::UnsupportedDelta;
        }
      },
      delta.delta);

  return batch;
}

}  // namespace drt::redesign::overlay
