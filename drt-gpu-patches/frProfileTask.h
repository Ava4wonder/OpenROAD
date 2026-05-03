// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors
//
// Modified for DRT profile-CSV instrumentation (topic: design-gpu-kernels-for-
// the-drt-phase-of-openroad...). Adds a third build variant controlled by the
// PROFILE_CSV compile definition; unchanged behaviour when PROFILE_CSV and
// HAS_VTUNE are both undefined.

#pragma once

#ifdef HAS_VTUNE
#include <ittnotify.h>
#endif

#ifdef PROFILE_CSV
#include <string>
#endif

namespace drt {

#ifdef HAS_VTUNE

class ProfileTask
{
 public:
  ProfileTask(const char* name) : done_(false)
  {
    domain_ = __itt_domain_create("TritonRoute");
    name_ = __itt_string_handle_create(name);
    __itt_task_begin(domain_, __itt_null, __itt_null, name_);
  }

  ~ProfileTask()
  {
    if (!done_) {
      __itt_task_end(domain_);
    }
  }

  void done()
  {
    done_ = true;
    __itt_task_end(domain_);
  }

 private:
  __itt_domain* domain_;
  __itt_string_handle* name_;
  bool done_;
};

#elif defined(PROFILE_CSV)

// CSV-backed RAII profiler. Implementation in frProfileTaskCsv.cpp.
// Output path taken from env var DRT_PROFILE_CSV (default: drt_profile.csv).
//
// name is COPIED into std::string at construction so callers may pass
// dynamic strings (e.g., FlexDR.cpp:1327 batch_name.c_str()) safely.
class ProfileTask
{
 public:
  ProfileTask(const char* name);
  ~ProfileTask();
  void done();

 private:
  std::string name_;
  long long start_ns_;
  bool done_;
};

#else

// No-op version
class ProfileTask
{
 public:
  ProfileTask(const char* name) {}
  void done() {}
};

#endif

}  // namespace drt
