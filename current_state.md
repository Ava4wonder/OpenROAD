# outer_loop_plus — current state

## Contribution claim

Introduce a persistent, in-memory, global model inside `src/drt/`
(`AdaptiveMarkerModel`) that consumes existing `frMarker` objects and worker
outcomes and emits per-region/per-layer policy modifiers consumed by each
`FlexDRWorker` as a read-only snapshot. Improve weighted marker score and/or
outer-iter count on ISPD-18 test9 (then test10) **vs upstream master**
without regressing build, existing regressions, or runtime materially.

Branch base: `upstream/master` HEAD (= `The-OpenROAD-Project/OpenROAD@master`,
commit `fb6bde3f48`). The V2.6.h K-bias work on `redesign_rebase_v3` is on
a separate branch and is NOT part of this baseline.

Targets:
  * Weighted marker score — improve vs upstream-master on test9 8t.
  * Outer iter count — improve vs upstream-master's 7 iters on test9 8t.
  * Build green + DRT regressions pass with feature disabled.
  * Wall runtime — within +10% vs upstream-master's ~14:06.

## Evidence base (objective, with provenance)

| Variant | Iters | DRT wall | DRC | WL (µm) | Vias | Source |
|---|---|---|---|---|---|---|
| Upstream master via `openroad/orfs:latest` docker image (older master) | 7 | ~14:06 | 0 | — | — | user-reported reference; STALE — upstream has moved 504 commits forward |
| **Upstream master `fb6bde3f48` (this branch, Patch 1, env UNSET)** | **4 opt + 1 cleanup** | **10:27** | **0** | **5,412,412** | **2,284,621** | **canonical baseline; measured 2026-05-19 on H100** |
| Patch 2 UNSET (regression check) | 4 opt + 1 cleanup | 10:18 | 0 | 5,412,412 ✓ | 2,284,621 ✓ | bit-identical to canonical baseline |
| Patch 2 SET (observation enabled, identity policy) | 4 opt + 1 cleanup | 10:23 | 0 | 5,412,412 ✓ | 2,284,621 ✓ | +5s wall (+0.8%) for observation; routing bit-identical to UNSET |

**Key finding from Patch 1 verification (2026-05-19):** Today's upstream
master (`fb6bde3f48`) already reaches 4 optimization iters DRC-clean on
ISPD-18 test9 8t in 10m 27s. This is 504 commits ahead of the upstream
that produced the earlier 7/14:06 reference, and the routing has improved
in that window. **Canonical baseline for this branch is 4 opt iters / 10:27
/ WL 5,412,412 / vias 2,284,621**, not 7/14:06.

Implication for the AdaptiveMarkerModel contribution claim: improving on
4 iters is materially harder than improving on 7. The realistic L1 target
becomes **3 opt iters DRC-clean** (or weighted-marker-score reduction at
4 iters if the iter floor can't be pushed below 4 on this design).

DRT-0267 timing breakdown for the canonical baseline:

| Phase | Elapsed |
|---|---|
| Iter 0 (initial routing) | ~4:30 |
| Iter 1 (opt) | 3:27 |
| Iter 2 (opt) | 0:43 |
| Iter 3 (opt) | 0:21 |
| Iter 4 (guides tiles cleanup) | 1:26 |
| **DRT total** | **10:27** |

CPU 1h 03m 45s = 6.1-thread avg utilization. Peak memory 8.35 GB.

Measurement env (canonical): H100 host `azureuser@20.110.81.12`, container
`drt-build-env`, ISPD-18 test9, 8 OMP threads. **No V2.6.h env vars are set
on this branch** — the binary is built from upstream master with no V2
patches, so those env vars have no effect.

Driver TCL: `/work/orfs/flow/ispd_bench/run_test.tcl` (env-driven design
selection via `ISPD_DESIGN` / `ISPD_BENCH_DIR` / `ISPD_OUT_DIR`).

H100 build location for this branch: `/work/baseline` bind-mount (the
`OpenROAD-baseline` worktree), kept separate from `/work/redesign` which
hosts the `redesign_rebase_v3` work.

## Belief state (interpretive)

  * Upstream-master on ISPD-18 test9 8t is reported as 7 iters / ~14:06. The
    bulk of those 7 iters is "search-and-repair" — each iter accumulates
    markers, then the next routes again to fix them. The adaptive model
    hypothesis: per-region persistent heat lets later iters route
    DIFFERENTLY in hot regions before the marker engine forces it,
    reducing both DRC and total iter count.
  * The probe data from `redesign_rebase_v3` (98-99% of K=2 fire wall in
    `gridGraph_.search()`, 91% skip path) is informative but does NOT
    apply here — V2.6.h's K-bias isn't compiled into this branch.
  * Different angle, different mechanism: AdaptiveMarkerModel shapes
    cost LANDSCAPE per region; V2.6.h K-bias explores alternate
    topologies per net. Both could compose later if both prove out.

## Hypotheses (with falsification criteria)

  * **H-adapt-1:** Persistent region heat (decayed across iters) correlates
    with where late-iter markers concentrate. Falsified if hotspot tiles
    from iter 0-1 don't overlap iter-N marker concentration.
  * **H-adapt-2:** Scalar drcCost/markerCost multipliers (1.25× in hot,
    1.50× in severe) reduce final DRC and/or iter count on test9.
    Falsified if enabled-vs-disabled shows no benefit OR regression.
  * **H-adapt-3:** Guide relaxation (`relax_guide=true`) in severe-hotspot
    workers helps when the guide-imposed track conflicts with marker
    geometry. Falsified if relaxation increases marker count OR WL.
  * **H-adapt-4** (deferred to Patches 5-6): Rule-aware + layer-aware
    marker increments tune cost more precisely. Falsified per-layer if
    increment bump doesn't reduce that layer's markers.

## Risks

  * **Determinism** — model state at end of iter N depends on order of
    marker observations. Counted once per outer iter (not per worker) to
    avoid double-counting from overlapping worker DRC boxes.
  * **Cost-multiplier blowup** — caps: `drc_cost_mul ≤ 2.0`,
    `marker_cost_mul ≤ 2.0`, `marker_decay_override ≤ 0.99`. Activation
    gated on `iter ≥ 3` and `same hotspot persists ≥ 2 observations`.
  * **Guide relaxation pathology** — relaxing guides globally hurts
    wirelength. Restrict to severe-hotspot workers only; require hotspot
    persistence ≥ 2 iters before enabling.
  * **Marker-source ownership extraction is fragile** —
    `extractMarkerNets()` starts conservatively with `frcNet` owners only;
    add inst-term / pin-fig / block-object owners in later patches.
  * **Upstream master is a moving target.** This branch is rebased on
    `fb6bde3f48`. If upstream lands large drt changes during this work,
    we may need a periodic rebase. Tracking via the `upstream` git
    remote.

## Pivot criteria

Stop pursuing a patch K if:
  * Three consecutive runs with K's feature enabled fail to improve weighted
    score on test9.
  * K introduces regression on test10 once test10 is added.
  * K's feature-disabled regression run differs from upstream-master
    baseline (build-green rule violation).

## Next actions

  1. **Patch 1 verification run** (in progress queueing): test9 8t with
     `OPENROAD_DRT_ADAPTIVE_MARKER` UNSET on the new upstream-master-based
     binary. Expect ~7 iters / ~14m wall / 0 DRV. This both validates the
     Patch 1 disabled-shell guarantee AND establishes the canonical
     baseline measurement on this branch's exact build.
  2. After Patch 1 verifies: Patch 2 (observation + CSV logging).
