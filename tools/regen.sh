#!/bin/sh
# Regenerate everything derived from the disc image:
#   1. extract the ISO9660 filesystem      -> disc/
#   2. identify PsyQ library functions     -> analysis/psyq_symbols_*.txt
#   3. statically recompile both executables -> src/gen/{alien,slus}
set -e
cd "$(dirname "$0")/.."
BIN="AlienResurrectionBinary/Alien Resurrection (USA) (Track 1).bin"
PY="python3 -I"

[ -f disc/ALIEN.BIN ] || $PY tools/extract_iso.py "$BIN" disc
mkdir -p analysis
[ -f analysis/psyq_symbols_alien.txt ] || \
    $PY tools/psyq_match_all.py disc/ALIEN.BIN reference/psx_psyq_signatures > analysis/psyq_symbols_alien.txt
[ -f analysis/psyq_symbols_slus.txt ] || \
    $PY tools/psyq_match_all.py disc/SLUS_006.33 reference/psx_psyq_signatures > analysis/psyq_symbols_slus.txt

# ALIEN.BIN: main code 0x80010100-0x80091300, plus a hand-written assembly
# section placed after .data (0x800A4A2C-0x800A8680).
$PY tools/recomp.py --exe disc/ALIEN.BIN --code-end 80091300 \
    --extra-code 800a4a2c:800a8680 --prefix ALIEN \
    --syms analysis/psyq_symbols_alien.txt --out src/gen/alien \
    --overrides tools/overrides_alien.txt
$PY tools/recomp.py --exe disc/SLUS_006.33 --code-end 8001dc20 --prefix SLUS \
    --syms analysis/psyq_symbols_slus.txt --out src/gen/slus --split 4 \
    --overrides tools/overrides_slus.txt
