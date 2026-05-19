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

  1. **Patch 1** — Skeleton files + `CMakeLists.txt` integration + disabled
     model field on FlexDR. **No behaviour change** with env var unset; with
     env var set, model is constructed but methods are no-ops.
  2. **Patch 2** — Marker observation + CSV logging (heat decay + hotspot
     detection + per-iter row dump). No routing behaviour change.
  3. **Patch 3** — Worker `AdaptiveWorkerPolicy` snapshot + scalar
     drcCost/markerCost/fixedShapeCost multipliers. Feature-gated.
  4. **Patch 4** — Region-specific guide relaxation (`relax_guide`,
     `setFollowGuide(false)` in hot workers). Feature-gated.
  5. **Patch 5** — Rule-aware marker increments. New overloads on
     `FlexGridGraph::addMarkerCostPlanar/Via` accepting `amount`. Old
     overloads preserved.
  6. **Patch 6** — Layer-aware policy. Compact per-layer multiplier arrays in
     `AdaptiveWorkerPolicy`.
  7. **Patch 7** — Net-score logging + weak queue priority (tie-breaker only;
     no full reorder).

After Patch 7 if results justify: conflict-graph prototype (diagnostic only,
no routing impact yet).

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
