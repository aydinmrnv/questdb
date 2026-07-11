# Corpus

Real, post-serialization QWP wire bodies captured from a live QuestDB server via the
throwaway dump hooks documented in `../patches/dump-hooks.md`. Every file is the exact
byte range QuestDB writes into the WebSocket send/recv buffer for one QWP message body
(egress: delta section + table block, pre-zstd; ingress: the raw received binary-frame
payload) — i.e. genuine wire content, not synthetic/random data.

## Files

| file | bytes | direction |
|---|---:|---|
| `egress-0-1294b.bin`      | 1,294     | egress |
| `egress-1-3759b.bin`      | 3,759     | egress |
| `egress-2-12553b.bin`     | 12,553    | egress |
| `egress-3-77366b.bin`     | 77,366    | egress |
| `egress-4-93392b.bin`     | 93,392    | egress |
| `egress-5-206881b.bin`    | 206,881   | egress |
| `egress-6-280108b.bin`    | 280,108   | egress |
| `egress-7-423691b.bin`    | 423,691   | egress |
| `ingress-0-1316b.bin`     | 1,316     | ingress |
| `ingress-1-3781b.bin`     | 3,781     | ingress |
| `ingress-2-12575b.bin`    | 12,575    | ingress |
| `ingress-3-77388b.bin`    | 77,388    | ingress |
| `ingress-4-206903b.bin`   | 206,903   | ingress |
| `ingress-5-258999b.bin`   | 258,999   | ingress |
| `ingress-6-517074b.bin`   | 517,074   | ingress |
| `ingress-7-1292095b.bin`  | 1,292,095 | ingress |

16 files, ~3.3 MB total. Both directions span roughly 1.3 KB to ~0.3-1.3 MB, log-spaced,
so the bench harness (`kestrel/bench/`) sweeps codecs against small, medium, and large
real bodies rather than one size class.

## Fidelity: HIGH — real server, representative schema, both real egress code paths

**Driver:** primary capture path from the task brief — a genuine 2-process setup, not
the toy-schema embedded-test fallback:

1. A throwaway QuestDB server built from this worktree's `core` (with the dump hooks
   compiled in), booted standalone via `java -cp core/target/classes io.questdb.ServerMain`
   against a fresh data dir, listening on non-default ports (47001 HTTP/WS+QWP, 47002
   PG wire, 47003 min-http) to avoid clashing with other QuestDB instances already
   running on this shared host. `line.tcp.enabled=false`; QWP UDP stays off (default).
2. `benchmarks/src/main/java/org/questdb/QwpEgressReadBenchmark` (unmodified in the
   final tree — see Deviations) run repeatedly against that server with
   `-DrowCount=` set to 50, 150, 500, 3000, 8000, 20000, 60000 to get a real size
   spread. Each run: (a) ingests rows over ILP/WebSocket (`Sender.fromConfig("ws::...")`)
   — this is QWP ingress traffic and fires the ingress hook once per `sender.flush()`
   (twice for the 60000-row run, since it crosses the 50,000-row `auto_flush_rows`
   boundary); (b) reads the table back with `QwpQueryClient` over real QWP egress
   (warm-up + measured pass) — this fires the egress hook once per RESULT_BATCH frame
   assembled.

**Schema** (real, not toy): table `egress_bench`:
```sql
CREATE TABLE egress_bench (
  ts TIMESTAMP, id LONG, price DOUBLE, sym SYMBOL, note VARCHAR
) TIMESTAMP(ts) PARTITION BY HOUR WAL
```
`sym` cycles over 8 low-cardinality values (AAPL/MSFT/.../NFLX); `note` is a short
VARCHAR (`"n" + hex`, ≤4 chars). This is the benchmark's own "representative shape for
time-series analytics" table — the same schema this PR's docs use for egress
benchmarking, not a reduced test fixture.

**Both real egress code paths are represented**, which matters for compression
realism (see Deviations below for why this needed a second hook): the small/medium
files (1,294 - 93,392 bytes) are dominated by the coalesced
RESULT_BATCH+RESULT_END single-syscall path (`sendResultBatchAndEnd`), and the larger
files (206,881 - 423,691 bytes) include bodies from the general multi-batch streaming
path (`sendResultBatch`) once a query's result stopped fitting in one batch.

**Not yet captured / recommend before final box go/no-go:** everything here is the
5-column narrow schema. `QwpEgressReadBenchmarkWide` (15 columns, 5 of them
high-cardinality SYMBOL, in the same `benchmarks/` module) would add a wide/high-symbol-
churn shape to the corpus — worth one more capture pass before the Phase B/C
compression bake-off is treated as final, since delta-dict behavior differs materially
with high-cardinality symbols. No toy/embedded-test data is in this corpus, so no
recapture is needed on fidelity grounds alone — the wide-schema addition is the one
open gap.

## Deviations from the task brief worth flagging

- **Capture command:** the brief's `mvn exec:java` invocation on the JMH benchmark
  classes does not work (confirmed by recon before this task started — see
  `.superpowers/sdd/progress.md` line 5): these classes have `main()` methods but need
  a separately-running server, not a JMH harness. Used the corrected 2-process approach
  above instead.
- **Second egress hook, not in the original brief.** The brief specified one hook, in
  `sendResultBatch` after its `long qwpEnd = ...` line. That method is only reached
  mid-stream, when a query needs more than one batch. Every capture attempt using only
  that hook produced **zero** egress files even though queries ran and returned correct
  data — because `QwpEgressUpgradeProcessor` has a second, deliberately-duplicated
  method, `sendResultBatchAndEnd` (its own doc comment: "Used on the cursor-exhausted
  branch of streamResults so a short query ends in one syscall"), which is what actually
  ships any query whose entire result fits in one batch — i.e. most small/medium
  queries, including this benchmark's. A second one-line hook was added at the
  structurally identical point in that method (after its own
  `long qwp1End = preludeEnd + deltaSize + tableBlockSize;`, same dump call, same
  pre-compression capture point). See `../patches/dump-hooks.md` for both locations.
