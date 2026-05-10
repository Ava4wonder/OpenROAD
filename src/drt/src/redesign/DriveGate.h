// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.a — process-wide gate that decides whether the V2 path
// should DRIVE production routing (mutate FlexDR state) for a
// given (worker_call, iter) pair, or stay in shadow mode.
//
// Three layers of gating, in order of strictness:
//
//   (1) Build flag — when ENABLE_DRT_REDESIGN_DRIVE is OFF (the
//       default), DriveGate::ShouldDriveAndCount() ALWAYS returns
//       false regardless of env vars. Production builds without
//       this flag PHYSICALLY CANNOT enter drive mode.
//
//   (2) Kill-switch env var — OPENROAD_DRT_REDESIGN_DRIVE_DISABLE,
//       when set to non-empty non-"0", forces ShouldDriveAndCount
//       to return false even with the build flag on. Belt-and-
//       suspenders for emergency revert without rebuilding.
//
//   (3) Subset filter — when neither (1) nor (2) blocks, drive
//       fires when both are true:
//         * the per-process worker counter is < N
//           (env var OPENROAD_DRT_REDESIGN_DRIVE_N, default 0
//            which means no drive)
//         * `iter` <= ITER_LIMIT
//           (env var OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT,
//            default 0 which means iter 0 only)
//
// V2.6.a SCOPE — gate logic only. The CALLER (V2.6.b) is
// responsible for actually mutating production routing when
// ShouldDriveAndCount returns true. V2.6.a's tests verify the
// gate decisions; V2.6.b/V2.6.gate verify the mutation effect.
//
// Thread safety: the worker counter is mutex-serialised; OMP
// callers from inside processWorkersBatch can call concurrently
// and at most N total invocations across the process will return
// true.

#pragma once

#include <cstdint>

namespace drt::redesign {

class DriveGate
{
 public:
  // Combined check + atomic-increment of the per-process worker
  // counter. Returns true iff all three layers admit the call.
  // Even when the build flag is OFF, this function still exists
  // and returns false — callers don't need to wrap in #ifdef.
  static bool ShouldDriveAndCount(int iter);

  // Test-only — clears the worker counter. Env vars are re-read
  // on the next ShouldDriveAndCount call.
  static void ResetForTest();

  // Process-wide accumulated drive count (test / introspection).
  static std::int64_t worker_count() noexcept;

  // Returns true iff the binary was compiled with
  // ENABLE_DRT_REDESIGN_DRIVE defined. Lets tests / consumers
  // branch on the build-time fact at run time without their own
  // #ifdef (the macro is set on drt_lib's compile units, not
  // necessarily on every consumer).
  static bool BuildFlagEnabled() noexcept;
};

}  // namespace drt::redesign
