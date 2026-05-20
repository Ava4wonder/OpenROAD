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
| Patch 2.1 SET (MinStep + expanded classify) | 4 opt + 1 cleanup | 10:29 | 0 | 5,412,412 ✓ | 2,284,621 ✓ | bit-identical routing; expanded switch caught 0 new markers — the 47K "Other" bucket is null-constraint markers from FlexGCWorker, not unrecognized enum cases |
| Patch 3.1b SET (iter ≥ 1, fixed thresh 200/800, drc+mark 1.10/1.25 + 1.25/1.50) | 4+1 | 10:38 | 0 | 5,412,872 | 2,286,327 | iter-1 markers −9.3% vs UNSET, iter-2 −12.7%; first positive per-iter signal |
| **Patch 3.1c+d sweep `G_drc_strong`** (percentile thresh, hot drc 1.25 / sev drc 1.50, marker mul 1.0) ★ | **4+1** | **10:19** | **0** | **5,412,873 (+0.0087%)** | **2,287,220 (+0.114%)** | **current best: iter-1 wscore −25.1% / iter-2 wscore −36.4% vs A_identity; iter-3 tail still 3 (rule mix shifts to 1sh+2ms); wall slightly BETTER than P1 canonical** |

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

## Design lesson from the Patch 3.1c+d sweep + ablation (sharp + defensible)

```
Persistent frMarker heat:    GOOD sensor    (selects WHICH workers)
Marker-cost multiplier:      BAD actuator   (creates detours, adds tail markers)
DRC-cost multiplier:         USEFUL actuator (raises legality sensitivity)
```

The original "hot region → raise marker cost" intuition was empirically
rejected by the F-vs-D and G-vs-E ablation pairs (DRC+marker uniformly
loses to DRC-only across iter-1, iter-2, vias, and tail-markers).
Historical marker heat tells the model **where** to intervene;
**how strongly** to penalise comes from current DRC cost (live
legality risk), not from stale marker history.

For the contribution claim, this reframes the result:

> The AdaptiveMarkerModel's useful contribution is **not "more marker
> cost"**. It is **persistent-marker-driven regional selection for
> stronger DRC-sensitive routing**.

## Best-current-candidate config (G, test9-only)

```
percentile thresholds:   hot = top 10% of nonzero tiles by heat
                         severe = top 2%
                         static floor: 200 / 800

hot tiles:               drc_cost_mul    = 1.25
                         marker_cost_mul = 1.00
                         fixed_shape_mul = 1.00

severe tiles:            drc_cost_mul    = 1.50
                         marker_cost_mul = 1.00
                         fixed_shape_mul = 1.00
                         marker_decay_override = 0.99 (pending ablation)
```

Code defaults still hold D's values; this is the next config to lock
into Options once H + G_no_decay confirm and test10 supports it.

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

  1. **Patch 3.2 tail-marker sweep** (in flight on H100): A, D, F, G
     all rebuilt with tail-marker CSV dump enabled. Compare iter-3
     tail bboxes + nets across variants. If A/D/F share the same
     physical markers, H1 (one geometric knot → multiple markers)
     is confirmed and the 4-iter floor on test9 is structural, not
     a tuning issue.

  2. **One more DRC-only ladder point: H_drc_xstrong**
     (`hot_drc=1.50, severe_drc=2.00, marker_mul=1.0`). One run.
     Probes whether G is already at the sweet spot or stronger DRC
     pressure helps further without via blowup.

  3. **Ablate `marker_decay_override=0.99`**: run G with the
     override disabled (`severe_decay_override=-1`). Tests whether
     G's gain comes from DRC mul alone or also from stronger heat
     persistence. Removes a confounder.

  4. **Run A / F / G / G_no_decay (and H if it survives test9)
     on test10.** The minimum cross-design validation. Test9's
     4-iter tail looks structural, so iter-count drop is unlikely
     here regardless of policy. test10 is the design with more
     iteration headroom; if G drops iters there, we have a real
     end-to-end contribution.

  5. **Patch 4 (guide relaxation) BLOCKED** pending step 4. G
     already delivers strong intermediate improvement without
     guide relaxation. Guide rewriting is more invasive and would
     add detour cost; justified only if test10 shows a guide-
     induced central-congestion failure mode that DRC-only policy
     cannot break.

  6. **Patch 5 redesigned**: original "rule-aware marker
     increments" goal is now obsolete (marker mul is bad). The
     refined goal: **rule-aware DRC-policy / marker-policy
     separation**. E.g., a short-heavy hotspot raises DRC mul
     strongly; a cut-spacing hotspot raises via-related cost; an
     EOL-heavy hotspot uses a directional/stub-aware penalty.
     Use marker history to *classify the failure mode*, then
     choose a rule-specific actuator. See plan.md.
