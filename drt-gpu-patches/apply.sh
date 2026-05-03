#!/bin/bash
# Idempotent applier for DRT profile-CSV instrumentation.
# Usage: ./apply.sh [OPENROAD_ROOT]
#   default OPENROAD_ROOT = ~/openroad-profile/OpenROAD

set -euo pipefail

OPENROAD_ROOT="${1:-$HOME/openroad-profile/OpenROAD}"
PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[apply.sh] OPENROAD_ROOT = $OPENROAD_ROOT"
echo "[apply.sh] PATCH_DIR     = $PATCH_DIR"

[[ -d "$OPENROAD_ROOT/src/drt/src" ]] || { echo "FATAL: drt source tree not found" >&2; exit 2; }

# -----------------------------------------------------------------------------
# 1) Replace frProfileTask.h with our three-branch version.
# -----------------------------------------------------------------------------
install -m 0644 "$PATCH_DIR/frProfileTask.h" "$OPENROAD_ROOT/src/drt/src/frProfileTask.h"
echo "[1/5] frProfileTask.h replaced"

# -----------------------------------------------------------------------------
# 2) Install new CSV backend source.
# -----------------------------------------------------------------------------
install -m 0644 "$PATCH_DIR/frProfileTaskCsv.cpp" "$OPENROAD_ROOT/src/drt/src/frProfileTaskCsv.cpp"
echo "[2/5] frProfileTaskCsv.cpp installed"

# -----------------------------------------------------------------------------
# 3) Add ProfileTask at top of FlexGridGraph::search body.
#    Target file does not currently include frProfileTask.h; add the include.
# -----------------------------------------------------------------------------
F="$OPENROAD_ROOT/src/drt/src/dr/FlexGridGraph_maze.cpp"
if ! grep -q 'frProfileTask.h' "$F"; then
  # Add include near other drt/ headers
  python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if 'frProfileTask.h' in s:
    sys.exit(0)
# Insert after the last existing drt local-include
m = list(re.finditer(r'^#include\s+"[^"]+"\s*$', s, flags=re.M))
if not m:
    # Fallback: after the first blank line
    idx = s.find('\n\n')
    assert idx > 0, "cannot find insertion point"
    s = s[:idx] + '\n#include "frProfileTask.h"' + s[idx:]
else:
    last = m[-1]
    s = s[:last.end()] + '\n#include "frProfileTask.h"' + s[last.end():]
p.write_text(s)
PY
fi

# Insert ProfileTask scope at the first line of the search() body.
python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
marker = 'DRW:maze_search'
if marker in s:
    sys.exit(0)
# Match the function body opener unambiguously
pat = re.compile(
    r'(bool FlexGridGraph::search\([^{]*\)\s*\{\s*\n)',
    flags=re.M,
)
new_s, n = pat.subn(
    r'\1  ProfileTask _prof_search("DRW:maze_search");\n',
    s,
    count=1,
)
if n != 1:
    print("FATAL: could not locate FlexGridGraph::search body", file=sys.stderr)
    sys.exit(3)
p.write_text(new_s)
PY
echo "[3/5] FlexGridGraph_maze.cpp instrumented"

# -----------------------------------------------------------------------------
# 4) Instrument FlexGCWorker::Impl::main():
#      - ProfileTask at top of function
#      - wrap each check*() call in a scoped ProfileTask
# -----------------------------------------------------------------------------
F="$OPENROAD_ROOT/src/drt/src/gc/FlexGC_main.cpp"
python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()

# (a) Add top-of-function ProfileTask if not already present.
if '"DRW:gc_main"' not in s:
    pat = re.compile(
        r'(int FlexGCWorker::Impl::main\(\)\s*\{\s*\n)',
        flags=re.M,
    )
    new_s, n = pat.subn(
        r'\1  ProfileTask _prof_gc("DRW:gc_main");\n',
        s,
        count=1,
    )
    if n != 1:
        print("FATAL: could not locate FlexGCWorker::Impl::main body", file=sys.stderr)
        sys.exit(4)
    s = new_s

# (b) Wrap individual check calls. Each substitution is idempotent: if the
# ProfileTask wrapper is already present, the pattern won't match.
check_wraps = [
    ('checkMetalCornerSpacing', 'GC:checkMetalCornerSpacing', 'checkMetalCornerSpacing();'),
    ('checkMetalSpacing',       'GC:checkMetalSpacing',       'checkMetalSpacing();'),
    ('checkMetalShape_false',   'GC:checkMetalShape',         'checkMetalShape(false);'),
    ('checkMetalEndOfLine',     'GC:checkMetalEndOfLine',     'checkMetalEndOfLine();'),
    ('checkCutSpacing',         'GC:checkCutSpacing',         'checkCutSpacing();'),
    ('checkMetalSpacingTableInfluence',
                                 'GC:checkMetalSpacingTableInfluence',
                                 'checkMetalSpacingTableInfluence();'),
]
for _key, tag, call in check_wraps:
    wrapper_marker = f'"{tag}"'
    if wrapper_marker in s:
        continue
    # Match a standalone occurrence of the call that is NOT already inside our wrapper block.
    # Use a narrow single-line pattern: the call on its own line with leading whitespace.
    pat = re.compile(r'^(\s*)' + re.escape(call) + r'\s*$', flags=re.M)
    matches = list(pat.finditer(s))
    if not matches:
        print(f"WARN: did not find call site for {call}", file=sys.stderr)
        continue
    # Only replace the *first* occurrence inside FlexGCWorker::Impl::main.
    # Heuristic: first match after the "int FlexGCWorker::Impl::main()" header.
    header = re.search(r'int FlexGCWorker::Impl::main\(\)', s)
    assert header, "FlexGCWorker::Impl::main header not found"
    target = next((m for m in matches if m.start() > header.end()), None)
    if target is None:
        print(f"WARN: call {call} not found after main() header", file=sys.stderr)
        continue
    indent = target.group(1)
    replacement = (
        f'{indent}{{\n'
        f'{indent}  ProfileTask _p("{tag}");\n'
        f'{indent}  {call}\n'
        f'{indent}}}'
    )
    s = s[:target.start()] + replacement + s[target.end():]

p.write_text(s)
PY
echo "[4/5] FlexGC_main.cpp instrumented"

# -----------------------------------------------------------------------------
# 5) Patch src/drt/CMakeLists.txt:
#      - add option(PROFILE_CSV ...)
#      - if PROFILE_CSV: add source + target_compile_definitions
# -----------------------------------------------------------------------------
F="$OPENROAD_ROOT/src/drt/CMakeLists.txt"
python3 - "$F" <<'PY'
import sys, pathlib, re
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if 'PROFILE_CSV' in s:
    sys.exit(0)

# 5a) Add option() after the existing DEBUG_DRT_UNDERFLOW option line.
s = re.sub(
    r'(option\(DEBUG_DRT_UNDERFLOW[^\n]*\)\n)',
    r'\1option(PROFILE_CSV "Emit per-task CSV timing from DRT (topic: drt-gpu)" OFF)\n',
    s,
    count=1,
)

# 5b) Append conditional target_sources + definition at end of file.
s += """

# DRT profile-CSV instrumentation (topic: design-gpu-kernels-for-the-drt-phase-of-openroad...)
if(PROFILE_CSV)
  target_sources(drt_lib PRIVATE src/frProfileTaskCsv.cpp)
  target_compile_definitions(drt_lib PRIVATE PROFILE_CSV=1)
endif()
"""
p.write_text(s)
PY
echo "[5/5] CMakeLists.txt patched"

echo
echo "All patches applied. To build with instrumentation:"
echo "  cd $OPENROAD_ROOT"
echo "  ./etc/Build.sh -no-gui -no-tests -threads=32 -cmake='-DPROFILE_CSV=ON'"
echo "Then run with:"
echo "  DRT_PROFILE_CSV=/tmp/drt_profile.csv ./build/bin/openroad ..."
