# outer_loop_plus — plan

## Goal

Add an in-memory, persistent, global model inside `src/drt/` — the
`AdaptiveMarkerModel` — fed by existing `frMarker` objects and worker outcomes,
default-disabled so OpenROAD builds and all existing regressions run unchanged.

The branch is rebased directly on top of **`upstream/master`** (= The-
OpenROAD-Project/OpenROAD `master`). With the env var unset the binary must
behave exactly like a clean upstream-master build — this is the *true*
baseline the adaptive model improves against (`redesign_rebase_v3`'s V2.6.h
K-bias work is a SEPARATE branch and is NOT part of this baseline).

The model emits a read-only policy snapshot consumed by each `FlexDRWorker`
before `main()`. Policy modifiers include:
  * per-region cost multipliers (drcCost / markerCost / fixedShapeCost)
  * per-layer marker-cost multipliers
  * via-cost multiplier
  * guide-penalty multiplier (`relax_guide` + `guide_cost_mul`)
  * optional net reroute priority
  * optional clip-size escalation hint

The conceptual delta from current upstream DRT:
```
upstream master:     current markers + fixed global iter schedule
AdaptiveMarkerModel: persistent marker history + per-region policy modifiers
```

## Design principles

  1. **Default behavior = upstream-master behavior exactly.** Feature gated
     behind `OPENROAD_DRT_ADAPTIVE_MARKER=1` (later promoted to a Tcl option).
  2. **Global model, local workers.** `AdaptiveMarkerModel` is owned by
     `FlexDR`, mutated only on the FlexDR thread. Each `FlexDRWorker` receives
     a read-only `AdaptiveWorkerPolicy` snapshot before `main()` and produces
     an `AdaptiveWorkerStats` record after. Stats are merged into the model
     serially after each OMP-parallel worker batch. No atomics, no
     shared-state mutation from workers.
  3. **No `FlexGridGraph` mutation in early patches.** First-version policy
     manipulates only scalar worker costs (existing knobs). Rule- and
     layer-aware marker increments are deferred to Patches 5/6.
  4. **Every patch buildable, regressions green.** 7-patch sequence below.

## File layout (added)

```
src/drt/src/dr/AdaptiveMarkerTypes.h   — POD structs (rule class, obs, policy, stats)
src/drt/src/dr/AdaptiveMarkerUtils.h
src/drt/src/dr/AdaptiveMarkerUtils.cpp — classifyConstraint, extractMarkerNets, getRuleWeight, getRuleAwareInflatedBox
src/drt/src/dr/AdaptiveMarkerModel.h
src/drt/src/dr/AdaptiveMarkerModel.cpp — the persistent model itself
```

## Hooks in FlexDR (Patch 1+)

  1. `FlexDR` owns `std::unique_ptr<AdaptiveMarkerModel> adaptive_marker_model_`.
  2. (Patch 2) Once per outer iter: `beginOuterIter(iter)` decays heat → `observeGlobalMarkers(topBlock_->getMarkers())` once → `endOuterIter()` updates hotspots.
  3. (Patch 3) Before each worker batch: `getWorkerPolicy(route_box, drc_box, iter, base_drc, base_marker, base_fixed, base_decay)` → `worker.setAdaptivePolicy(policy)`.

Worker uses policy only for scalar cost multipliers in early patches. Worker
returns `AdaptiveWorkerStats` (rule counts + congestion flag + box) that
FlexDR ingests after `#pragma omp parallel for` returns.

## Patch sequence (every commit buildable + regressions green)

  1. **Patch 1** ✅ — Skeleton + disabled model field on FlexDR. Verified
     bit-identical to upstream master on env UNSET.
  2. **Patch 2** ✅ — Marker observation + CSV logging. Bit-identical
     routing on env SET (Patch 2.1 added MinStep rule class).
  3. **Patch 3** ✅ — Worker `AdaptiveWorkerPolicy` snapshot + scalar
     multipliers. Iterated 3.1a (instrumentation) / 3.1b (iter gate
     lowered to ≥ 1) / 3.1c+d (percentile thresholds + env-var multiplier
     sweep) / 3.2 (per-marker tail dump) / 3.3 (lock H defaults).
     **Final candidate: config H_drc_xstrong** (hot drc 1.50, severe
     drc 2.00, marker mul 1.00, decay override disabled). On test9 8t:
     iter-1 wscore −30.7%, iter-2 wscore −43.9% vs A_identity; iter-3
     tail collapses from 3 → 1 marker (structural floor), wall faster
     than P1 canonical, no DRC regression. I_drc_uxstrong (1.75/2.50)
     plateaued at H — ladder ceiling confirmed. G_no_decay ≈ G —
     `marker_decay_override` is unnecessary, set to -1 (disabled) by
     default. **Patch 3.2 H1 result:** test9 iter-3 tail contains one
     genuine structural marker (layer 16, no net owner — pin-access /
     macro-edge residue) shared across all variants; the second iter-3
     marker observed across most variants is also shared structurally;
     the third is policy-dependent and removed by H. So the 4-iter
     test9 tail is structural, not a tuning issue, and H is at the
     test9-on-this-design ceiling.
  4. **Patch 4 — REDESIGNED**: Constraint-Field-Guided Detailed Routing
     (proactive DRC-aware cost layer). The original "guide relaxation"
     goal was abandoned: Patch 3.5 profiling showed wall time is
     dominated by early expensive iters and pathological workers, not
     by guide-imposed detours. Marker-driven repair is reactive; we
     need a proactive spatial constraint layer so the maze search can
     see DRC risk BEFORE exact markers are materialised.

     **Thesis:** move DRT partially from
       `route → exact-check → marker → repair`
     toward
       `compile geometry+rules → spatial risk fields → DRC-aware path
        search → exact-check for certification + remaining repair`

     **MVP scope** (default-off, env `OPENROAD_DRT_CONSTRAINT_FIELD=1`):
       * Phase 4.1 — Skeleton: `ConstraintField{,Builder,Types}.{h,cpp}`,
         worker-local fields, `constraint_field_stats.csv`. No
         routing behaviour change.
       * Phase 4.2 — Via/cut spacing risk field: splat existing vias
         and cuts into per-cut-layer uint16_t risk tiles; query
         during FlexGridGraph via expansion; add cost term
         `via_cost += lambda_cut * F_cut`. Cost-only (not blockage);
         exact FlexGC remains the legality oracle.

     **Strict correctness:** field is advisory. Disabling the env var
     must restore exactly upstream-master / Patch 3.3-H behaviour.
     Never suppresses exact DRC.

     **Deferred** (future Patch 4.3/4.4 if MVP wins):
       * Planar metal-spacing dilation field
       * EOL / corner directional kernels
       * Projection-indexed exact-candidate prefilter
       * Marker-history diffusion
       * Global (not just worker-local) field
       * GPU/fluid solver

  5. **Patch 4 (guide relaxation) — DROPPED**. Subsumed by the
     redesigned Patch 4 above. The original 3.1c+d sweep already
     showed that DRC-only policy (no guide rewriting) delivers strong
     improvement; profiling later confirmed runtime is dominated by
     within-worker A* effort, not by guide-imposed paths.
  6. **Patch 5 / Patch 6 — DEPRIORITISED**: rule-aware DRC policy +
     layer-aware actuator. These are still on the roadmap but lower
     priority than Patch 4. The Patch 3.5 profiling diagnosis
     reframed the runtime problem: it's not about which rule the
     model amplifies, it's about whether the router can see DRC risk
     before exact markers materialise. Patch 4's constraint-field
     layer attacks that root cause directly. Patches 5/6 (rule-aware
     mul scaling) become refinements of Patch 4 once it ships.
  7. **Patch 7** — Net-score logging + weak queue priority (tie-breaker
     only; no full reorder). Unchanged scope.

After Patch 7 if results justify: conflict-graph prototype (diagnostic
only, no routing impact yet).

## Design lesson (forced by Patch 3.1c+d sweep + DRC-only ablation)

```
Persistent frMarker heat:    GOOD sensor    (selects WHICH workers)
Marker-cost multiplier:      BAD actuator   (over-penalises stale local scars,
                                             pushes A* into detours, creates
                                             extra tail markers)
DRC-cost multiplier:         USEFUL actuator (raises live legality sensitivity
                                             in the selected workers)
```

Direct ablation evidence (test9 8t):
  - Mild pair (D = DRC+marker vs F = DRC-only): F beats D on iter-1,
    iter-2, vias, wall.
  - Strong pair (E = DRC+marker vs G = DRC-only): G beats E by 10%
    on iter-1 markers, halves the iter-3 tail (3 vs 6), fewer vias.
  - Marker-only configs (B, C): regress across the board; create
    new iter-3 failure modes (EOL + cut_spacing markers absent from
    A/D/F/G tails).

The original Patch 5 intuition "hot region → raise marker cost" was
plausible but the data forces the better rule: "hot region → raise
DRC cost, leave marker cost at 1.0."

## Patch 3 closure summary

Patch 3 sweep is closed. Final state:

  - ✅ **H_drc_xstrong (1.50/2.00)** locked as default (Patch 3.3).
  - ✅ **I_drc_uxstrong** plateaued at H — ladder ceiling confirmed.
  - ✅ **G_no_decay_override** ≈ G — `marker_decay_override` disabled
    by default (-1).
  - ✅ **Patch 3.4 K_taper** (Patch 3.4): tapers DRC mul down as
    markers shrink. test10: 22 outer iters (vs UNSET 17, H 44),
    wall 37:02 (vs UNSET 39, H 36:34). Cleanup tail cut in half vs
    H but loses ~25% per-iter quality iter 5-9. Held pending Patch
    3.5 profiling.
  - ✅ **Patch 3.5 runtime profiling**: iter + worker CSVs gated by
    `OPENROAD_DRT_ADAPTIVE_PROFILE=1`. test9 SMOKE_H verified
    instrumentation. Key findings even from smoke: iter 0+1 dominate
    DRT wall (76% of test9 total), 12× p99-to-max worker imbalance,
    iter 4 cleanup = 86s on a single worker. Cross-design profile
    paused — pivoted to Patch 4 once it became clear runtime is not
    bottlenecked by outer-iter count.

## Patch 4 — Constraint-Field-Guided Detailed Routing (research/refactor)

**Background**: Patch 3 established AdaptiveMarkerModel as a useful
persistent hotspot sensor. Regional DRC-cost amplification improves
convergence quality and on some designs reduces iteration count.
But Patch 3.5 profiling showed runtime is dominated by early
expensive DRT iterations and pathological workers, not by outer-
loop count. Reducing markers or iteration count isn't sufficient
for runtime speedup. We need a more fundamental routing-risk
abstraction.

**Framing — what NOT to do**:
  * Do NOT replace FlexGC exact checking.
  * Do NOT encode all LEF58 rules into a fluid model.
  * Do NOT remove frMarker or route_queue marker logic.

**Core hypothesis**: existing detailed routers treat many DRCs as
discrete posteriori marker events. We can improve runtime and
convergence by compiling selected design rules into spatial
constraint fields, allowing DRC-aware path search BEFORE violations
are materialised. Exact FlexGC remains the legality oracle.

**Architectural shift**: from
```
route → exact check → marker → repair
```
toward
```
compile geometry/rules → spatial constraint fields → DRC-aware path
search → exact check only for certification + remaining repair
```

**Module layout** (new files):
```
src/drt/src/dr/ConstraintFieldTypes.h
src/drt/src/dr/ConstraintField.h
src/drt/src/dr/ConstraintField.cpp
src/drt/src/dr/ConstraintFieldBuilder.h
src/drt/src/dr/ConstraintFieldBuilder.cpp
```
Worker-local first (no global field, no cross-thread mutation).

**Phases**:
  * **Phase 4.1** — Skeleton: types, empty fields, `constraint_field_
    stats.csv`. No behaviour change. Smoke test9.
  * **Phase 4.2** — Via/cut spacing field: splat existing vias/cuts
    into per-cut-layer uint16_t risk tiles; query during via
    expansion in FlexGridGraph; `via_cost += lambda_cut * F_cut`.
    Cost-only, not blockage. Eval test9 → test2 → test10.

**Deferred** (Phase 4.3+ if MVP wins):
  * Planar metal-spacing dilation field (Phase 4.3)
  * EOL / corner directional kernels (Phase 4.3)
  * Projection-indexed candidate prefilter (Phase 4.4, diagnostic
    only first)
  * Marker-history diffusion into smoother potential field
  * Global (chip-wide) field
  * GPU / fluid solver

**Strict correctness**:
  * Field is advisory. Final legality remains FlexGC + frMarker +
    existing repair loop.
  * `OPENROAD_DRT_CONSTRAINT_FIELD=0` (default) MUST produce
    upstream-equivalent routing.
  * Field must never suppress exact DRC.

**Env vars**:
  * `OPENROAD_DRT_CONSTRAINT_FIELD=1` enable
  * `OPENROAD_DRT_CONSTRAINT_FIELD_STATS_DIR=path` output dir for CSV

**Metrics per design × variant** (UNSET, H_static, H_static + field):
  * Final DRC, DRT wall, outer iters, WL, vias, peak mem
  * worker_wall_sum, worker_wall_max, active workers
  * per-rule marker counts (esp. cut_spacing, metal_spacing)
  * field build_wall_ms, field memory bytes
  * field queries / hits / cost added

**Key acceptance criterion**: profiling showed iter 0+1 dominate
wall — therefore the field MUST help early expensive workers. A
late-stage-only improvement is not enough.

**Success criteria (MVP)**:
  * Final DRC = 0
  * Field overhead small
  * worker_wall_sum or max in iter 0/1 decreases
  * cut/spacing markers decrease earlier
  * WL/via does not regress significantly

**Failure modes to watch**: false-positive detours, dense field
memory blowup, build time > saved routing time, same-net over-
penalisation, new EOL/cut markers from routing avoidance.

## Metrics (per design, per variant)

Weighted marker score (not raw count):
```
score = 100·shorts + 80·cut_shorts + 40·metal_spacing
      +  35·cut_spacing + 20·eol + 10·min_area
```

Plus: final DRC count, outer-iter count, DRT wall, total wirelength, total
vias, peak memory.

## Test set

Day 1 baseline + Patches 1-4: **ISPD-18 test9 8t** only. Add **test10** at
Patch 4+ once scalar policy is stable. Add **test2 + test3** at Patches 8-9
if results promising.

## Reference baselines (to be re-measured)

The previous `redesign_rebase_v3` measurements (4 iters / 16:30 wall) were on
a base that included extensive V2.6.h K-bias work and are NOT applicable to
this branch. The relevant numbers for `outer_loop_plus` are:

  * **Upstream-master (no V2 modifications)** = the target baseline. The
    user-reported earlier `openroad/orfs:latest` docker image gave 7 iters
    / 14m 06s on ISPD-18 test9 8t. The Patch 1 verification run on this
    branch with env var UNSET must reproduce that (within run-to-run noise).
  * **AdaptiveMarkerModel enabled (Patch N)** — measured after each
    Patch's incremental feature, env var SET.

## Falsification + pivot criteria

  * Patch 1: env-UNSET must reproduce ~7 iters / ~14:06 / 0 DRV / WL +
    vias matching the upstream-docker reference. A regression with the
    feature disabled is a stop-the-line bug.
  * Patches 3-7: with feature enabled, must improve weighted marker score
    OR reduce outer iter count OR shrink runtime by a clear margin on at
    least one design without harming another.
  * Disable + stop patch if two consecutive feature-enabled designs show
    regression in weighted score OR runtime > +10% with no benefit.

## Out of scope on this branch

  * Per-iter wall reduction via intra-worker parallelism (structurally
    blocked by OMP-over-workers saturation — documented in commit history
    of `redesign_rebase_v3` L2.c work).
  * `FlexGridGraph` edge-cost rewrites for adaptive heat (Patch 8+ if model
    proves out; not in 1-2 week scope).
  * Conflict-graph-driven route reordering (Patch 12+ deep refactor).
  * ML policy training (out of scope entirely on this branch).
  * Compositing the AdaptiveMarkerModel with the V2.6.h K-bias work —
    that's a future merge of two independent branches once each proves
    out on its own.

## Thread-safety rule

```
FlexDR thread:     owns and mutates AdaptiveMarkerModel
Worker threads:    receive read-only AdaptiveWorkerPolicy snapshot
                   produce AdaptiveWorkerStats
After OMP batch:   FlexDR merges stats into model SERIALLY
```

No atomics. No shared heatmap updates from workers. No nondeterministic
model mutation.
