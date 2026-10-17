#!/bin/bash
# Regenerate the README images and animations in docs/media from your own disc.
#
#   tools/make_media.sh
#
# Needs a built ./build/alien, ffmpeg, and the disc image in
# AlienResurrectionBinary/ (or DISC=path). Most captures are headless and
# take seconds; the launcher and in-game overlay captures open a window
# (about a minute; don't move the mouse over it). Uses a scratch save
# folder, never your own config.ini or memory cards.
#
# Scenes:
#   * new game: Cross at frame 620 starts level 1, Ripley can move from
#     about frame 2240;
#   * alien attack: the first attract demo, with the game's demo
#     invulnerability (options +0x20) cleared at frame 2600 and demo
#     playback (0x800A48C4) stopped at 2633, so the HUD shows and the alien
#     that arrives at ~2640 attacks for real. The scene relies on the demo's
#     last stick input carrying over, as in the game's own stick handling
#     (--original-stick).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/alien}
DISC=${DISC:-AlienResurrectionBinary}
OUT=docs/media
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
command -v ffmpeg > /dev/null || { echo "ffmpeg is required" >&2; exit 1; }

NEW_GAME=(--press 620:cross)
ALIEN_SCENE=(--poke 2600:80096bdc:0:1 --poke 2633:800a48c4:0 --original-stick)
# ignore connected controllers: the launcher would name them on screen
export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000

headless() {
    rm -rf "$TMP/save" && mkdir -p "$TMP/save"
    "$BIN" --headless --renderer hw --exe ALIEN.BIN --disc "$DISC" --save-dir "$TMP/save" "$@" \
        > "$TMP/log.txt" 2>&1 || { cat "$TMP/log.txt" >&2; exit 1; }
}

# jpg IN OUT WIDTH [scale flags]
jpg() {
    ffmpeg -loglevel error -y -i "$1" -vf "scale=$3:-2:flags=${4:-lanczos}" -q:v 3 "$2"
}

# anim_pair LEFT_PREFIX RIGHT_PREFIX OUT WIDTH FPS HOLD_SECONDS
# Two numbered frame sequences side by side as a looping animated WebP
# (shown like a GIF on GitHub, at a fraction of a GIF's size for moving
# scenes), last frame held.
anim_pair() {
    ffmpeg -loglevel error -y -framerate "$5" -i "$1_%04d.png" -framerate "$5" -i "$2_%04d.png" \
        -filter_complex "[0]scale=$4:-2:flags=lanczos,pad=iw+8:ih:0:0:black[a];\
[1]scale=$4:-2:flags=lanczos[b];[a][b]hstack=inputs=2,tpad=stop_mode=clone:stop_duration=$6" \
        -c:v libwebp_anim -pix_fmt yuv420p -lossless 0 -q:v 70 -loop 0 "$3"
}

echo "comparison: original vs this port"
headless --scale 1 --aspect 4:3 --no-pgxp --sharp --frames 2645 "${ALIEN_SCENE[@]}" \
    --shot 2643:"$TMP/orig.png"
jpg "$TMP/orig.png" "$OUT/compare_original.jpg" 960 neighbor
headless --scale 3 --frames 2645 "${ALIEN_SCENE[@]}" --shot 2643:"$TMP/port.png"
jpg "$TMP/port.png" "$OUT/compare_port.jpg" 1280

echo "sprint"
headless --scale 2 --frames 2492 "${NEW_GAME[@]}" --stick 2250:240:128:0:128:128 \
    --record 2250:120:2:"$TMP/run"
headless --scale 2 --frames 2492 "${NEW_GAME[@]}" --stick 2250:240:128:0:128:128 \
    --press 2250:sprint:240 --record 2250:120:2:"$TMP/sprint"
anim_pair "$TMP/run" "$TMP/sprint" "$OUT/sprint.webp" 640 30 1

echo "enemy damage"
headless --scale 2 --frames 2760 "${ALIEN_SCENE[@]}" --record 2630:64:2:"$TMP/dmg100"
headless --scale 2 --frames 2760 "${ALIEN_SCENE[@]}" --enemy-damage 50 \
    --record 2630:64:2:"$TMP/dmg50"
anim_pair "$TMP/dmg100" "$TMP/dmg50" "$OUT/enemy_damage.webp" 640 30 2

echo "infinite flashlight"
# level 1 has no flashlight: give it (inventory 0x8011BCB8 +4, bit 28) and
# switch it on with Circle; the original battery runs out after 1200 vblanks
FLASH=(--poke 2255:8011bcbc:10000100 --press 2270:circle --frames 3602)
headless --scale 3 "${NEW_GAME[@]}" "${FLASH[@]}" --no-infinite-flashlight \
    --shot 3600:"$TMP/flash_orig.png"
jpg "$TMP/flash_orig.png" "$OUT/flashlight_original.jpg" 960
headless --scale 3 "${NEW_GAME[@]}" "${FLASH[@]}" --shot 3600:"$TMP/flash_inf.png"
jpg "$TMP/flash_inf.png" "$OUT/flashlight_infinite.jpg" 960

echo "launcher (opens a window)"
for page in main controls; do
    rm -rf "$TMP/save" && mkdir -p "$TMP/save"
    "$BIN" --disc "$DISC" --save-dir "$TMP/save" --launcher-shot "$TMP/launcher_$page.png" \
        --launcher-page "$page" > "$TMP/log.txt" 2>&1
done
jpg "$TMP/launcher_main.png" "$OUT/launcher.jpg" 1280
jpg "$TMP/launcher_controls.png" "$OUT/controls.jpg" 1280

echo "in-game overlay (opens a window, about 40 seconds)"
# pause menu, then Settings scrolled to the Gameplay and Enemy damage rows
keys="F2,WAIT20,SHOT,Down,Return"
for i in $(seq 13); do keys="$keys,Down"; done
keys="$keys,SHOT,Escape,Escape"
rm -rf "$TMP/save" && mkdir -p "$TMP/save"
"$BIN" --play --skip-intro --disc "$DISC" --save-dir "$TMP/save" "${NEW_GAME[@]}" \
    --frames 2400 --overlay-keys "2300:$keys" --overlay-shot "$TMP/overlay" \
    > "$TMP/log.txt" 2>&1
jpg "$TMP/overlay_1.png" "$OUT/pause.jpg" 1280
jpg "$TMP/overlay_2.png" "$OUT/settings.jpg" 1280

ls -l "$OUT"
