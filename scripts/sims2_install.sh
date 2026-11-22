#!/usr/bin/env bash
# The Sims 2 (PAL, SLES-53718) remaster: builds the game from the player's own disc image. The launcher (Sims2Launcher.exe) runs this
# inside its downloaded bash with a PATH of its downloaded tools only; it reads the "[install] step N/5: ..." lines for its progress page.
#   scripts/sims2_install.sh <disc image>
# 1 check the disc, 2 put the kit (kit/sims2: our patch section, entries.txt, the menu font) into games/sims2 without overwriting
# anything there, 3 download PCSX2's public patch file for the game (scripts/pnach.py fetch), 4 build with port.sh exactly as the
# remaster is built (--no-explore --native-only, sections "50 FPS" + "Aspect from memory", --widescreen), 5 take the menu sounds from
# the disc (scripts/sims2_sounds.py). Nothing taken from the disc ever goes into the repository.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ $# -eq 1 ] || { echo "usage: scripts/sims2_install.sh <disc image>" >&2; exit 2; }
iso="$1"
[ -f "$iso" ] || { echo "[install] failed: the disc image $iso is not there" >&2; exit 1; }
py=()
for candidate in python3 python "py -3"; do
  if $candidate -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' > /dev/null 2>&1; then read -r -a py <<< "$candidate"; break; fi
done
[ ${#py[@]} -gt 0 ] || { echo "[install] failed: no Python 3.8 or newer" >&2; exit 1; }
game="$ROOT/games/sims2"

echo "[install] step 1/5: checking the disc"
check="$(mktemp -d)"
"${py[@]}" -I scripts/disc.py extract "$iso" "$check" SYSTEM.CNF > /dev/null 2>&1 || { echo "[install] failed: $iso cannot be read as a PS2 disc image" >&2; exit 1; }
cnf="$(tr '[:lower:]' '[:upper:]' < "$check/SYSTEM.CNF")"
rm -rf "$check"
case "$cnf" in
  *SLES_537.18*) ;;
  *SLUS_212.65*) echo "[install] failed: the US version (SLUS-21265) is not supported yet" >&2; exit 2 ;;
  *) echo "[install] failed: this is not The Sims 2 (PAL, SLES-53718)" >&2; exit 2 ;;
esac

echo "[install] step 2/5: the remaster's own files"
mkdir -p "$game"
cp -r -n kit/sims2/. "$game/"

echo "[install] step 3/5: PCSX2's patch file for the game"
community="$game/pnach-pcsx2"
"${py[@]}" -I scripts/pnach.py fetch "$iso" --out "$community"
pnach_file="$(ls "$community"/*.pnach | head -1)"
[ -f "$pnach_file" ] || { echo "[install] failed: PCSX2's patch file was not downloaded" >&2; exit 1; }

echo "[install] step 4/5: building the game (this takes a few minutes)"
bash scripts/port.sh sims2 "$iso" --no-explore --native-only --pnach "$pnach_file" --pnach "$game/remaster.pnach" \
  --pnach-section "50 FPS" --pnach-section "Aspect from memory" --widescreen

echo "[install] step 5/5: the menu sounds from the disc"
"${py[@]}" -I scripts/sims2_sounds.py "$iso" "$game/menu/sounds"
[ -f build/native-sims2/ps2run.exe ] || [ -f build/native-sims2/ps2run ] || { echo "[install] failed: the game was not built" >&2; exit 1; }
echo "[install] done"
