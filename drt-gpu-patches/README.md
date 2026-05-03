# DRT profile-CSV instrumentation patches

Minimal instrumentation for Track A (H1/H2 hypotheses — per-worker time distribution and in-worker phase breakdown).

## Design

- Extend `drt::ProfileTask` RAII timer (already present as no-op in `frProfileTask.h`) with a third build variant: **PROFILE_CSV**, enabled by `-DPROFILE_CSV=ON` CMake option in `src/drt/`.
- Thread-local ring of records, lock-free after first push. Shared-ptr-based buffer registration keeps per-thread data alive past thread exit.
- `std::atexit` dumps `thread_id,task_name,start_ns,duration_ns` to path in env var `DRT_PROFILE_CSV` (default `drt_profile.csv`).
- New `ProfileTask` sites instrumented for the profile question:
  - `FlexDRWorker::main` — already has `ProfileTask("DRW:main")` upstream.
  - `FlexGridGraph::search` — new `ProfileTask("DRW:maze_search")` (the serial A\*).
  - `FlexGCWorker::Impl::main` — new `ProfileTask("DRW:gc_main")` wrapping the dispatcher, plus per-check-type sub-tasks around each `checkMetal*()` / `checkCutSpacing()`.

## Files

- `frProfileTask.h.patch` — adds PROFILE_CSV branch that declares out-of-line ProfileTask class.
- `frProfileTaskCsv.cpp` — new source file with per-thread buffer + atexit CSV dump. Built only when PROFILE_CSV is set.
- `FlexGridGraph_maze.cpp.patch` — adds ProfileTask at top of `search()` body.
- `FlexGC_main.cpp.patch` — adds top-level ProfileTask in `FlexGCWorker::Impl::main()` plus per-check sub-tasks.
- `CMakeLists.txt.patch` — adds `PROFILE_CSV` option + conditional source + define.
- `apply.sh` — applies everything to `~/openroad-profile/OpenROAD/` on the H100 host.

## Usage

```bash
ssh -i ~/.ssh/guest_gpu_access_rsa azureuser@20.110.81.12
cd ~/openroad-profile/OpenROAD
./artifacts/patches/apply.sh          # apply; idempotent
./etc/Build.sh -no-gui -no-tests -threads=32 -cmake='-DPROFILE_CSV=ON'
DRT_PROFILE_CSV=/tmp/drt_profile.csv ./build/bin/openroad ...
```

CSV output has one row per ProfileTask scope exit.
