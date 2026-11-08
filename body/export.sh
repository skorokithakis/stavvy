#!/bin/sh
# Regenerate stl/*.stl and the three preview renders from eve_robot.scad.
#
# Every part is exported through its print_* variant, so the print orientation
# lives in the .scad file and each STL comes out sitting on the bed (min Z = 0).
# Set OPENSCAD if openscad is not on PATH.

set -e
cd "$(dirname "$0")"

OPENSCAD="${OPENSCAD:-openscad}"
mkdir -p stl

# Report the triangle count and the lowest vertex of an STL, and fail if the
# part is not sitting on the bed. The arms' bed shift is a measured constant
# rather than a formula, so this is the check that catches it going stale.
check_on_bed() {
    python3 - "$1" <<'PY'
import struct
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
count = struct.unpack('<I', data[80:84])[0]
zs = []
for i in range(count):
    values = struct.unpack('<12f', data[84 + i * 50:84 + i * 50 + 48])
    zs += [values[5], values[8], values[11]]
low, high = min(zs), max(zs)
print('%-22s %6d tris  min Z %+.5f  height %.2f' % (path, count, low, high - low))
if abs(low) > 0.01:
    sys.exit('%s is not on the bed: min Z %+.4f' % (path, low))
PY
}

for part in body neck arm_left arm_right; do
    "$OPENSCAD" -D "part=\"print_$part\"" --export-format binstl \
        -o "stl/$part.stl" eve_robot.scad
    check_on_bed "stl/$part.stl"
done

"$OPENSCAD" --imgsize=760,1100 --colorscheme=Tomorrow \
    --camera=0,0,50,90,0,0,300 -o preview_front.png eve_robot.scad
"$OPENSCAD" --imgsize=760,1100 --colorscheme=Tomorrow \
    --camera=0,0,50,90,0,90,300 -o preview_side.png eve_robot.scad
"$OPENSCAD" --imgsize=760,1100 --colorscheme=Tomorrow \
    --camera=0,0,50,90,0,180,300 -o preview_back.png eve_robot.scad
