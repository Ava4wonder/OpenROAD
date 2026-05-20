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
     sweep). **Best candidate: G_drc_strong** (hot drc 1.25, sev drc 1.50,
     marker mul 1.0). On test9 8t: iter-1 wscore −25%, iter-2 wscore −36%
     vs identity policy, wall 10:19 (vs P1 canonical 10:27), no DRC
     regression, no iter-count drop (4-iter test9 tail looks structural).
     **3.2** (in flight) dumps per-marker iter-3 tail rows to test H1
     (one geometric knot → multiple markers).
  4. **Patch 4 (guide relaxation) — BLOCKED** pending test10 validation.
     G already delivers strong intermediate improvement without guide
     relaxation. Guide rewriting is more invasive and would add detour
     cost; justified only if test10 shows a guide-induced central-
     congestion failure mode that DRC-only policy cannot break.
  5. **Patch 5 — REDESIGNED**: original "rule-aware marker increments"
     goal is obsolete (3.1c+d sweep showed marker-cost multiplier is a
     bad actuator). The refined goal: **rule-aware DRC-policy /
     marker-policy separation**. Use marker history to CLASSIFY the
     failure mode, then choose a rule-specific actuator:
       - Short-heavy hotspot:  raise DRC multiplier strongly
       - Cut-spacing hotspot:  raise via-related cost (not generic marker)
       - EOL-heavy hotspot:    directional/stub-aware penalty
       - Metal-spacing hotspot: spacing-aware DRC pressure, layer-specific
     This is more meaningful than blindly making `addMarkerCostPlanar()`
     rule-aware. See "What this means for Patch 5/6" section below.
  6. **Patch 6 — REDESIGNED**: layer-aware actuator (per-layer DRC mul,
     not per-layer marker increment). Same lesson — marker history is
     the sensor, DRC cost is the actuator.
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

## Open experiments queue (post 3.2)

These ablations are designed to lock G in and characterise its
sweet spot before locking defaults:

  - **H_drc_xstrong** — one more DRC-only ladder point at
    `hot_drc=1.50, severe_drc=2.00, marker=1.0`. Diagnostic: is
    G at the sweet spot, or does stronger DRC keep helping without
    via blowup?
  - **G_no_decay_override** — same as G but with
    `severe_decay_override = -1` (disabled). Tests whether G's
    gain comes from DRC mul alone or also from stronger heat
    persistence. Removes a confounder.
  - **Run A / F / G / G_no_decay / (optional H) on test10.** The
    minimum cross-design validation. test9 4-iter tail is
    structural (H1 result pending from Patch 3.2); test10 has
    more iteration headroom and is the design where iter-count
    benefit is plausible. The "iter count drop" piece of the
    original contribution claim depends on this run.

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
