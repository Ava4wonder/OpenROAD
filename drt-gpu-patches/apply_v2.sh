#!/bin/bash
# apply_v2.sh — Extend DRT profile-CSV instrumentation with finer-grained
# per-function wrappers. Apply AFTER apply.sh.
#
# Adds ProfileTask RAII scopes at the top of:
#   * FlexDRWorker::route_queue_main            (dr/FlexDR_maze.cpp:1883)
#   * FlexDRWorker::routeNet (outer)            (dr/FlexDR_maze.cpp:3212)
#   * FlexDRWorker::routeNet_postAstarUpdate    (dr/FlexDR_maze.cpp:2426)
#   * FlexDRWorker::routeNet_postAstarWritePath (dr/FlexDR_maze.cpp:2525)
#   * FlexDRWorker::routeNet_postAstarPatchMinAreaVio (dr/FlexDR_maze.cpp:3308)
#   * FlexDRWorker::routeNet_postAstarAddPatchMetal (dr/FlexDR_maze.cpp:3669)
#   * FlexDRWorker::initMarkers                 (dr/FlexDR_init.cpp:3175)
#   * FlexGCWorker::Impl::updateGCWorker        (gc/FlexGC_init.cpp:994)

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
  # $1=file, $2=function-signature-regex (must capture through opening {), $3=task-name
  python3 - "$1" "$2" "$3" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
pat_src = sys.argv[2]
name = sys.argv[3]
s = p.read_text()
marker = f'"{name}"'
if marker in s:
    sys.exit(0)
pat = re.compile(pat_src + r'\s*\{\s*\n', flags=re.M)
m = pat.search(s)
if not m:
    print(f"FATAL: could not locate body of function for {name} in {p}", file=sys.stderr)
    sys.exit(5)
indent_guess = '  '
replacement = m.group(0) + f'{indent_guess}ProfileTask _prof("{name}");\n'
s2 = s[:m.start()] + replacement + s[m.end():]
p.write_text(s2)
PY
}

# --- FlexDR_maze.cpp extensions -----------------------------------------------
F=src/drt/src/dr/FlexDR_maze.cpp
ensure_include "$F"

wrap_fn_body "$F" \
  'void FlexDRWorker::route_queue_main\([^)]*\)' \
  'DRW:route_queue_main'

wrap_fn_body "$F" \
  'bool FlexDRWorker::routeNet\(drNet\* net, std::vector<FlexMazeIdx>& paths\)' \
  'DRW:routeNet'

wrap_fn_body "$F" \
  'void FlexDRWorker::routeNet_postAstarUpdate\([^{]*\)' \
  'DRW:postAstarUpdate'

wrap_fn_body "$F" \
  'void FlexDRWorker::routeNet_postAstarWritePath\([^{]*\)' \
  'DRW:postAstarWritePath'

wrap_fn_body "$F" \
  'void FlexDRWorker::routeNet_postAstarPatchMinAreaVio\([^{]*\)' \
  'DRW:postAstarPatchMinAreaVio'

wrap_fn_body "$F" \
  'void FlexDRWorker::routeNet_postAstarAddPatchMetal\([^{]*\)' \
  'DRW:postAstarAddPatchMetal'

echo "[1/3] FlexDR_maze.cpp extended"

# --- FlexDR_init.cpp ----------------------------------------------------------
F=src/drt/src/dr/FlexDR_init.cpp
ensure_include "$F"

wrap_fn_body "$F" \
  'void FlexDRWorker::initMarkers\(const frDesign\* design\)' \
  'DRW:initMarkers'

echo "[2/3] FlexDR_init.cpp extended"

# --- FlexGC_init.cpp — updateGCWorker -----------------------------------------
F=src/drt/src/gc/FlexGC_init.cpp
ensure_include "$F"

wrap_fn_body "$F" \
  'void FlexGCWorker::Impl::updateGCWorker\(\)' \
  'GC:updateGCWorker'

echo "[3/3] FlexGC_init.cpp extended"
echo
echo "Done. Rebuild with:"
echo "  cd $OPENROAD_ROOT"
echo "  ./etc/Build.sh -no-gui -no-tests -threads=32 -cmake='-DPROFILE_CSV=ON'"
