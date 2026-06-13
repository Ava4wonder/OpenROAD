# DRT Thread-Scaling — Hybrid + STUBFIX reproduction

Branch `F1_continuous_flow`. Two env-gated mechanisms; **both default OFF**, so
with no env vars the binary is bit-identical to upstream FlexDR.

| env var | effect |
|---|---|
| `OPENROAD_DRT_TS_BATCHMODE=hybrid` | iteration 0 routed as one concurrent batch with per-net commit ownership; cleanup iters use color-merged batches. |
| `OPENROAD_DRT_TS_STUBFIX=1` | stub/guide tile shaping: min 6-GCell tile side, split >8:1 aspect into <=12-GCell segments. Also helps stock upstream. |

Key commits: `a2e1c8bb3d` hybrid mode, `4aef1fd252` STUBFIX,
plus the `hybrid => color` cleanup hunk committed with this doc.

## Build
```
docker exec drt-build-env bash -c "cd /work/baseline/build-container && cmake --build . --target openroad -j 16"
```

## Reproduce — ISPD (test9 / test3 / test10), 64 threads
`run_test_64t.tcl` = `run_test.tcl` with `set_thread_count 64`.
```
# baseline
docker exec -e ISPD_DESIGN=ispd18_test9 \
  -e ISPD_BENCH_DIR=/work/orfs/flow/ispd_bench/tests/ispd18_test9 -e ISPD_OUT_DIR=/tmp \
  drt-build-env timeout 3600 /work/baseline/build-container/bin/openroad \
  -no_init /work/orfs/flow/ispd_bench/run_test_64t.tcl
# candidate
docker exec -e ISPD_DESIGN=ispd18_test9 \
  -e ISPD_BENCH_DIR=/work/orfs/flow/ispd_bench/tests/ispd18_test9 -e ISPD_OUT_DIR=/tmp \
  -e OPENROAD_DRT_TS_BATCHMODE=hybrid -e OPENROAD_DRT_TS_STUBFIX=1 \
  drt-build-env timeout 3600 /work/baseline/build-container/bin/openroad \
  -no_init /work/orfs/flow/ispd_bench/run_test_64t.tcl
```

## Reproduce — asap7 ORFS (ibex / swerv_wrapper)
```
docker exec [-e OPENROAD_DRT_TS_BATCHMODE=hybrid -e OPENROAD_DRT_TS_STUBFIX=1] \
  -e OPENROAD_EXE=/work/baseline/build-container/bin/openroad -e NUM_CORES=64 \
  drt-build-env bash -c "cd /work/orfs/flow && \
  make DESIGN_CONFIG=designs/asap7/ibex/config.mk \
  OPENROAD_EXE=/work/baseline/build-container/bin/openroad NUM_CORES=64 do-5_2_route"
```

## Measured results (64t, all converge to 0 DRVs, UNSET-vs-UNSET cost model)
| design | UNSET | hybrid+STUBFIX | speedup |
|---|---:|---:|---:|
| test9  | 229 s | 150 s  | 1.53x |
| test3 (macros) | 1605 s | 1077 s | 1.49x |
| swerv_wrapper (asap7) | 788 s | 668 s | 1.18x |
| test10 | 1553 s | 1324 s | 1.17x |
| ibex (asap7) | 154 s | 138 s | 1.12x |

8t: ibex 448->396 (1.13x), swerv 3189->3063 (1.04x). STUBFIX alone on stock
upstream: test9 8t 622->556 (1.12x).

NOTE: harness files (`run_test*.tcl`, ispd_bench, asap7 configs) live in the
companion `OpenROAD-flow-scripts` checkout under `/work/orfs`.
