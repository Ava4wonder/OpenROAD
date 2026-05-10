// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.6.a — DriveGate implementation.

#include "DriveGate.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

namespace drt::redesign {

namespace {

struct GateState
{
  std::mutex mu;
  // Re-read env vars once per process (after the first call OR
  // after ResetForTest). Cached for cheap repeated lookup.
  bool env_loaded = false;
  std::int64_t max_n = 0;
  std::int64_t iter_limit = 0;
  bool kill_switched = false;

  // Per-process worker drive counter. Strictly monotonic; only
  // ResetForTest clears it.
  std::int64_t driven_count = 0;
};

GateState& State()
{
  static GateState s;
  return s;
}

// Parse an integer-valued env var; absent / unparsable → fallback.
std::int64_t GetEnvInt(const char* name, std::int64_t fallback) noexcept
{
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return fallback;
  }
  try {
    return std::stoll(std::string(v));
  } catch (...) {
    return fallback;
  }
}

bool GetEnvKill(const char* name) noexcept
{
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

void EnsureEnvLoaded(GateState& s)
{
  if (s.env_loaded) {
    return;
  }
  s.max_n = GetEnvInt("OPENROAD_DRT_REDESIGN_DRIVE_N", 0);
  s.iter_limit
      = GetEnvInt("OPENROAD_DRT_REDESIGN_DRIVE_ITER_LIMIT", 0);
  s.kill_switched
      = GetEnvKill("OPENROAD_DRT_REDESIGN_DRIVE_DISABLE");
  s.env_loaded = true;
}

}  // namespace

bool DriveGate::ShouldDriveAndCount(int iter)
{
  // Layer (1): build flag. When the macro is undefined, this
  // function is still callable but always returns false. Callers
  // do not need #ifdef wrapping.
#ifndef ENABLE_DRT_REDESIGN_DRIVE
  (void) iter;
  return false;
#else
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  EnsureEnvLoaded(s);

  // Layer (2): kill switch.
  if (s.kill_switched) {
    return false;
  }

  // Layer (3): subset filter.
  if (iter > s.iter_limit) {
    return false;
  }
  if (s.driven_count >= s.max_n) {
    return false;
  }

  // Admit. Increment INSIDE the lock so concurrent OMP callers
  // see a consistent counter. The (++count, return true) pair is
  // atomic with respect to other ShouldDriveAndCount calls.
  s.driven_count += 1;
  return true;
#endif
}

void DriveGate::ResetForTest()
{
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  s.env_loaded = false;
  s.max_n = 0;
  s.iter_limit = 0;
  s.kill_switched = false;
  s.driven_count = 0;
}

std::int64_t DriveGate::worker_count() noexcept
{
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  return s.driven_count;
}

bool DriveGate::BuildFlagEnabled() noexcept
{
#ifdef ENABLE_DRT_REDESIGN_DRIVE
  return true;
#else
  return false;
#endif
}

}  // namespace drt::redesign
