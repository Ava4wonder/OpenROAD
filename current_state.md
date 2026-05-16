# grid_state_access — Current State

## Contribution claim

A CPU-only refactor of OpenROAD drt's maze search that replaces the
priority-queue-of-objects frontier with (a) flat SoA grid-state arrays and
(b) a bucketed/vector frontier, behind a compile and runtime flag, with
identical or near-identical QoR vs master baseline.

## Evidence base (objective, with provenance)

| date | metric | value | source |
|---|---|---|---|
| 2026-05-12 | open-list share of CPU time (RouterAcc DATE'26) | 95% 1t / 88% 8t | RouterAcc §II.B.2 |
| 2026-05-10 | nangate45/swerv DRT wall (master, 80t, H100) | 267 s | ~/net_order_test/logs/n45_swerv_baseline_5_2_route.log |
| 2026-05-10 | n45/swerv iter-0 markers | 37,496 | same |
| 2026-05-10 | OpenROAD nightly CI n45/swerv | 435 s / iter-0=37,551 | jenkins.openroad.tools build #6985 |

## Belief state (interpretive)

- The maze-search frontier IS the hot loop (per RouterAcc and corroborated
  by our prior cache-miss-prone profile on H100). SoA + bucketed frontier
  attacks both: (i) reduces per-element memory footprint and improves
  prefetcher behaviour, (ii) replaces log-N heap ops with O(1) bucket
  insert + amortized linear pop.
- The existing cost model is complex and history-dependent. Best to keep
  cost-calculation logic verbatim and only change storage + selection.
- The K-bias invariant (recursive routeNet must `resetStatus()`) means the
  epoch bump that replaces `resetStatus()` MUST happen at every existing
  `resetStatus()` call site, not in `routeNet` itself.

## Active hypotheses

- **GS-H1**: Frontier ops account for >50% of drt CPU time on n45/swerv
  when measured with cachegrind on the hot kernel.
  - falsifies if: cachegrind shows <30% in frontier code paths.
- **GS-H2**: SoA layout + epoch reset gives ≥2× speedup on isolated maze
  search microbenchmark vs current code.
  - falsifies if: <1.3× microbench speedup.
- **GS-H3**: Bucketed frontier (bucket_width=1) preserves DRV count to
  within ±5% on swerv + ispd18_test9 vs baseline.
  - falsifies if: any test regresses by >5% DRVs.
- **GS-H4**: End-to-end DRT speedup ≥1.5× on n45/swerv when both SoA + bucket
  enabled.
  - falsifies if: <1.2×.

## Risks

- **GS-R1**: History-dependent cost may need multi-label-per-node; one-label
  pruning could degrade QoR. Mitigation: keep PQ fallback; add best-K labels
  only if measured QoR loss.
- **GS-R2**: Cache lines from packed SoA may evict each other if access
  pattern is poor. Mitigation: profile, reorder fields, group by access
  affinity.
- **GS-R3**: K-bias invariant violation — epoch bump misplaced. Mitigation:
  centralize bump into `resetStatus()` wrapper; existing call sites become
  the only path.
- **GS-R4**: H100 disk pressure (711 MB free as of 2026-05-16). Build needed
  for Phase 1-3 verification will require ~0.3 GB. Track in execution log.
- **GS-R5**: Another openroad process (PID 606423) is on the host and the
  user's redesign run uses drt-build-env. Building or running here risks
  CPU contention. Mitigation: defer build; do file edits only.

## Next actions

- [x] Create `grid_state_access` branch + scaffold docs. (7ede745fb5)
- [x] Read `src/drt/src/dr/FlexGridGraph*`, `FlexMazeTypes.h`, `frBaseTypes.h`
      to map types.
- [x] Phase 1: `src/drt/src/dr/MazeNodeIndex.h` flat indexing helpers
      (59914b8f4e).
- [x] Phase 2: `src/drt/src/dr/MazeSearchStateSoA.{h,cpp}` SoA arrays +
      epoch reset (59914b8f4e).
- [x] Phase 3: `src/drt/src/dr/BucketedFrontier.{h,cpp}` vector-of-buckets
      + overflow heap (59914b8f4e).
- [ ] Add unit tests under `src/drt/test/` for MazeNodeIndex round-trip
      and neighbor lookup (deferred: gtest infrastructure not set up
      without a successful build).
- [x] Phase 4: SoA + bucketed-frontier search path behind
      `DRT_USE_SOA_BACKEND=1` runtime flag (a0c82935fa). K-bias
      invariant honored: epoch bump lives in resetStatus(), the
      canonical reset site. Build verification pending.
- [ ] Build verification of P1-P4 inside drt-build-env once user
      authorizes (likely after their concurrent redesign run
      finishes). Validation checklist in execution_log.md.
- [ ] Phase 5: batch processing, prefetch, optional TBB. After Phase 4
      stable.
