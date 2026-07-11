#!/usr/bin/env bash
# kestrel/scripts/run-bench.sh
# Run the sweep over the committed corpus, print a readable table, keep results.csv. Run on the box.
set -euo pipefail
cd "$(dirname "$0")/../bench"
[ -x ./bench ] || { echo "run build-bench.sh first"; exit 1; }
./bench ../corpus
echo; echo "=== results (sorted by ratio within codec) ==="
{ head -1 results.csv; tail -n +2 results.csv | sort -t, -k1,1 -k6,6n; } | column -s, -t
echo; echo "Paste results.csv back. Key columns: ratio, throughput_MBps, p99_us, cores_freed (compare by direction: compress vs decompress)."
