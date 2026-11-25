#!/bin/sh
# PC port, Linux (WSL): tools, Ruby and Sapphire, tests. From Windows:
#   MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- sh /mnt/d/LUC/pokemonall/decomp/pokeruby/src/platform_pc/scripts/build_linux.sh [ROM_DIR]
# Builds out of the source tree (in $HOME: much faster than /mnt/d). The
# optional ROM folder (rom_dir, the player's own ROMs; default
# /mnt/d/LUC/pokemonall/roms if it exists) enables the game_french tests and
# is written into each build's config.ini.
set -e
SRC=/mnt/d/LUC/pokemonall/decomp/pokeruby
OUT=$HOME/pokeruby-linux
ROM_DIR=${1:-/mnt/d/LUC/pokemonall/roms}
[ -d "$ROM_DIR" ] || ROM_DIR=
mkdir -p "$OUT"
cmake -S "$SRC/src/platform_pc/tools" -B "$OUT/tools" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$OUT/tools"
cmake -S "$SRC" -B "$OUT/ruby" -G Ninja -DCMAKE_C_COMPILER=clang -DPRS_ROM_DIR="$ROM_DIR"
cmake --build "$OUT/ruby"
cmake -S "$SRC" -B "$OUT/sapphire" -G Ninja -DCMAKE_C_COMPILER=clang -DPRS_GAME_VERSION=SAPPHIRE -DPRS_ROM_DIR="$ROM_DIR"
cmake --build "$OUT/sapphire"
# config.ini next to each exe (created with its defaults by a first run): rom_dir.
for b in ruby:pokeruby sapphire:pokesapphire; do
    d="$OUT/${b%%:*}"
    [ -f "$d/config.ini" ] || (cd "$d" && PRS_HEADLESS=1 PRS_MUTE=1 PRS_EXIT_AFTER=1 "./${b##*:}" >/dev/null 2>&1 || true)
    if [ -n "$ROM_DIR" ] && [ -f "$d/config.ini" ]; then
        if grep -q '^rom_dir *=' "$d/config.ini"; then
            sed -i "s|^rom_dir *=.*|rom_dir = $ROM_DIR|" "$d/config.ini"
        else    # config.ini older than the rom_dir key
            sed -i "/^rom *=/a rom_dir = $ROM_DIR" "$d/config.ini"
        fi
    fi
done
cd "$OUT/ruby" && ctest --output-on-failure --timeout 3000
cd "$OUT/sapphire" && ctest --output-on-failure --timeout 3000
