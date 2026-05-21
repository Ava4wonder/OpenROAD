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
| Patch 3.1c+d sweep `G_drc_strong` (hot drc 1.25 / sev drc 1.50, marker mul 1.0) | 4+1 | 10:19 | 0 | 5,412,873 | 2,287,220 | iter-1 wscore −25.1% / iter-2 wscore −36.4% vs A_identity; iter-3 tail 3; wall ≤ P1 canonical |
| **Patch 3.3 locked default `H_drc_xstrong`** (hot drc 1.50 / sev drc 2.00, marker mul 1.0, decay override disabled) ★ | **4+1** | **≤ 10:19** | **0** | **~5,412,8xx** | **~2,287,0xx** | **canonical Patch 3 config (test9 8t): iter-1 wscore −30.7% / iter-2 wscore −43.9% vs A_identity; iter-3 tail collapses 3 → 1 (structural floor); no DRC regression. I (1.75/2.50) plateaued — ladder ceiling. G_no_decay ≈ G — decay override disabled in default.** |

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

## Locked default config (H, Patch 3.3 — test9 anchor)

```
percentile thresholds:   hot = top 10% of nonzero tiles by heat
                         severe = top 2%
                         static floor: 200 / 800

hot tiles:               drc_cost_mul    = 1.50
                         marker_cost_mul = 1.00
                         fixed_shape_mul = 1.00

severe tiles:            drc_cost_mul    = 2.00
                         marker_cost_mul = 1.00
                         fixed_shape_mul = 1.00
                         marker_decay_override = -1 (disabled)
```

These are now the Options defaults in `AdaptiveMarkerModel.h` and
fire whenever `OPENROAD_DRT_ADAPTIVE_MARKER=1`. All five values are
still env-var-overridable for future sweeps. Anchor evidence:
test9 ladder F → G → H monotonic on iter-1/iter-2 wscore; I
plateaued; G_no_decay ≈ G. Cross-design validation on test2 +
test10 is in flight.

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

## Main-track AdaptiveMarkerModel — TEST9 CEILING REACHED 2026-05-21

Three direct attempts to push past P3.3 H on test9 all closed
marginal or negative. P3.3 H_drc_xstrong locked at 4 opt + 1
cleanup / 10:19 wall / DRC=0 / WL 5,413,159 / vias 2,288,021 is
the **current AdaptiveMarkerModel main-track ceiling on test9**.

| Patch | Mechanism | Best result vs H | Verdict |
|---|---|---|---|
| **P5** | iter-0 uniform DRC mul | all 3 variants WORSE on wall, iter-0 markers UP +1.4-5.9% | NEGATIVE |
| **P6** | stubborn-net marker boost (×2 / ×3 / ×5) | mul=1.0: iter-2 −5 markers (−2.1%). Higher saturates. | MARGINAL |
| **P7** | per-worker init-marker DRC mul (N=20/50/100, after V1 plumbing bug fix `df906f3aa8`) | N=50: iter-1 −33 markers (−2.75%) but iter-0 +884 (+1.0%), wall +2s | MARGINAL |
| **P8** | rule-aware DRC mul boost | not run — pattern made outcome predictable | DROPPED |

**Pattern across all three independent experiments**: every
per-iter / per-worker / cross-iter scalar-mul perturbation
produces sub-1% wall changes, sign-ambiguous tradeoffs, and a
consistent iter-0 marker increase as a side effect. Outer iter
count never moves from 5.

**The structural diagnosis**: H is at a local optimum on test9
where the marker-heat sensor + DRC-mul actuator combination has
saturated. Further improvement requires either (a) a
fundamentally different mechanism (non-mul actuator, explicit
worker scheduling, net priority, ML-trained policy) or (b) a
different design where the model has slack (test10 has 16-44
iters → likely more room; test2 already converges faster than
H can affect).

All P5-P7 code stays on the branch behind env vars (defaults
preserve P3.3 H behaviour exactly).

## Patch 4 — RAMPED DOWN 2026-05-21

After Patch 4.3 V1 passed the convergence-safety bar on test9
(same iter count, same wall, vias −0.17 %, iter-2 −7 %), we
ramped down Patch 4 to return to the AdaptiveMarkerModel main
track. Reasoning: the field's actuator is via/cut spacing — a
minor DRC class — while the dominant runtime cost on test9 is
planar shorts + metal-spacing + congestion (per P3.5 profiling
showing iter 0+1 = 76 % of wall). Even tuned aggressively, the
ceiling on field-only speedup is small relative to what's
likely available from main-track AdaptiveMarkerModel work.

Code for Patches 4.1-4.3 stays on the branch behind the env var
`OPENROAD_DRT_CONSTRAINT_FIELD` (default-OFF, disabled-path
bit-identical to P3.3 H baseline). The V1 codebase
(ProjectionIndex + marker-conditioned splat + categorical cost
model + extended instrumentation) is correct and reusable if
we revisit later.

## Patch 4 Phase 4.2 — CLOSED as NEGATIVE result (2026-05-20)

Phase 4.2 V1 (via/cut spacing field with maze-cost integration)
shipped through 4 commits (a5574aec12 → c5292da25f). Test9 H + field
vs H_no_field result:

| Metric | H_no_field | H_plus_field (λ=1.0) | Δ |
|---|---:|---:|---|
| Outer iters | 5 | 7 | **+40 %** |
| Wall | 10:18 | 15:28 | **+50 %** |
| Trajectory | 92,709→1,200→243→1→0 | 93,598→1,373→317→5→2→2→0 | worse |
| Vias | 2,288,021 | 2,226,314 | **−2.7 %** (only positive signal) |
| DRC final | 0 ✓ | 0 ✓ | converges |

**The design diagnosis (sharp):**

> Phase 4.2 via/cut field targets via/cut risk.
> The dominant runtime/violation cost on test9 is iter 0/1 massive
> marker count — mostly planar shorts, metal-spacing, and congestion
> conflicts, expensive workers. **The via/cut field is the wrong
> first speedup actuator.**

P3.5 profiling already showed iter 0+1 dominate wall (76 % on
test9) with 12× p99-to-max worker imbalance. Those expensive iters
are expensive because of *planar* DRC repair work, not via-row
optimisation. P4.2's broad via-density penalty moved routing but
didn't reduce the actual repair cost — and in fact made it worse
by destabilising the initial topology.

A secondary pathology: the field activated on EVERY worker
(including clean ones), in EVERY iter (including iter 0 where no
markers exist to inform "risk"), and the splat source was blanket
"every via in worker's drcBox" rather than marker-conditioned
neighbourhoods. Net effect = via-density penalty, not DRC-risk
guidance.

## Patch 4.3 thesis — Projection-Indexed, Marker-Conditioned Fields

The field should not represent generic via density. It should
represent estimated future repair difficulty:

> `field_cost(candidate) ≈ risk that this candidate will create or
>  preserve expensive exact DRC markers`

Four corrections from P4.2:

  1. **Net-aware lookup** — already plumbed in P4.2.c
     (`current_routing_net_` set in `search()`, passed to
     `getViaRisk`); P4.3 extends with per-category weights:
     `same_net_weight = 0`, `fixed > marker_conditioned > diff_net`.

  2. **Marker-conditioned generation** — splats only from bloated
     `worker.getInitMarkers()` boxes + AdaptiveMarkerModel hotspot
     regions. Clean workers build no field.

  3. **Projection-filtered candidates** — x/y-interval + layer +
     owner + shape kind index; emit splats only for rule-relevant
     geometric pairs.

  4. **Conservative activation** — default-OFF for iter 0,
     cleanup, clean workers, low-global-marker regime.

Cost model becomes categorical (per-shape-kind weights) instead of
single λ-scalar. Same-net contribution explicitly 0.

Initial implementation should NOT chase speedup. The first goal is
**convergence-safe guidance** — final DRC = 0, no iter-count
regression, no worse trajectory. Runtime optimization comes only
after the field stops hurting convergence.

Ablation plan A0..A5 in plan.md. Required new instrumentation
(per-worker maze pushed/popped, FlexGC sub-phase wall, route_queue
size) extends P3.5 profiling.

## Patch 4 thesis — Constraint-Field-Guided Detailed Routing

Patch 3 closed with a sharp design lesson (heat = sensor, DRC mul =
actuator) and demonstrated the ceiling of policy-only optimisation
(H_drc_xstrong locked, K_taper as a non-monotonic refinement). The
Patch 3.5 profiling work then revealed the real runtime bottleneck:
**iter 0+1 dominate DRT wall (76% on test9), and within those iters
worker walls show 12× p99-to-max imbalance**. Reducing outer iter
count or marker count alone cannot break through this — the
expensive iters are expensive because of within-worker A* effort
plus FlexGC repair, not because of how many iters there are.

The Patch 4 thesis attacks this directly:

> Move DRT partially from `route → exact check → marker → repair`
> toward `compile geometry/rules → spatial constraint fields →
> DRC-aware path search → exact check for certification + remaining
> repair`.

Concretely: build worker-local **constraint fields** that splat
existing vias/cuts (and later fixed metal) into per-layer uint16_t
risk tiles. During FlexGridGraph maze expansion, add a small risk-
cost term so the search avoids high-risk tiles BEFORE the route is
committed and FlexGC materialises a marker. Exact FlexGC remains
the legality oracle. Default-OFF. Two MVP phases:

  1. **Phase 4.1** — skeleton + empty fields + stats CSV (no
     behaviour change).
  2. **Phase 4.2** — via/cut spacing risk field, queried at via
     expansion (`via_cost += lambda_cut * F_cut`).

Strict correctness: `OPENROAD_DRT_CONSTRAINT_FIELD=0` must restore
upstream-equivalent (or P3.3-H-equivalent) routing exactly.

This is a **research/refactor branch** — separate concerns from
Patch 3's policy-tuning track. See plan.md for full scope, phases,
metrics, success criteria, failure modes.

## Next actions

  1. ✅ **Patch 3.2 tail-marker sweep** — done. H1 partially
     confirmed (one structural iter-3 marker shared across all
     variants; second is also structural; third is policy-
     dependent and removed by H). The 4-iter test9 tail is
     structural — not a tuning issue.

  2. ✅ **H_drc_xstrong ladder probe** — done. H beats G on
     test9 iter-1/iter-2 wscore and collapses the iter-3 tail
     from 3 → 1.

  3. ✅ **G_no_decay_override ablation** — done. ≈ G across all
     metrics. `marker_decay_override` is unnecessary; disabled
     in defaults (-1).

  4. ✅ **Patch 3.3 — H locked as default** in `Options` struct
     (Patch 3 closure commit).

  5. **Cross-design validation on test2 + test10 with H +
     control set** — IN FLIGHT (Patch 3.3 follow-up). Variants:
     UNSET (true upstream baseline), A_identity (model on, all
     multipliers 1.0 — isolates instrumentation overhead),
     H (locked default). 6 runs total. test10 is the design
     with iteration headroom where iter-count drop is plausible;
     test2 is a smaller sanity check that H does not regress on
     an already-tight design.

  6. **Patch 4 (guide relaxation) BLOCKED** pending step 5.
     H already delivers strong intermediate improvement without
     guide relaxation. Guide rewriting is more invasive and
     would add detour cost; justified only if test10 shows a
     guide-induced central-congestion failure mode that DRC-only
     policy cannot break.

  7. **Patch 4 Phase 4.2** — CLOSED as negative result (this
     section above; commits a5574aec12 → c5292da25f). Code stays
     on the branch behind the OPENROAD_DRT_CONSTRAINT_FIELD env
     var (default-OFF, so disabled-path is bit-identical to P3.3).
     Same-net filter + lambda scaling left in place — they're not
     wrong, they're just insufficient on their own.

  8. **Patch 4.3** (NEW ACTIVE BRANCH) —
     Projection-Indexed, Marker-Conditioned Constraint Fields.
     Implementation order: (a) marker-conditioned splat source +
     activation policy (replace blanket via splat with
     getInitMarkers-bloated regions); (b) per-shape-kind cost
     weights; (c) projection index over candidates; (d) extended
     instrumentation (maze push/pop + FlexGC sub-phase wall +
     route_queue size). Ablation A0..A5. See plan.md.

  9. **Patch 5 / Patch 6 deprioritised**: rule-aware DRC policy +
     layer-aware actuator. Subsumed by Patch 4 once 4.3 ships —
     becomes a refinement (per-rule-class field weights) rather
     than a separate patch.
