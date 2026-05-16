# grid_state_access — Plan

Branch base: `master @ 9a00dc952f`. Created 2026-05-16.
Owner: ava.

## 1. Objective

CPU-only acceleration of the OpenROAD drt maze routing search loop. Replace
the current object-heavy A*/maze-routing frontier with a more cache-friendly,
vector-oriented design:

> Convert maze routing state into flat Structure-of-Arrays grid-state arrays,
> and replace the `std::priority_queue`-based open list with a
> bucketed/vector frontier.

Current implementation: each net is routed by preparing source and destination
access points, then repeatedly calling `gridGraph_.search(...)` from
`FlexDRWorker::routeNet()`. Inside `FlexGridGraph::search()`, the router
initializes a wavefront, pushes source points, repeatedly pops the best
wavefront state, checks destination reachability, and expands neighbors. The
wavefront is backed by `std::priority_queue<FlexWavefrontGrid>`, where
`FlexWavefrontGrid` stores many per-state fields (coordinates, path cost,
estimated cost, via/turn history, traceback buffer).

This plan keeps the routing algorithm semantically close to existing
behaviour but changes data layout and frontier management to improve cache
locality, reduce heap overhead, reduce allocation/object movement, and
enable batch expansion on CPU.

## 2. Motivation

The existing maze router uses a priority queue of `FlexWavefrontGrid`
objects. Clean but CPU-inefficient for large searches:

- `std::priority_queue` incurs O(log N) push/pop cost.
- Each queue element is a relatively large object.
- Heap operations produce irregular memory access.
- AoS-style wavefront state is not friendly to cache prefetching or SIMD.
- Closed/visited state is partially stored through grid-graph metadata such
  as previous A* node direction, which makes bulk reset and repeated net
  routing less efficient.

Target: refactor maze search around flat indexed arrays.

## 3. Scope

CPU-only. No CUDA, no GPU kernels, no heterogeneous routing.

**In scope**
- Replace object-based wavefront storage with flat arrays.
- Replace `std::priority_queue` with bucketed/vector frontier.
- Preserve existing routing cost semantics as much as possible.
- Preserve traceback behaviour.
- Preserve deterministic or near-deterministic tie-breaking.
- Validate against existing OpenROAD detailed routing regression tests.

**Out of scope**
- GPU acceleration.
- Major routing algorithm replacement.
- Global routing changes.
- DRC rule simplification.
- Net scheduling redesign.

## 4. Proposed Design

### 4.1 Flatten the 3D Maze Grid

```
node_id = (z * yDim + y) * xDim + x;
```

Per-node data in dense vectors:

```cpp
std::vector<frCost>   g_cost;
std::vector<frCost>   f_cost;
std::vector<uint8_t>  node_state;     // unseen/open/closed
std::vector<uint8_t>  parent_dir;
std::vector<uint8_t>  last_dir;
std::vector<frCoord>  vlength_x;
std::vector<frCoord>  vlength_y;
std::vector<frCoord>  tlength;
std::vector<uint8_t>  prev_via_up;
std::vector<uint32_t> visit_epoch;
```

### 4.2 Epoch-Based Reset

```
current_epoch++;
if (visit_epoch[node] != current_epoch) {
    visit_epoch[node] = current_epoch;
    g_cost[node] = INF;
    node_state[node] = UNSEEN;
}
```

No full-array zeroing between searches.

### 4.3 Replace `std::priority_queue` with Bucketed Frontier

```
std::vector<std::vector<NodeId>> buckets;
frCost current_min_key;

bucket_id = key / bucket_width;
buckets[bucket_id].push_back(node);
```

Process current non-empty bucket as a vector batch.

Backends, in order of complexity:
1. exact integer buckets (if cost range bounded)
2. circular bucket queue
3. radix heap
4. two-level bucket queue
5. bucketed priority queue with overflow heap

Priority key should approximate the current `FlexWavefrontGrid::operator<`:
cost first, then distance to center, layer preference, path cost, x/y,
last direction, via direction, length fields.

### 4.4 Preserve Existing Cost Model

`getNextPathCost()` includes bend, via-spacing, via-turn, DRC, marker, fixed-
shape, block, guide, jumper, NDR-related terms. Refactor storage and frontier
selection only; reuse existing cost calculation:

```
next_g = getNextPathCost(curr_state, dir, route_with_jumpers);
next_h = getEstCost(next_node, dst_box, dir);
next_f = next_g + next_h;
```

## 5. Implementation Phases

### Phase 1: Flat Node Indexing Utilities

```cpp
NodeId getNodeId(x, y, z);
void getXYZ(NodeId id, x, y, z);
NodeId getNeighbor(NodeId id, dir);
```

Keep `FlexMazeIdx` for compatibility; allow fast flat indexing internally.

**Deliverable:** flat-index utilities, unit tests for index conversion and
neighbor lookup, no behaviour change.

### Phase 2: MazeSearchStateSoA

```cpp
class MazeSearchStateSoA {
 public:
   std::vector<frCost>   g_cost;
   std::vector<frCost>   f_cost;
   std::vector<uint8_t>  state;
   std::vector<uint8_t>  parent_dir;
   std::vector<uint8_t>  last_dir;
   std::vector<frCoord>  vlength_x;
   std::vector<frCoord>  vlength_y;
   std::vector<frCoord>  tlength;
   std::vector<uint8_t>  prev_via_up;
   std::vector<uint32_t> epoch;
};
```

**Deliverable:** state allocated once per worker/grid graph; per-search
logical reset via epoch; compatibility bridge from `FlexWavefrontGrid` to
SoA fields.

### Phase 3: BucketedFrontier

```cpp
class BucketedFrontier {
 public:
   void clear_for_epoch();
   void push(NodeId node, frCost key);
   bool empty() const;
   std::vector<NodeId>& pop_min_bucket();
};
```

Initial bucket_width = 1 (preserve A*-like ordering).

**Deliverable:** bucketed frontier backend; compile-time and runtime switch
between existing priority queue and new frontier; debug-mode pop-order and
path-result comparison vs old.

### Phase 4: Refactor `FlexGridGraph::search()`

```cpp
while (!frontier.empty()) {
    auto& batch = frontier.pop_min_bucket();
    for (NodeId curr : batch) {
        if (state[curr] == CLOSED) continue;
        state[curr] = CLOSED;
        if (isDst(curr)) { traceBackPathSoA(curr, path); return true; }
        for (dir : dirs) {
            if (!isExpandableSoA(curr, dir)) continue;
            NodeId next = getNeighbor(curr, dir);
            frCost new_g = computeNextPathCostSoA(curr, dir);
            if (new_g < g_cost[next]) {
                update_state(next, curr, dir, new_g);
                frontier.push(next, new_g + h_cost(next));
            }
        }
    }
}
```

**Deliverable:** new search path behind feature flag; existing path preserved
for rollback; same route legality as baseline.

### Phase 5: Batch-Oriented CPU Optimizations

Once correctness established:
- process one bucket as a vector batch
- precompute neighbor IDs
- compact edge blocked/cost flags
- reduce `isExpandable` branch mispredict
- touched-node lists for partial reset
- optional intra-bucket parallelism (OpenMP/TBB) after single-thread
  determinism validated

**Deliverable:** performance-tuned CPU search; metrics for heap ops removed,
nodes expanded/sec, cache miss rate, total drt runtime.

## 6. Validation Plan

Compare old and new on:
- routing success/failure count
- DRC violations
- wirelength
- via count
- runtime
- determinism across repeated runs
- path equivalence on small testcases
- full OpenROAD regression suite

First milestone: identical/near-identical QoR (not max speed). Then tune
bucket width and batching.

## 7. Expected Benefits

- lower frontier push/pop overhead
- better cache locality
- fewer temporary object copies
- faster repeated search initialization
- easier profiling and future vectorization
- cleaner path to parallel bucket expansion

The router remains an A*/maze router; the search engine becomes
data-oriented rather than container-oriented.

## 8. Main Risks

OpenROAD drt cost is **history-dependent**: cost depends on current
(x, y, z), last direction, via history, turn length, NDR taper state,
traceback buffer. A naïve one-label-per-node shortest path may prune valid
or better states too aggressively.

**Known sharp edge** (from MEMORY.md `project_routenet_kbias_invariant`):
recursive routeNet from inside K-bias hook **must** explicitly call
`gridGraph_.resetStatus()`; `mazeNetInit` is invoked by the caller, not by
`routeNet` itself. The SoA epoch-reset path must preserve this invariant —
specifically, the epoch counter bump that conceptually clears the SoA arrays
must happen at the same call sites where `resetStatus()` is called today.

Mitigation:
- initially preserve the existing state fields
- match current tie-breaking as much as possible
- keep fallback to old priority queue (compile flag + runtime env)
- consider best-K labels per node if one-state-per-node causes QoR
  regression

## 9. Final Target

A CPU-only, drop-in maze-search backend that replaces the current object-
based priority queue wavefront with flat SoA state arrays and a
bucketed/vector frontier, improving memory locality and reducing
frontier-management overhead while preserving existing detailed-routing
cost semantics.
