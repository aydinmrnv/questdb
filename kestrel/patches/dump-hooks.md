# Throwaway dump-hook patch (Task 4 corpus capture)

**Status: THROWAWAY. Must be dropped before Phase B/C (real accelerator integration
work on the live egress/ingress path).** This patch exists only to capture real QWP
wire bodies into `kestrel/corpus/` for the compression bake-off harness
(`kestrel/bench/`). It adds file-I/O and an `AtomicInteger`-gated counter into two
hot server paths and must not ship.

## Files touched

### 1. `core/src/main/java/io/questdb/std/KestrelDump.java` — new file, entirely throwaway

Env/property-gated dump helper. No-ops unless `KESTREL_DUMP_DIR` is set (as a system
property or env var — checked both ways so it reaches JVMs where only one is easy to
set, e.g. a forked test JVM). Writes at most `KESTREL_DUMP_MAX` (default 64) files per
direction as `<dir>/<egress|ingress>-<NNN>.bin`.

### 2. `core/src/main/java/io/questdb/cutlass/qwp/server/egress/QwpEgressUpgradeProcessor.java` — 2 one-line hooks

The brief called for one hook, in `sendResultBatch`. A second was required — see
`../corpus/MANIFEST.md` ("Deviations") for why: `sendResultBatch` is only reached for
non-final batches of a multi-batch stream, and a **separate, deliberately-duplicated**
method, `sendResultBatchAndEnd`, ships every query whose result fits in a single batch
(the common case for small/medium queries). Without the second hook, capture produced
zero egress files.

- Line 1605, inside `sendResultBatch(...)`, immediately after
  `long qwpEnd = preludeEnd + deltaSize + tableBlockSize;`:
  ```java
  if (io.questdb.std.KestrelDump.on()) io.questdb.std.KestrelDump.dump("egress", preludeEnd, (int) (qwpEnd - preludeEnd));
  ```
- Line 1712, inside `sendResultBatchAndEnd(...)`, immediately after
  `long qwp1End = preludeEnd + deltaSize + tableBlockSize;`:
  ```java
  if (io.questdb.std.KestrelDump.on()) io.questdb.std.KestrelDump.dump("egress", preludeEnd, (int) (qwp1End - preludeEnd));
  ```

Both fire before the optional zstd-compression step in their respective methods, so
the dumped bytes are always the uncompressed post-serialization body — the thing the
bake-off harness needs to compress itself.

### 3. `core/src/main/java/io/questdb/cutlass/qwp/server/QwpIngressUpgradeProcessor.java` — 1 one-line hook

- Line 840, first statement inside
  `handleBinaryMessage(HttpConnectionContext context, QwpIngressProcessorState state, long payload, int length)`:
  ```java
  if (io.questdb.std.KestrelDump.on()) io.questdb.std.KestrelDump.dump("ingress", payload, length);
  ```

## How to drop these before Phase B/C

If this commit is still the tip of the branch (or no later commit has touched any of
the three files above), the clean way is:

```bash
git log --oneline --grep "capture real QWP egress/ingress corpus"   # find the sha
git revert --no-edit <that-sha>
```

Otherwise (later commits have touched these files), remove by hand:

```bash
git rm core/src/main/java/io/questdb/std/KestrelDump.java
grep -rn "KestrelDump" core/src/main/java/io/questdb/cutlass/qwp/server/ \
  # delete each matching line (3 total: 2 in egress/QwpEgressUpgradeProcessor.java, 1 in QwpIngressUpgradeProcessor.java)
```

Every inserted line is self-contained (`if (io.questdb.std.KestrelDump.on()) ...;`, no
multi-line braces, nothing else on the line) and greppable by the string
`KestrelDump`, so hand-removal is a one-line delete per hit with no follow-on edits
needed. After removing, rebuild core (`mvn -pl core -am -Dmaven.test.skip=true package`)
to confirm the tree still compiles clean without the throwaway class.

## Capture environment notes (for anyone re-running this)

- The throwaway server must be launched with the same JVM module-access flags real
  QuestDB launch scripts use, or every `Worker` thread fails at startup with
  `IllegalAccessError: ... cannot access class jdk.internal.vm.ContinuationScope`
  (silent-ish: the server still binds its ports, but WAL-apply/query/write workers are
  all dead, so nothing actually works). Minimum flags for a plain
  `-cp core/target/classes io.questdb.ServerMain` launch:
  ```
  --sun-misc-unsafe-memory-access=allow --enable-native-access=ALL-UNNAMED
  --add-opens=java.base/java.lang=ALL-UNNAMED
  --add-opens=java.base/java.lang.reflect=ALL-UNNAMED
  --add-opens=java.base/java.nio=ALL-UNNAMED
  --add-opens=java.base/java.time.zone=ALL-UNNAMED
  --add-exports=java.base/jdk.internal.vm=ALL-UNNAMED
  ```
- This host runs several other QuestDB instances on the default ports (9000/8812/9003).
  Point the throwaway server at free ports instead of killing anything: drop a
  `conf/server.conf` in its data dir before first boot with
  `http.net.bind.to=`, `http.min.net.bind.to=`, `pg.net.bind.to=` overrides (and
  `line.tcp.enabled=false` since ILP/TCP isn't needed for QWP capture).
- Background the server with the harness's own backgrounding (e.g. a bash tool's
  `run_in_background`), not a hand-rolled `nohup ... & disown` — the latter did not
  survive the tool-call boundary in this environment and the server was SIGTERM'd
  within ~1s of reaching ready state on the first attempt.
