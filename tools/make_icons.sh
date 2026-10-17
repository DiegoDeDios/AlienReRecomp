#!/bin/sh
# Regenerate the app icons (original artwork) in packaging/icon/:
#   icon_1024.png (master, tools/make_icon.swift), AppIcon.icns (macOS),
#   AppIcon.ico (Windows). macOS only (swift, sips, iconutil).
set -e
cd "$(dirname "$0")/.."
OUT=packaging/icon
swift tools/make_icon.swift $OUT/icon_1024.png
TMP=$(mktemp -d)
SET=$TMP/AppIcon.iconset
mkdir -p $SET
for s in 16 32 128 256 512; do
    sips -z $s $s $OUT/icon_1024.png --out $SET/icon_${s}x${s}.png >/dev/null
    d=$((s * 2))
    sips -z $d $d $OUT/icon_1024.png --out $SET/icon_${s}x${s}@2x.png >/dev/null
done
iconutil -c icns $SET -o $OUT/AppIcon.icns
for s in 16 24 32 48 64 128 256; do
    sips -z $s $s $OUT/icon_1024.png --out $TMP/ico_$s.png >/dev/null
done
# .ico with PNG-compressed images (Windows Vista and later)
python3 - "$TMP" "$OUT/AppIcon.ico" <<'PY'
import struct, sys
tmp, out = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]
imgs = [open('%s/ico_%d.png' % (tmp, s), 'rb').read() for s in sizes]
data = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for s, img in zip(sizes, imgs):
    data += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(img), off)
    off += len(img)
open(out, 'wb').write(data + b''.join(imgs))
PY
rm -rf "$TMP"
ls -la $OUT
