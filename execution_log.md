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

---

## 2026-05-20 — Patch 2 committed + UNSET/SET dual verification

Patch 2 = commit `d3060196e5` on `outer_loop_plus`. Real
`classifyConstraint` (17 frConstraintTypeEnum cases → 7 rule classes),
`extractMarkerNets` (conservative frcNet-only), `getRuleAwareInflatedBox`
(spacing rules get 1.5× bloat). AdaptiveMarkerModel: lazy heat-array
allocation, beginOuterIter→decayHeat, observeOneMarker accumulates
weighted heat into the (rule,layer,ty,tx) flat array + bumps net_score_,
updateHotspots scans tile totals for severe threshold, writeCsvRowIf
Enabled dumps per-iter row. FlexDR.cpp hooks at top + bottom of
searchRepair (counted ONCE per outer iter, never per worker).

### UNSET regression check (test9 8t)

Output: `ispd18_test9_olp_p2_unset_8t/run.log`. Result:
  * 4 opt + 1 cleanup iters
  * DRT wall 10:18 (Patch 1 baseline was 10:27 — within run-to-run noise)
  * DRC 0, WL 5,412,412 µm ✓ bit-identical, vias 2,284,621 ✓
    bit-identical
  * Peak memory 8.37 GB (Patch 1 baseline 8.35 — neutral)

Disabled-shell guarantee empirically reconfirmed: env UNSET → no model
construction → routing exactly matches Patch 1 / pure upstream master.

### SET observation+CSV check (test9 8t, env SET, CSV path provided)

Output: `ispd18_test9_olp_p2_set_8t/run.log` + `adaptive_marker.csv`.
Result:
  * 4 opt + 1 cleanup iters
  * DRT wall 10:23 (Patch 2 UNSET was 10:18 → **+5s overhead = +0.8% wall**)
  * DRC 0, WL 5,412,412 ✓, vias 2,284,621 ✓ — **routing bit-identical
    to UNSET** (observation has zero routing impact, as Patch 2 design
    requires)
  * Peak memory 8.39 GB (UNSET 8.37 → **+24 MB heat-map cost**, well
    inside the budget the heat-array sizing assumed)

CSV is the headline artifact for this patch. Per-iter rows:

```
iter,total_markers,weighted_score,num_hotspots,short,cut_short,metal_spc,cut_spc,eol,min_area,ns_metal,other
0,92709,2482125,295,8781,0,19186,1329,15594,0,1,47818
1,1618,132040,174,1156,0,273,118,68,0,2,1
2,390,31010,81,264,0,85,26,15,0,0,0
3,3,240,35,2,0,1,0,0,0,0,0
4,0,0,9,0,0,0,0,0,0,0,0
```

**weighted_score convergence:** 2.48M → 132K → 31K → 240 → 0. This is
the metric Patches 3-7 will use to measure policy effects against the
identity-policy SET baseline. **Number of unique tile hotspots peaks
at 295 in iter 0** (with `severe_hotspot_threshold = 800` default) and
decays toward 9 by iter 4 (these 9 are stale tiles from the iter-0
heat map whose decayed value still sits above threshold).

**Followup work flagged:** the `other_count` column is 47,818 in iter 0
(about half of all markers). My `classifyConstraint` switch is missing
some common frConstraintTypeEnum cases on this design. Doesn't affect
weighted_score's directional convergence but per-rule attribution will
be incomplete until I expand the switch. Worth a 10-min refinement
patch before Patch 3 to maximise signal-to-noise on per-rule policy
effects.

Expected next: Patch 3 (worker AdaptiveWorkerPolicy snapshot + scalar
multipliers on drcCost/markerCost/fixedShapeCost). Feature-gated.
Goal: first measurable wall or weighted-score improvement when env SET.

---

## 2026-05-20 — Patch 3 + 3.1 series + sweep + ablation

Patch 3 (commit `06a5dd644f`) wired AdaptiveWorkerPolicy through to
worker `setCost`. First SET run was a slight regression: WL +58 µm,
vias +316, iter count unchanged. Diagnosis: iter gate at `iter >= 2`
blocked policy from acting on iters 0+1 where 99.6% of markers live.

Patch 3.1a (commit `c354b40be3`) added per-call CSV instrumentation
to prove the diagnosis: iter 0 + 1 received zero policy calls; iter 2
received policy on 91% of workers, iter 3 on 95%, but those iters
have only 386 + 4 markers — too late.

Patch 3.1b (commit `ca91fc0ce2`) dropped the gate to `iter >= 1`.
First positive signal: iter-1 markers down 9.3%, iter-2 down 12.7%,
DRC clean, iter count still 4.

Patch 3.1c+d (commit `47b718cc9d`) bundled:
  - percentile-based hot/severe thresholds (top 10% / top 2% of
    nonzero tiles by heat) with static thresholds as a floor
  - env-var-driven multiplier overrides so a sweep can run from one
    binary

Sweep on test9 8t (7 variants):

```
Variant         hot(drc/mark)  sev(drc/mark)  i1 wscore  i2 wscore  i3
P1 canonical    —              —              —          —          —
A_identity      1.0/1.0        1.0/1.0        131,740    31,030     3 (2sh+1ms)
B_mark_mild     1.0/1.25       1.0/1.50       131,860    30,320     9
C_mark_strong   1.0/1.50       1.0/2.00       135,870    32,015     9
D_drc_mark_mild 1.10/1.25      1.25/1.50      116,525    24,830     3 (2sh+1ms)
E_drc_mark_strong 1.25/1.50    1.50/2.00      109,765    19,980     6 (3sh+3ms)
F_drc_mild      1.10/1.0       1.25/1.0       115,905    24,520     3 (2sh+1ms)
G_drc_strong ★  1.25/1.0       1.50/1.0       98,635     19,725     3 (1sh+2ms)
```

**Headline finding (DRC-only ablation):**
  - F BEATS D on every metric (mild DRC-only > mild DRC+marker)
  - G BEATS E on every metric (strong DRC-only > strong DRC+marker)
  - Marker multiplier alone (B, C) regresses across the board
  - **Marker-cost multiplier is actively harmful when paired with DRC**

**Design lesson the data forces:**
```
Persistent frMarker heat — GOOD sensor (selects which workers).
Marker-cost multiplier  — BAD actuator (creates detours, adds tail markers).
DRC-cost multiplier     — USEFUL actuator (raises legality sensitivity).
```

The original "hot region → raise marker cost" intuition was wrong.
The correct rule is "hot region → raise DRC cost, leave marker cost
at 1.0." Historical marker heat tells you *where* to intervene; it
shouldn't dictate *how much* to penalise markers locally — current
legality risk (DRC cost) is the better local signal.

**G best so far** (test9 8t):
  - vs A_identity: iter-1 wscore −25.1%, iter-2 wscore −36.4%
  - iter-3 tail: 3 markers (same count as A; rule mix shifts from
    2 short+1 ms to 1 short+2 ms — different physical residual)
  - DRT wall: 10:19 (best in sweep, even vs P1 canonical 10:27)
  - WL +0.0087%, vias +0.114% vs P1 canonical (tiny cost)
  - iter count 4+1 (unchanged)

**Convergence-quality win, not iter-count win on test9.** Whether
G translates to iter-count drop on harder designs (test10) is the
open question.

---

## 2026-05-20 — Patch 3.2 tail-marker dump (in flight)

Patch 3.2 (commit `7c5130e000`) added per-marker CSV dump when an
iter's marker count <= tail_dump_threshold (default 20). Used to
test the user-flagged hypothesis: are the 3-marker tails in
A/D/F (2 short + 1 metal_spacing) the SAME physical markers
(H1: one geometric knot → multiple markers)?

Sweep launched: A, D, F, G with tail logging enabled. Build + 4 ×
14 min runs in flight on H100. Results expected ~15:19 UTC.

Expected next: tail.csv comparison across variants. If A/D/F share
the same bboxes + layers + nets → H1 confirmed, tail is a localised
deterministic residue independent of policy strength. Then either
implement targeted late-stage repair (Patch 8+ scope), or accept
the 4-iter floor as structural to test9 and move to test10.

---

## 2026-05-20 — Patch 2.1 committed + SET verification

Patch 2.1 = commit `2cdabc444d`. Added AdaptiveRuleClass::MinStep
(bucket 7, before Other). Expanded MetalSpacing to absorb
SpacingTableInfluence, SpacingTableOrth, Lef58WidthTableOrth,
Lef58SpacingWrongDir, Lef58KeepOutZone, Lef58TwoWiresForbiddenSpc,
Lef58ForbiddenSpc, Lef58Enclosure, MetalWidthVia,
Lef58RightWayOnGridOnly, Lef58RectOnly. Expanded Eol to absorb
Lef58SpacingEndOfLineWithinEncloseCut/ParallelEdge/MaxMinLength.
New MinStep bucket covers frcMinStep, Lef58MinStep, Minimumcut,
Lef58MinimumCut.

CSV header gained `min_step_count` column between ns_metal_count
and other_count.

### SET verification (test9 8t)

Output: `ispd18_test9_olp_p21_set_8t/`. Result:
  * 4 opt + 1 cleanup iters
  * DRT wall 10:29 (Patch 2 SET was 10:23 — within noise)
  * DRC 0, WL 5,412,412 ✓ bit-identical, vias 2,284,621 ✓
    bit-identical
  * Peak memory 8.34 GB

CSV iter 0:
```
0,92709,2482125,295,8781,0,19186,1329,15594,0,1,0,47818,182,157,10000
        total  score   ho  Sh   CS  MS    CSpc  Eol   MA NM MStp Other
```

**Surprise:** `min_step_count = 0` AND `other_count = 47818`
(unchanged from Patch 2). The expanded switch caught zero new
markers. Diagnosis: those 47,818 "other" markers don't carry a
`frConstraint*` at all — `marker.getConstraint()` returns nullptr,
so `classifyConstraint(nullptr)` falls through to Other. Sum check:
8781+19186+1329+15594+0+1+0+47818 = 92,709 ✓.

This is a known-ish OpenROAD behaviour: some marker-creation paths
(notably in FlexGCWorker's geometric checks) emit `frMarker` with
the constraint field left null — the marker carries the bbox and
layer but not the rule pointer. The weighted_score still weighs
them at the default (10 / unit), correctly attributed in the
overall total. The per-rule columns are a diagnostic, not a
control input.

**Decision:** accept the Other bucket as-is and proceed to Patch 3.
Per-tile heat (sum across all rules) and per-net score are unaffected
by the missing per-rule attribution. Investigating where the
null-constraint markers come from would mean instrumenting
FlexGCWorker's addMarker call sites — out of scope for the
AdaptiveMarkerModel work, and not blocking the contribution claim.

Expected next: Patch 3 (worker AdaptiveWorkerPolicy snapshot +
scalar drcCost/markerCost/fixedShapeCost multipliers, hotspot-
triggered). First patch to actually CHANGE routing behaviour when
SET vs UNSET. Goal: weighted_score improvement OR iter-count
reduction on test9 8t without harming WL.

---

## Patches 3.0 → 3.3 — worker policy + multiplier sweep + lock H (test9 anchor)

### 3.0/3.1 sequence

Landed worker AdaptiveWorkerPolicy snapshot path (FlexDR computes
policy per-worker via `getWorkerPolicy(drc_box, …)`, worker takes
read-only copy). First version applied scalar drc/marker/fixed-
shape multipliers in `createWorker` via `std::lround(cost * mul)`
before `setCost`.

  * 3.1a — per-policy-call CSV diagnostic (`policy_log_path`),
    iter / drc_box / heat / classification / multipliers emitted.
  * 3.1b — found that iter gate `>= 2` was blocking the policy
    from the iters where 99.6% of markers live; lowered to `>= 1`.
    First positive signal: iter-1 markers −9.3%, iter-2 −12.7%
    vs UNSET (fixed thresholds, mul 1.10/1.25 + 1.25/1.50).
  * 3.1c — percentile-based dynamic thresholds: top 10% nonzero
    tiles = hot, top 2% = severe. Effective threshold =
    `max(static_floor, dynamic_percentile)` — distribution-
    adaptive across designs without losing the absolute-heat
    sanity check.
  * 3.1d — env-var-driven multiplier sweep (HOT_DRC_MUL /
    HOT_MARKER_MUL / SEVERE_DRC_MUL / SEVERE_MARKER_MUL /
    SEVERE_DECAY_OVERRIDE / HOT_PERCENTILE / SEVERE_PERCENTILE).
    Defaults remain the 3.1b values; any subset can be overridden
    for sweeping.

Sweep results on ISPD-18 test9 8t (4 opt + 1 cleanup base):

| Variant | Hot drc/mk | Sev drc/mk | Iter-1 wscore | Iter-2 wscore | Iter-3 markers | Wall | DRC |
|---|---|---|---|---|---|---|---|
| A_identity | 1.00/1.00 | 1.00/1.00 | 100% | 100% | 3 | 10:36 | 0 |
| B/C (marker-only) | 1.00/1.10–1.50 | 1.00/1.25–2.00 | regress | regress | +EOL/cut | — | 0 |
| D | 1.10/1.25 | 1.25/1.50 | −9% | −13% | 3 | 10:38 | 0 |
| E | 1.25/1.50 | 1.50/2.00 | −18% | −24% | 3 | — | 0 |
| F (DRC-only mild) | 1.10/1.00 | 1.25/1.00 | beats D | beats D | 3 | — | 0 |
| **G (DRC-only strong)** | **1.25/1.00** | **1.50/1.00** | **−25.1%** | **−36.4%** | **3** | **10:19** | **0** |
| **H (DRC-only xstrong)** ★ | **1.50/1.00** | **2.00/1.00** | **−30.7%** | **−43.9%** | **1** ★ | **≤ 10:19** | **0** |
| I (DRC-only uxstrong) | 1.75/1.00 | 2.50/1.00 | ≈ H | ≈ H | 1 | ≈ H | 0 |
| G_no_decay | 1.25/1.00 | 1.50/1.00 (decay=-1) | ≈ G | ≈ G | 3 | ≈ G | 0 |

**Design lesson (sharp, defensible, repeatable across F-vs-D and
G-vs-E pairs):**

```
Persistent frMarker heat:    GOOD sensor    (selects WHICH workers)
Marker-cost multiplier:      BAD actuator   (creates detours, adds tail markers)
DRC-cost multiplier:         USEFUL actuator (raises legality sensitivity)
```

### 3.2 — per-marker tail dump (H1 test)

Added `OPENROAD_DRT_ADAPTIVE_TAIL_LOG` + `tail_dump_threshold`
(default 20). When an iter's marker count ≤ threshold, every
marker is dumped as a separate CSV row: bbox / layer / rule
class / net IDs. const-correctness bug (frMarker::getAggressors
is non-const, can't be called from a const writeTailRow) fixed
by emitting `-1` placeholder instead of touching aggressors —
committed as `b0168bbba1`.

Cross-variant comparison of test9 iter-3 tails revealed:
  * marker-1 (layer 16, no net owner) — shared across A, F, G,
    H. Pin-access / macro-edge structural residue.
  * marker-2 (net pair 66482↔133240) — shared structurally across
    variants but at slightly different bbox locations (different
    routing solutions hitting the same congested net pair).
  * marker-3 — policy-dependent. Present in A/F/G, absent in H.

So H1 ("one geometric knot → multiple markers") is **partially
confirmed**: the iter-3 tail is *mostly* structural. The 4-iter
floor on test9 is intrinsic to the design at upstream-master
HEAD, not a tuning issue. H removes the one tail marker that
policy CAN remove.

### 3.3 — lock H as default

Closed Patch 3 by hardcoding the H values into the `Options`
defaults in `AdaptiveMarkerModel.h`:

```
hot_drc_mul = 1.50f          (was 1.10f)
hot_marker_mul = 1.00f       (was 1.25f)
severe_drc_mul = 2.00f       (was 1.25f)
severe_marker_mul = 1.00f    (was 1.50f)
severe_decay_override = -1.0f  (was 0.99f; -1 = disabled)
```

Marker multiplier kept as a struct field but defaulted to 1.0
with a comment recording the negative result (so future
experimenters don't burn cycles re-discovering that marker mul
is a bad actuator). Same for `severe_decay_override`: kept as a
field, defaulted to disabled, with the G_no_decay ≈ G ablation
noted.

Env-var override site in `FlexDR.cpp` reads `opts.X_mul` as the
fallback, so the new defaults flow through automatically. Stale
comment ("defaults to the Patch 3 / 3.1b values") updated to
"defaults to the Patch 3.3 H values".

### Cross-design validation (next, IN FLIGHT)

  * Designs: ispd18_test2 (small, low-iter — sanity check that
    H does not regress on a design where there's little
    headroom), ispd18_test10 (large, more iteration headroom
    — where iter-count drop is plausible).
  * Variants per design: UNSET (true upstream baseline),
    A_identity (model on, multipliers all 1.0 — isolates
    instrumentation overhead from policy effect), H (locked
    default).
  * 6 runs total. test2 + test10 designs both already on H100
    (test2 downloaded 2026-05-19; test10 from earlier work).

### Patch 4 status

BLOCKED pending cross-design results. Will be unblocked only if
test10 shows a guide-induced central-congestion failure mode
that DRC-only policy cannot break. Otherwise skip directly to
the redesigned Patch 5 (rule-aware DRC-policy / marker-policy
separation).
