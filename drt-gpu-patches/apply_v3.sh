#!/bin/bash
# apply_v3.sh — Wrap the outer FlexDRWorker::route_queue() at FlexDR_maze.cpp:1721
# (complements apply_v2.sh which wraps route_queue_main at :1883).
#
# Goal: close the ~3 600 s gap between route_queue_main (28.6% of DRW:main on
# jpeg) and the unaccounted 71% of per-worker time. route_queue is the outer
# wrapper that builds the ripup queue and handles per-iter setup.

set -euo pipefail

OPENROAD_ROOT="${1:-$HOME/openroad-profile/OpenROAD}"
cd "$OPENROAD_ROOT"

F=src/drt/src/dr/FlexDR_maze.cpp

python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if '"DRW:route_queue"' in s:
    print('[v3] already applied; skipping')
    sys.exit(0)
# Match exactly `void FlexDRWorker::route_queue()` — zero-arg signature.
pat = re.compile(
    r'(void FlexDRWorker::route_queue\(\)\s*\{\s*\n)',
    flags=re.M,
)
new_s, n = pat.subn(
    r'\1  ProfileTask _prof_rq("DRW:route_queue");\n',
    s,
    count=1,
)
if n != 1:
    print('FATAL: could not locate FlexDRWorker::route_queue()', file=sys.stderr)
    sys.exit(3)
p.write_text(new_s)
print('[v3] wrapped route_queue()')
PY

echo "[done] To rebuild with PROFILE_CSV still on:"
echo "  cd $OPENROAD_ROOT"
echo "  ./etc/Build.sh -no-gui -no-tests -threads=32 -cmake='-DPROFILE_CSV=ON'"
