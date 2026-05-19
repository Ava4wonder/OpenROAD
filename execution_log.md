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
