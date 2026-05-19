# outer_loop_plus — execution log

Append-only, dated, precipitating change + measurement.

---

## 2026-05-19 — Branch rebased onto upstream/master (fresh start)

Previous `outer_loop_plus` (branched off `redesign_rebase_v3` tip
`a8af5f046c`) was deleted local + remote. Reason: a UNSET-env-var run on
that base still produced 4 iters / 16:30 wall because the V2.6.h K-bias
work was compiled in via ENABLE_DRT_REDESIGN_OVERLAY + the V2 env vars
were set on the run command. That is NOT a clean upstream baseline.

New `outer_loop_plus` branches from `upstream/master` HEAD = commit
`fb6bde3f48` (= The-OpenROAD-Project/OpenROAD@master pulled this session
via the `upstream` git remote). With this base, the env-UNSET binary
should reproduce the user-reported 7 iters / ~14:06 wall.

The 5 AdaptiveMarker* source files I created on the previous branch were
saved to `/tmp/olp_redo/` before the branch reset and copied back into
the new working tree. No content change between the two copies.

Decisions inherited from earlier session:
  * Test set Day 1: test9 only; add test10 at Patch 4+; add test2/3 at
    Patches 8-9 if results warrant.
  * Build system: CMake only.
  * H100 build location: `OpenROAD-baseline` worktree (separate from
    `OpenROAD-redesign` which hosts `redesign_rebase_v3`).

Patch 1 (skeleton + disabled model field on FlexDR) is the immediate
next commit. Verification run target: env UNSET → ~7 iters / ~14m wall
/ 0 DRV / matching WL+vias on ISPD-18 test9 8t.

Expected next entry: Patch 1 commit hash + verification run metrics.

---

## 2026-05-19 — Patch 1 committed + UNSET verification run

Patch 1 = commit `c9f9c99dd2` on `outer_loop_plus` (parent `fb6bde3f48` =
upstream/master HEAD). Skeleton files + `CMakeLists.txt` integration +
forward decl + unique_ptr field on FlexDR + conditional construction
gated on `OPENROAD_DRT_ADAPTIVE_MARKER=1`.

Build location on H100: `/work/baseline/build-container` (= host worktree
`/home/azureuser/openroad-profile/OpenROAD` checked out at
`outer_loop_plus_local` tracking `fork/outer_loop_plus`). Stashed
`grid_state_access` branch's uncommitted work first.

Build hiccups (both resolved, recorded for future reference):
  1. CMake configure failed because or-tools requested Boost 1.87.0 but
     container has 1.89.0. Fix: `cmake -DBoost_DIR=/usr/local/lib/cmake/Boost-1.89.0 ..`.
  2. Initial build hit `No rule to make target '.../src/sta/sdc/Variables.tcl'`
     — `src/sta` submodule was out of sync after branch switch. Fix:
     `git submodule update --init --recursive src/sta`.
  3. LTO link needs ~10 GB temp on `/dev/root` which is at 100% used.
     Fix: `TMPDIR=/dev/shm` (294 GB tmpfs).

Final build artifact: `/home/azureuser/openroad-profile/OpenROAD/build-container/bin/openroad`
(110 MB) at 2026-05-19 20:56:21 UTC.

### Patch 1 verification run (env UNSET) — test9 8t

Output dir: `/home/azureuser/openroad-profile/OpenROAD-flow-scripts/flow/ispd_runs/ispd18_test9_olp_p1_unset_8t/`.

| Metric | Value |
|---|---|
| Optimization iters | 4 (0, 1, 2, 3) |
| Cleanup iter | 1 ("Start 4th guides tiles iteration") |
| DRT wall | 10m 27s |
| DRT CPU | 1h 03m 45s |
| Final DRC violations | 0 |
| Wirelength | 5,412,412 µm |
| Total vias | 2,284,621 |
| Peak memory | 8.35 GB |
| Avg CPU utilisation | ~6.1 threads of 8 |

**The Patch 1 disabled-shell guarantee holds.** No env vars set, model
not constructed, routing identical to a pure upstream-master build.

**Important finding:** these numbers are 4 iters / 10:27 — BETTER than
the user's earlier 7 iters / 14:06 reference. The difference is 504
upstream-master commits between the older docker image and current
`fb6bde3f48`. Upstream has independently improved DRT convergence
during that window. New canonical baseline locked into `current_state.md`.

Expected next entry: Patch 2 commit hash + observation-CSV verification
(env SET) showing observation logs + bit-stable routing.
