// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.b — ClipDumpHook implementation.

#include "ClipDumpHook.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace drt::redesign::legality {

namespace {

std::string EnvOrEmpty(const char* name)
{
  const char* v = std::getenv(name);
  return v == nullptr ? std::string{} : std::string{v};
}

}  // namespace

ClipDumpHook& ClipDumpHook::Instance()
{
  // Function-local static: thread-safe init in C++11+.
  static ClipDumpHook instance;
  return instance;
}

ClipDumpHook::ClipDumpHook()
{
  const std::string path = EnvOrEmpty("DRT_DUMP_GC_CLIPS");
  if (path.empty()) {
    return;  // remain inactive
  }
  // Open exclusively for binary write, truncating any prior file. Failure
  // to open keeps the hook inactive — never blocks routing.
  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_.is_open()) {
    std::fprintf(
        stderr,
        "[drt::redesign] WARNING DRT_DUMP_GC_CLIPS=%s could not be opened "
        "for write; clip dumping disabled.\n",
        path.c_str());
    return;
  }
  try {
    WriteHeader(file_);
    file_.flush();
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "[drt::redesign] WARNING failed to write clip-dump header: "
                 "%s; clip dumping disabled.\n",
                 e.what());
    file_.close();
    return;
  }
  design_hint_ = EnvOrEmpty("DRT_DUMP_DESIGN");
  pdk_hint_ = EnvOrEmpty("DRT_DUMP_PDK");
  active_ = true;
}

ClipDumpHook::~ClipDumpHook()
{
  if (file_.is_open()) {
    file_.close();
  }
}

void ClipDumpHook::Dump(const ClipRecord& record)
{
  if (!active_) {
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(mu_);
    WriteRecord(file_, record);
  } catch (const std::exception& e) {
    if (!warned_once_.exchange(true)) {
      std::fprintf(stderr,
                   "[drt::redesign] WARNING clip-dump write failed: %s. "
                   "Subsequent failures will be silenced.\n",
                   e.what());
    }
    // Per guardrail 5, do not propagate. Routing continues.
  } catch (...) {
    if (!warned_once_.exchange(true)) {
      std::fprintf(stderr,
                   "[drt::redesign] WARNING clip-dump write failed with "
                   "non-std::exception. Subsequent failures silenced.\n");
    }
  }
}

}  // namespace drt::redesign::legality
