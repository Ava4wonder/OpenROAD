# outer_loop_plus — Patch Exploration Log (2026-05-19 → 2026-05-21)

Canonical record of every AdaptiveMarkerModel / Constraint-Field /
OMP-scheduling exploration on the `outer_loop_plus` branch, tested
on ISPD-18 test9 (8 threads, H100 host, `drt-build-env` container).

## Reference baselines (test9 8t)

| Variant | Iters | Trajectory | WL | Vias | DRT wall |
|---|---:|---|---:|---:|---|
| **UNSET** (pure upstream master) | 4 opt + 1 cleanup | 92,709 → 1,618 → 390 → 3 → 0 | 5,412,412 | 2,284,621 | 10:27 |
| **P3.3 H** ★ (locked default) | 4 opt + 1 cleanup | 92,709 → 1,200 → 243 → 1 → 0 | 5,413,159 | 2,288,021 | **10:19** |

P3.3 H is the locked AdaptiveMarkerModel default: persistent
per-tile heat array drives a 3-tier (severe / hot / identity)
DRC-mul ladder, with `severe_drc_mul = 2.00` / `hot_drc_mul = 1.50`
on top-2% / top-10% heat tiles. Marker mul + decay override are
disabled (P3.2 ablation showed neither contributes).

## Chronology of explorations after P3.3 H locked

### Patch 3.4 K_taper (held, not default) — commit `c047f948d5`

Mechanism: at each `beginOuterIter`, pick DRC mul tier from a
4-step ladder keyed on **previous iter's total marker count**:
`>1000 → H tier (1.5/2.0)`, `200..1000 → G (1.25/1.5)`,
`50..200 → D (1.10/1.25)`, `≤50 → identity (1.0/1.0)`.

Result on test10 (where H bloats outer iter count 17 → 44):
K_taper recovers to 22 iters (vs UNSET 17 / H 44), wall 37:02 vs
UNSET 39 / H 36:34. Cleanup tail cut in half vs H but loses
mid-iter quality.

Held as a variant. Useful on designs with long cleanup tail.

### Patch 3.5 runtime profiling — commit `06fb1a1911` + fix `4ebb274ff3`

Per-iter + per-worker CSV instrumentation gated by
`OPENROAD_DRT_ADAPTIVE_PROFILE=1`. Captures search_repair_wall,
worker wall p50/p95/p99/max, hot/severe/identity worker counts.

Key findings from H_profile on test9 (iter sr_wall, max_worker):
* iter 0 (init): 266s, max 5.73s, **11% imbalance overhead** (sum/8 vs actual)
* iter 1 (opt):  209s, max 5.10s, **17% imbalance overhead**
* iter 2:        36s,  max 1.81s (97% of workers exit immediately — empty drc_box)
* iter 3:        22s,  max **14.0s** ← single pathological worker = 64% of iter wall
* iter 4 clean:  89s,  max **88.5s** ← 1 worker = 100% of iter wall

iter 0 + iter 1 together = 76% of total DRT wall.

### Patch 4 Constraint-Field-Guided Detailed Routing — RAMPED DOWN

Phase 4.1 (`a5574aec12`) skeleton + 4.2 V1 (`d2edfe6a89`) cost
integration + 4.2.b build-seam fix (`aaf29688cb`) + 4.2.c same-net
filter + λ scaling (`c5292da25f`) + 4.3 marker-conditioned +
projection-indexed (`5fbda56f1a`).

P4.2 V1 test9: −2.7% vias but +50% wall + 5→7 iters → broad
via-density penalty, wrong actuator for test9's planar-DRC
bottleneck.

P4.3 V1 test9: convergence-safe (same iters, same wall +0.04%
WL, iter-2 −7% markers). Mechanism works, but gains too small
vs main-track. **Ramped down 2026-05-21**.

### Patch 5 — Iter-0 uniform DRC pressure — NEGATIVE (commit `0af4842ea2`)

Hypothesis: iter 0 wall (40% of DRT) is reachable by blanket DRC
mul at iter 0. Tried mul ∈ {1.25, 1.50, 2.00}.

Result: all 3 variants strictly worse on wall (+11s to +17s).
iter-0 marker count went **UP**, not down (+1.4% / +0.9% / +5.9%).
Diagnosis: blanket pressure without per-region info → forced
detours create more shorts/spacing markers. No sensor at iter 0.

### Patch 6 — Stubborn-marker classification — MARGINAL (commit `9443dc1dae`)

Track nets with markers in consecutive iters; boost their heat
weight by `(1+stubborn_mult)`. Boosts stubborn regions into
severe tier earlier.

Result: best variant `mul=1.0` (×2 weight): iter-2 −5 markers
(−2.1%). Higher boosts (2.0, 4.0) saturate back to H trajectory
— heat-array already classifies most regions as severe. Wall
within noise.

### Patch 7 — Per-worker init-marker DRC mul — MARGINAL (commits `9aa9d3c` + `df906f3aa8` fix)

Per-worker formula: `mul = 1.0 + min(init_markers/N, 1.0) ×
(severe_mul − 1.0)`. Override applied after `init()` runs inside
`worker.main()`.

V1 had a **plumbing bug**: mutated `adaptive_policy_.drc_cost_mul`
but didn't re-call `gridGraph_.setCost()`, so the changed mul
never reached the maze. Routing was bit-identical to H. Bug
caught by smoke test, fixed in P7.b.

Post-fix result: best variant N=50 → iter-1 −33 markers (−2.75%),
but iter-0 +884 (+1.0%), wall +2s. N=20 too aggressive:
iter-0 +2,318 (+2.5%), +11s wall.

### Patch 8 — Rule-aware DRC mul boost — DROPPED (not run)

Same kind of mechanism as P5/P6/P7. P5/P6/P7 pattern was so
consistent (sub-1% wall changes, iter-0 markers up) that P8 was
dropped per user direction without running.

### Pattern from P5/P6/P7

**Every per-iter / per-worker / cross-iter scalar-mul perturbation
produces:**
- sub-1% wall changes
- sign-ambiguous tradeoffs (one iter improves, another regresses)
- iter-0 marker count consistently INCREASES
- outer iter count never moves from 5

**H is at a local optimum on test9** that small scalar-mul tweaks
cannot unlock.

### Patch 9 — OMP scheduling reform — NEGATIVE (commit `1db000626b`)

Theory: P3.5 showed 11-17% imbalance overhead on iters 0+1. Pre-sort
workers heaviest-first → OMP dynamic kicks off heavy work at t=0
→ smaller tail.

Sort key: iter 0 → `queryGuide(routeBox).size()` (no markers
yet); iter ≥ 1 → `queryMarker(drcBox).size()`.

Test result (H + OMP_SORT, default-off env-gated):
* Trajectory: noise (±5 markers)
* WL/vias: noise
* **Total wall 10:29 (vs H 10:19, +10s regression)**

Per-iter profile comparison (H baseline vs H+OMP_SORT):

| Iter | H sr_wall | S sr_wall | Δ | w_max H | w_max S | Δ_max |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 266.3s | 270.3s | +4.1s | 5,730ms | 5,703ms | −27 |
| 1 | 208.6s | 210.0s | +1.4s | 5,095 | 5,116 | +22 |
| 2 | 36.3s | 36.8s | +0.4s | 1,814 | 1,805 | −9 |
| 3 | 21.8s | 22.0s | +0.2s | 14,028 | 13,994 | −34 |
| 4 | 89.2s | 87.3s | −1.9s | 88,465 | 86,589 | −1,876 |

**Honest reflection on what went wrong:**

The implementation is correct (sort runs, workers dispatch in
sorted order, OMP loop unchanged). But the **theory was wrong**:

OMP `schedule(dynamic)` already does **work-stealing** by default.
When a thread finishes a task, it grabs the next-available worker
from the queue. **Heavy workers naturally land on whatever thread
is free first, regardless of queue order.**

The "11-17% imbalance overhead" I measured isn't from poor
scheduling — it's from:
- Single max-worker wall (un-parallelizable hard floor;
  8 threads can't make a 5s worker take less than 5s)
- OMP barrier sync at end of `#pragma omp parallel for`
- Memory bandwidth pressure
- OS scheduling jitter

None of these are addressable by sorting the input queue. The
sort just added serial region-query overhead on the FlexDR thread
(~10s across iters) without changing the wall floor.

**P9 closes negative — the OMP scheduling track is dead.**

## Final state of explored levers (test9)

| Lever | Mechanism | Test9 verdict |
|---|---|---|
| **P3.3 H** ★ | Per-tile heat → severe/hot DRC mul ladder | **Locked default** |
| P3.4 K_taper | Mul tier by previous-iter marker count | Held for design-specific use |
| P4 / 4.3 | Constraint-field guidance | Ramped down (small gains) |
| P5 | Blanket iter-0 DRC pressure | NEGATIVE |
| P6 | Stubborn-marker weight boost | MARGINAL (−2.1% iter-2) |
| P7 | Per-worker init-marker DRC mul | MARGINAL (−2.75% iter-1 / +1% iter-0) |
| P8 | Rule-aware DRC mul boost | DROPPED (predictable to be marginal) |
| P9 | OMP scheduling reform | NEGATIVE (theory wrong) |

## What this branch ships

All code is on `outer_loop_plus` HEAD just before the 2026-05-21 rollback:
* P3.3 H defaults are active by default (when `OPENROAD_DRT_ADAPTIVE_MARKER=1`)
* All other patches default-OFF behind env vars
* When all env vars unset → bit-identical to upstream master
* When `OPENROAD_DRT_ADAPTIVE_MARKER=1` → P3.3 H baseline

Env-var matrix:
```
OPENROAD_DRT_ADAPTIVE_MARKER=1               enable H baseline
OPENROAD_DRT_ADAPTIVE_K_TAPER=1              P3.4 mul taper
OPENROAD_DRT_ADAPTIVE_PROFILE=1              P3.5 CSV emission
OPENROAD_DRT_CONSTRAINT_FIELD=1              P4 field guidance
OPENROAD_DRT_ADAPTIVE_ITER0_DRC_MUL=<float>  P5
OPENROAD_DRT_ADAPTIVE_STUBBORN_MULT=<float>  P6
OPENROAD_DRT_ADAPTIVE_PER_WORKER_NORMALIZER=<int>  P7
OPENROAD_DRT_ADAPTIVE_OMP_SORT=1             P9
```

## Why test9 is at a ceiling

P3.5 profiling showed the actual runtime bottlenecks on test9:
1. **Initial routing (iter 0) is 40% of wall** with no marker
   sensor available — can't be improved by heat-based policy
2. **Single max-worker walls** (5-14s on iters 1-3) are the
   wall floor; OMP parallelism can't help with single tasks
3. **Cleanup iter 4** has 2 workers, one takes 88s — pure
   single-thread serial work
4. The H policy is already saturating the available improvement
   from per-region DRC pressure

To move past the test9 ceiling requires one of:
- A different design (test10 has 16+ iters → more headroom;
  test2 already converges fast)
- A fundamentally different mechanism (intra-worker
  parallelism, sub-task decomposition, cross-branch merge
  with V2.6.h K-bias, ML-trained policy)
- Not pursuing test9 wall further

## Archive

Per user direction 2026-05-21: this exploration log preserved as
a tagged commit `archive/post-p9-2026-05-21`. The `outer_loop_plus`
branch HEAD is reset to commit `8951e1c99d` (P3.3 H) as the
clean starting point for new designs.
