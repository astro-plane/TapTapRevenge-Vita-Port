#!/bin/sh
# Headless regression run (needs a C compiler and libpng headers):
#   test/run_tests.sh <data_dir> [snapshot_dir]
set -e
cd "$(dirname "$0")/.."
DATA="${1:?usage: test/run_tests.sh <data_dir> [snapshot_dir]}"
OUT="${2:-/tmp/ttrvita_shots}"
case "$DATA" in */) ;; *) DATA="$DATA/" ;; esac
mkdir -p "$OUT"
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o "$OUT/ttrtest" \
    src/game.c src/theme.c src/png_load.c test/plat_soft.c test/test_main.c $(pkg-config --cflags --libs libpng) -lm
TTR_DATA="$DATA" "$OUT/ttrtest" "$OUT"
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o "$OUT/themecheck" \
    src/theme.c src/png_load.c test/plat_soft.c test/theme_check.c $(pkg-config --cflags --libs libpng) -lm
TTR_DATA="$DATA" "$OUT/themecheck"
echo "snapshots are in $OUT"
