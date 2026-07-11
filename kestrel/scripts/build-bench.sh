#!/usr/bin/env bash
# kestrel/scripts/build-bench.sh
# Detect accelerator libs and build the harness with the right flags. Run on the box.
set -euo pipefail
cd "$(dirname "$0")/../bench"
FLAGS=()
# Require every lib the Makefile actually links under each flag (WITH_QAT: -lqatseqprod
# -lqatzip -lqat -lusdm; WITH_IAA: -lqpl -laccel-config) so a partial install degrades to
# software instead of failing the link. Patterns mirror probe.sh (libqat\. = base QAT lib).
have(){ local l; for l in "$@"; do ldconfig -p 2>/dev/null | grep -qE "$l" || return 1; done; }
have libqatseqprod libqatzip 'libqat\.' libusdm && FLAGS+=("WITH_QAT=1") || echo "QAT libs absent (need libqatseqprod libqatzip libqat libusdm) → skipping qat-* codecs"
have libqpl libaccel-config && FLAGS+=("WITH_IAA=1") || echo "IAA/QPL libs absent (need libqpl libaccel-config) → skipping iaa-deflate"
echo "building: make ${FLAGS[*]:-<software-only>}"
make clean && make selftest "${FLAGS[@]}" && ./selftest && make bench "${FLAGS[@]}"
echo "OK: ./bench built"
