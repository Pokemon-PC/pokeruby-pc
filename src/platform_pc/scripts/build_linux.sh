#!/bin/sh
# PC port, Linux (WSL): tools, Ruby and Sapphire, tests. From Windows:
#   MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- sh /mnt/d/LUC/pokemonall/decomp/pokeruby/src/platform_pc/scripts/build_linux.sh [RUBY_FR_ROM SAPPHIRE_FR_ROM]
# Builds out of the source tree (in $HOME: much faster than /mnt/d). The
# optional French ROMs (the player's own) enable the game_french tests.
set -e
SRC=/mnt/d/LUC/pokemonall/decomp/pokeruby
OUT=$HOME/pokeruby-linux
mkdir -p "$OUT"
cmake -S "$SRC/src/platform_pc/tools" -B "$OUT/tools" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$OUT/tools"
cmake -S "$SRC" -B "$OUT/ruby" -G Ninja -DCMAKE_C_COMPILER=clang -DPRS_FR_ROM="${1:-}"
cmake --build "$OUT/ruby"
cmake -S "$SRC" -B "$OUT/sapphire" -G Ninja -DCMAKE_C_COMPILER=clang -DPRS_GAME_VERSION=SAPPHIRE -DPRS_FR_ROM="${2:-}"
cmake --build "$OUT/sapphire"
cd "$OUT/ruby" && ctest --output-on-failure --timeout 3000
cd "$OUT/sapphire" && ctest --output-on-failure --timeout 3000
