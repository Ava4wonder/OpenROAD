#!/bin/bash
# apply_v4.sh — v4 instrumentation: fine-grained bookkeeping breakdown.
#
# Adds 12 new ProfileTask scopes to attribute the ~63.8% "exclusive bookkeeping"
# residual on swerv (T_init_route_excl + T_repair_excl). Apply AFTER
# apply.sh + apply_v2.sh + apply_v3.sh.
#
# Sites added (call frequency on swerv noted; ~1 µs per ProfileTask scope):
#   1. DRW:init_worker          init() body                  ~49k
#   2. DRW:worker_teardown      cleanup() body               ~49k
#   3. DRW:init_maze_cost       initMazeCost() body          ~49k
#   4. DRW:init_route_queue_entry  route_queue_init_queue()  ~294k (~6 iters × 49k)
#   5. DRW:maze_grid_init       FlexGridGraph::init() body   ~49k
#   6. DRW:maze_grid_reset      FlexGridGraph::resetStatus() ~7M
#   7. DRW:marker_remove        endRemoveMarkers() body      ~294k
#   8. DRW:net_ordering         sortRerouteNets() body       ~294k
#   9. DRW:net_ordering_q       sortRerouteQueue() body      ~294k
#  10. DRW:marker_insert        frRegionQuery::addMarker     ~30M
#  11. DRW:marker_insert_gc     FlexGC Impl::addMarker        ~30M
#  12. DRW:ripup                inline block in route_queue_main ~7M
#
# Sites NOT wrapped (would corrupt timing — too high frequency):
#   - history_cost_read (getNextPathCost / getEstCost): billions of calls per
#     stage; ProfileTask overhead would dominate. Stays bundled inside
#     DRW:maze_search.
#
# Sites NOT wrapped (inline operations not separable from parent function):
#   - object construction (drNet allocs, RouteQueueEntry construction etc.):
#     would require splitting function bodies. Stays in residual.
#   - route_queue_pop (single inline std::queue::pop call): trivial; stays
#     bundled.

set -euo pipefail

OPENROAD_ROOT="${1:-$HOME/openroad-profile/OpenROAD}"
cd "$OPENROAD_ROOT"

ensure_include() {
  local f="$1"
  if ! grep -q 'frProfileTask.h' "$f"; then
    python3 - "$f" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if 'frProfileTask.h' in s:
    sys.exit(0)
m = list(re.finditer(r'^#include\s+"[^"]+"\s*$', s, flags=re.M))
if m:
    last = m[-1]
    s = s[:last.end()] + '\n#include "frProfileTask.h"' + s[last.end():]
else:
    idx = s.find('\n\n')
    s = s[:idx] + '\n#include "frProfileTask.h"' + s[idx:]
p.write_text(s)
PY
  fi
}

wrap_fn_body() {
  # $1=file, $2=signature regex up to opening paren contents, $3=task name
  python3 - "$1" "$2" "$3" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
pat_src = sys.argv[2]
name = sys.argv[3]
s = p.read_text()
marker = f'"{name}"'
if marker in s:
    sys.exit(0)
pat = re.compile(pat_src + r'\s*\{\s*\n', flags=re.M | re.DOTALL)
m = pat.search(s)
if not m:
    print(f'FATAL: not found: {name} in {p}', file=sys.stderr)
    sys.exit(5)
s2 = s[:m.end()] + f'  ProfileTask _p_v4("{name}");\n' + s[m.end():]
p.write_text(s2)
PY
  echo "  ✓ $3"
}

# ----- 1, 2, 3, 4 in FlexDR_init.cpp ----------------------------------------
F=src/drt/src/dr/FlexDR_init.cpp
ensure_include "$F"
wrap_fn_body "$F" 'void FlexDRWorker::init\(const frDesign\* design\)' 'DRW:init_worker'
wrap_fn_body "$F" 'void FlexDRWorker::initMazeCost\(const frDesign\* design\)' 'DRW:init_maze_cost'
wrap_fn_body "$F" 'void FlexDRWorker::route_queue_init_queue\(\s*std::queue<RouteQueueEntry>&\s*[a-zA-Z_]+\s*\)' 'DRW:init_route_queue_entry'

# ----- 5, 6 in FlexGridGraph.cpp --------------------------------------------
F=src/drt/src/dr/FlexGridGraph.cpp
ensure_include "$F"
wrap_fn_body "$F" 'void FlexGridGraph::init\(const frDesign\*\s*design,\s*const odb::Rect&\s*routeBBox,\s*const odb::Rect&\s*extBBox,\s*frLayerCoordTrackPatternMap&\s*xMap,\s*frLayerCoordTrackPatternMap&\s*yMap,\s*bool\s*initDR,\s*bool\s*followGuide\)' 'DRW:maze_grid_init'
wrap_fn_body "$F" 'void FlexGridGraph::resetStatus\(\)' 'DRW:maze_grid_reset'

# ----- 7 in FlexDR_end.cpp ---------------------------------------------------
F=src/drt/src/dr/FlexDR_end.cpp
ensure_include "$F"
wrap_fn_body "$F" 'void FlexDRWorker::cleanup\(\)' 'DRW:worker_teardown'
wrap_fn_body "$F" 'void FlexDRWorker::endRemoveMarkers\(frDesign\* design\)' 'DRW:marker_remove'

# ----- 8, 9 in FlexDR_maze.cpp -----------------------------------------------
F=src/drt/src/dr/FlexDR_maze.cpp
wrap_fn_body "$F" 'bool FlexDRWorker::mazeIterInit_sortRerouteNets\(\s*int\s*[a-zA-Z_]+,\s*std::vector<drNet\*>&\s*[a-zA-Z_]+\s*\)' 'DRW:net_ordering'
wrap_fn_body "$F" 'bool FlexDRWorker::mazeIterInit_sortRerouteQueue\(\s*int\s*[a-zA-Z_]+,\s*std::vector<RouteQueueEntry>&\s*[a-zA-Z_]+\s*\)' 'DRW:net_ordering_q'

# ----- 10 in frRegionQuery.cpp -----------------------------------------------
F=src/drt/src/frRegionQuery.cpp
ensure_include "$F"
wrap_fn_body "$F" 'void frRegionQuery::addMarker\(frMarker\*\s*in\)' 'DRW:marker_insert'

# ----- 11 in gc/FlexGC.cpp ---------------------------------------------------
F=src/drt/src/gc/FlexGC.cpp
ensure_include "$F"
wrap_fn_body "$F" 'void FlexGCWorker::Impl::addMarker\(std::unique_ptr<frMarker>\s*in\)' 'DRW:marker_insert_gc'

# ----- 12 inline ripup block in route_queue_main ----------------------------
# The ripup block is inline within route_queue_main starting at the
# `for (auto& uConnFig : net->getRouteConnFigs())` and ending at `net->clear();`.
# We wrap it with an explicit { ProfileTask("DRW:ripup"); ... } scope.
F=src/drt/src/dr/FlexDR_maze.cpp
python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if '"DRW:ripup"' in s:
    sys.exit(0)
# Find the start: line with "for (auto& uConnFig : net->getRouteConnFigs())"
start_pat = re.compile(r'(\n)(\s*)for\s*\(auto&\s+uConnFig\s*:\s*net->getRouteConnFigs\(\)\)', flags=re.M)
sm = start_pat.search(s)
if not sm:
    print('FATAL: ripup start anchor not found', file=sys.stderr); sys.exit(6)
indent = sm.group(2)
# Find the matching net->clear(); after the start.
end_pat = re.compile(r'\n' + re.escape(indent) + r'net->clear\(\);')
em = end_pat.search(s, sm.end())
if not em:
    print('FATAL: ripup end anchor (net->clear()) not found', file=sys.stderr); sys.exit(7)
# Insert opening brace BEFORE the for-loop, and closing brace AFTER net->clear().
new_s = (
    s[:sm.start()]
    + sm.group(1)
    + indent + '{  // DRW:ripup begin\n'
    + indent + '  ProfileTask _p_v4("DRW:ripup");\n'
    + indent + 'for' + s[sm.end():em.end()]
    + '\n' + indent + '}  // DRW:ripup end'
    + s[em.end():]
)
p.write_text(new_s)
PY
echo "  ✓ DRW:ripup (inline block in route_queue_main)"

echo
echo "v4 patches applied. Rebuild with:"
echo "  cd $OPENROAD_ROOT && ./etc/Build.sh -no-gui -no-tests -threads=32 -cmake='-DPROFILE_CSV=ON'"
