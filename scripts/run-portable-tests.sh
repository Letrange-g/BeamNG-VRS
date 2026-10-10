#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for t in tests/check_*.py; do
  echo "[python] $t"
  python3 "$t"
done
for t in tests/*test.cpp; do
  echo "[c++] $t"
  exe="$(mktemp)"
  g++ -std=c++20 -O2 -Wall -Wextra -I . -I src "$t" -o "$exe"
  "$exe"
  rm -f "$exe"
done
