#!/usr/bin/env bash
# kestrel/scripts/build-bench.sh
# Detect accelerator libs and build the harness with the right flags. Run on the box.
set -euo pipefail
cd "$(dirname "$0")/../bench"
FLAGS=()
ldconfig -p | grep -q libqatseqprod && ldconfig -p | grep -q libqatzip && FLAGS+=("WITH_QAT=1") || echo "QAT libs absent → skipping qat-* codecs"
ldconfig -p | grep -q libqpl && FLAGS+=("WITH_IAA=1") || echo "QPL absent → skipping iaa-deflate"
echo "building: make ${FLAGS[*]:-<software-only>}"
make clean && make selftest "${FLAGS[@]}" && ./selftest && make bench "${FLAGS[@]}"
echo "OK: ./bench built"
