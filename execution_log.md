# grid_state_access — Execution Log

Append-only. Newest entries on top.

## 2026-05-16 — Branch creation and scaffold

What I did:
- Created branch `grid_state_access` from `master @ 9a00dc952f`.
- Wrote plan.md (verbatim from user-supplied 9-section description),
  execution_log.md, current_state.md.
- Single commit, docs only, no source changes.

What I observed:
- H100 disk at 711 MB free (100% used). Tight but enough for docs.
- User's drt-redesign V2.2 `L2c1_8t` ispd18_test9 run is in flight in
  `drt-build-env` (PID 653254). I will NOT touch the container.

Decision / next step:
- Proceed with Phase 1-3 code edits on the master worktree (file edits
  only, no rebuild). Code lives in OpenROAD/src/drt/; the redesign run
  uses `/work/redesign/src/drt/` from a separate worktree, so my edits
  do not affect their binary.
- Defer Phase 4-5 (search refactor + perf tuning) until after we can build
  and ack a clean compile.

Files / commits / artifacts:
- 7ede745fb5 grid_state_access: scaffold plan/execution_log/current_state

## 2026-05-16 — P4 SoA + bucketed-frontier search behind flag

What I did:
- Added WavefrontBucketedFrontier.{h,cpp}: parallel to P3 BucketedFrontier
  but holds FlexWavefrontGrid payloads. Vector-of-buckets + overflow heap,
  bucket_width=1 default.
- FlexGridGraph.h: 3 new includes; 4 method decls (searchSoA, pushFrontier,
  syncSoADims, isSoABackendEnabled); 4 new members (node_idx_, state_soa_,
  soa_frontier_, soa_backend_enabled_cache_).
- FlexGridGraph.cpp: augmented resetStatus() to bump SoA epoch + clear
  soa_frontier_ when flag enabled (K-bias invariant honored). Added
  syncSoADims, pushFrontier, isSoABackendEnabled definitions.
- FlexGridGraph_maze.cpp: search() prologue dispatches to searchSoA() when
  DRT_USE_SOA_BACKEND=1. searchSoA() mirrors the master search() body
  using soa_frontier_ for the open list and state_soa_ Closed flag for
  the closed-set check. Within-bucket A* tie-breaking preserved by
  sort(batch, FlexWavefrontGrid::operator<). 3 wavefront_.push call sites
  replaced with pushFrontier dispatcher.
- src/drt/CMakeLists.txt: registered WavefrontBucketedFrontier.cpp.

What I observed:
- Pure-additive on the not-enabled path: with the env var unset every
  call site behaves identically to master.
- Did not build. User redesign experiment still occupied drt-build-env
  earlier this session; staying out to avoid CPU contention.
- The K-bias invariant from MEMORY.md (project_routenet_kbias_invariant)
  is honored: epoch bump lives in resetStatus(), which is the single
  canonical call site recursive routeNet must already hit.

Decision / next step:
- Need build verification next. Until then, do NOT enable the flag in
  any production run; SoA path is unvalidated. Test plan when build
  is available:
  1. Compile-clean check.
  2. Smoke run nangate45/swerv with flag OFF; verify identical
     wallclock/markers vs master baseline (~267 s, 37,496 iter-0).
  3. Smoke run with flag ON; expect identical DRC count and similar
     wirelength (validates GS-H3).
  4. If smoke passes: measure end-to-end speedup vs master (GS-H4).

Files / commits / artifacts:
- a0c82935fa grid_state_access P4 (6 files, +374 lines):
  - src/drt/src/dr/WavefrontBucketedFrontier.h  ( 66 lines)
  - src/drt/src/dr/WavefrontBucketedFrontier.cpp( 94 lines)
  - src/drt/src/dr/FlexGridGraph.h              ( +28 lines)
  - src/drt/src/dr/FlexGridGraph.cpp            ( +63 lines)
  - src/drt/src/dr/FlexGridGraph_maze.cpp       (+125 -3 lines)
  - src/drt/CMakeLists.txt                      ( +1 line)

## 2026-05-16 — P1 + P2 + P3 building blocks committed

What I did:
- Read FlexMazeIdx (src/drt/src/dr/FlexMazeTypes.h), FlexGridGraph dim
  accessors, frBaseTypes (frMIdx=int, frCost=unsigned int, frCoord=int,
  frDirEnum {UNKNOWN,D,S,W,E,N,U}). Located src list in src/drt/CMakeLists.txt.
- Added 5 new files under src/drt/src/dr/:
  - MazeNodeIndex.h: flat 3D node-id helpers
  - MazeSearchStateSoA.{h,cpp}: SoA per-node arrays + epoch reset
  - BucketedFrontier.{h,cpp}: vector-of-vectors frontier + overflow heap
- Registered the two .cpp files in src/drt/CMakeLists.txt drt_lib target.

What I observed:
- Pure additions: no existing source modified. Existing FlexWavefrontGrid
  + std::priority_queue path is unchanged. New code is dead until Phase 4
  wires it into FlexGridGraph::search().
- Did not build. User's drt-redesign V2.2 L2c1_8t ispd18_test9 run is in
  flight (PID 653254) — staying out of drt-build-env to avoid CPU
  contention.

Decision / next step:
- Pause Phase 4 (search rewrite) until build+test loop is available.
  The K-bias invariant from MEMORY.md project_routenet_kbias_invariant
  means the epoch bump that conceptually replaces resetStatus() must be
  threaded into every existing resetStatus() call site, NOT placed
  inside routeNet itself. Document this as Phase-4 constraint.
- Phase 5 (perf tuning) follows Phase 4.
- Once build is allowed: compile-only smoke test of the three new
  translation units first, then implement Phase 4 behind a
  -DGRID_STATE_ACCESS_BACKEND cmake option + DRT_USE_SOA_BACKEND runtime
  env var (matches plan.md §5 Phase 3 deliverable).

Files / commits / artifacts:
- 59914b8f4e grid_state_access P1+P2+P3 (6 files, +416 lines):
  - src/drt/src/dr/MazeNodeIndex.h         (113 lines)
  - src/drt/src/dr/MazeSearchStateSoA.h    ( 94 lines)
  - src/drt/src/dr/MazeSearchStateSoA.cpp  ( 51 lines)
  - src/drt/src/dr/BucketedFrontier.h      ( 61 lines)
  - src/drt/src/dr/BucketedFrontier.cpp    ( 95 lines)
  - src/drt/CMakeLists.txt                 ( +2 lines)
