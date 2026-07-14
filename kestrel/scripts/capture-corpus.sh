#!/usr/bin/env bash
# kestrel/scripts/capture-corpus.sh
#
# Capture a REAL wide / high-cardinality QWP corpus on THIS box. Needs only JDK (25, to match
# the reference build) + Maven — NOT Rust: the x86-64 libquestdbr.so is committed in the repo
# and the capture hooks are Java-only, so `mvn` reuses the committed native lib.
#
# The captured corpus stays LOCAL (default: outside the repo, never committed), so it can be as
# large as you like — genuinely hundreds of millions of rows of real bodies, sitting right next
# to the bench + accelerators. When done, point bench/iaa_offload/iaa_scale at it.
#
# Run from the repo root (the QuestDB tree, i.e. the dir that contains core/ and kestrel/):
#   bash kestrel/scripts/capture-corpus.sh [rowCount] [maxBodies] [dataDir] [corpusDir]
# Defaults: rowCount=100000000  maxBodies=128  dataDir=$HOME/kestrel-capture-data  corpusDir=$HOME/kestrel-corpus-wide
# Point dataDir at a disk with room for the table (~100M rows ≈ several GB).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

ROWS="${1:-100000000}"
MAXB="${2:-128}"
DATA="${3:-$HOME/kestrel-capture-data}"
CORPUS="${4:-$HOME/kestrel-corpus-wide}"
PATCH="kestrel/patches/dump-hooks.patch"

# JVM module-access flags QuestDB needs (workers die silently without them — see dump-hooks.md).
FLAGS=(--sun-misc-unsafe-memory-access=allow --enable-native-access=ALL-UNNAMED
  --add-opens=java.base/java.lang=ALL-UNNAMED --add-opens=java.base/java.lang.reflect=ALL-UNNAMED
  --add-opens=java.base/java.nio=ALL-UNNAMED --add-opens=java.base/java.time.zone=ALL-UNNAMED
  --add-exports=java.base/jdk.internal.vm=ALL-UNNAMED)

command -v java >/dev/null || { echo "ERROR: install a JDK 25 first (Adoptium Temurin 25, or your distro's openjdk-25)."; exit 1; }
command -v mvn  >/dev/null || { echo "ERROR: install Maven first (e.g. apt-get install -y maven)."; exit 1; }
java -version 2>&1 | grep -qE '"(2[5-9]|[3-9][0-9])' || echo "WARN: JDK 25 recommended; found $(java -version 2>&1 | head -1)"

mkdir -p "$DATA/conf" "$CORPUS"
printf 'line.tcp.enabled=false\n' > "$DATA/conf/server.conf"   # ILP/TCP unused; ingest is QWP-over-WebSocket on 9000

# The wide driver hardcodes localhost:9000 (HTTP/WS/QWP) + 8812 (PG). Both must be OURS, else we'd
# create the table on / capture from a different server and get zero bodies. Fail fast if taken.
for p in 9000 8812; do
  if (exec 3<>/dev/tcp/127.0.0.1/"$p") 2>/dev/null; then exec 3>&- 3<&-
    echo "ERROR: port $p is already in use — stop the other QuestDB first (capture needs 9000+8812 free)."; exit 1
  fi
done

echo "== applying throwaway capture hooks =="
git apply "$PATCH"
SRV=""
cleanup() { echo "== cleanup: revert hooks + stop server =="; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true; git apply -R "$PATCH" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

echo "== building QuestDB (Java only, committed native lib, no Rust) =="
mvn -q -pl benchmarks -am -Dmaven.test.skip=true package
mvn -q -pl benchmarks dependency:build-classpath -Dmdep.outputFile=/tmp/kestrel-cp.txt
DEPS="$(cat /tmp/kestrel-cp.txt)"
SERVER_CP="core/target/classes:$DEPS"                          # hooked classes first
DRIVER_CP="core/target/classes:benchmarks/target/classes:$DEPS"

echo "== booting server (hooks ON, KESTREL_DUMP_DIR=$CORPUS, up to $MAXB bodies/direction) =="
rm -f "$CORPUS"/egress-*.bin "$CORPUS"/ingress-*.bin 2>/dev/null || true
java "${FLAGS[@]}" -DKESTREL_DUMP_DIR="$CORPUS" -DKESTREL_DUMP_MAX="$MAXB" \
  -cp "$SERVER_CP" io.questdb.ServerMain -d "$DATA" > /tmp/kestrel-server.log 2>&1 &
SRV=$!
echo -n "   waiting for :9000 "; ready=0
for _ in $(seq 1 180); do
  if (exec 3<>/dev/tcp/127.0.0.1/9000) 2>/dev/null; then exec 3>&- 3<&-; ready=1; break; fi
  kill -0 "$SRV" 2>/dev/null || { echo; echo "server died on boot — see /tmp/kestrel-server.log"; exit 1; }
  echo -n .; sleep 1
done
[ "$ready" = 1 ] || { echo; echo "server not ready — see /tmp/kestrel-server.log"; exit 1; }
echo " up"; sleep 2

echo "== ingest $ROWS wide/high-cardinality rows + query back (fires the hooks) =="
echo "   (once egress+ingress bodies reach $MAXB you can Ctrl-C — the rest of the driver's PG/HTTP passes just add time)"
java "${FLAGS[@]}" -cp "$DRIVER_CP" -DrowCount="$ROWS" org.questdb.QwpEgressReadBenchmarkWide || true

echo
E=$(ls "$CORPUS"/egress-*.bin  2>/dev/null | wc -l)
I=$(ls "$CORPUS"/ingress-*.bin 2>/dev/null | wc -l)
echo "== captured: $E egress + $I ingress real wide bodies in $CORPUS =="
du -sh "$CORPUS" 2>/dev/null
echo
echo "Next — run the bake-off over the REAL wide corpus and compare to the narrow one:"
echo "  cd kestrel/bench && make bench 2>/dev/null; ./bench $CORPUS | tee wide.csv"
echo "  # ratios: wide/high-cardinality vs the committed narrow corpus (kestrel/corpus/)"
echo "  sudo ./iaa_offload $CORPUS     # offload numbers on real production-shaped bodies (if built with QPL)"
