# QWP compression acceleration — design spec

**Status:** draft · **Date:** 2026-07-11 · **Branch:** `kestrel` (off `master`)

Evaluate, with measurements on real hardware accelerators, whether offloading the
QWP wire protocol's compression to fixed-function accelerators is worthwhile, and if so
wire the winner into QuestDB. Two codec strategies are in scope: **accelerating the
existing egress zstd** (QAT), and **offering Deflate across both egress *and* ingress**
(IAA or QAT, both directions).

The work is staged **A → B → C** so the cheap experiment (A) gates the two invasive
integrations (B, C) and supplies the parameters they need (winning codec / level /
block size). B and C are therefore designed here only at plan altitude; their exact
knobs bind to A's results.

**Scoping:** the first implementation plan covers **Phase 0 + Phase A** only (runnable
now). Phases B and C get their own plans authored *after* A reports, so their parameters
are bound to measured results rather than guessed.

---

## 1. Background — what QWP actually does (verified against the tree)

QWP = **QuestDB Wire Protocol**: a v1 binary, **columnar** protocol over WebSocket
(HTTP upgrade). *Ingress* = ingestion (client → server), *egress* = query results
(server → client). A UDP variant ("QWIP") reuses the binary message format without
WebSocket framing or compression.

Compression facts that scope this work:

- **Compression exists in exactly one place: egress query results, and it is zstd.**
  The server compresses; the client decompresses.
  - Call sites: `QwpEgressUpgradeProcessor.java:1619` and `:1717` (one-shot
    `Zstd.compress`), followed by a `Vect.memcpy` copy-back of the compressed bytes
    into the send buffer (`:1621` / `:1719`) — an extra copy on the hot path.
  - Only the post-prelude **body** is compressed; the prelude stays raw so the client
    I/O thread can route without decompressing (`FLAG_ZSTD`, `QwpConstants.java:84-91`).
  - One `ZSTD_CCtx` per connection, level fixed at creation
    (`QwpEgressProcessorState.java:943`, `zstdCCtx()`); freed in `close()`.
  - zstd native binding: `core/rust/qdbr/src/qwp_zstd.rs` (JNI, zero-copy over direct
    buffers), `zstd = "0.13"` crate → bundled **libzstd 1.5.7** (`zstd-sys
    2.0.16+zstd.1.5.7`). JNI surface: `io/questdb/std/Zstd.java`
    (`createCCtx/compress/createDCtx/decompress/freeCCtx/freeDCtx`).
- **Compression is opt-in and OFF by default.** `compressionCodec = COMPRESSION_NONE`
  unless the client sends `X-QWP-Accept-Encoding: zstd` (`QwpEgressCompressionNegotiator`).
  When enabled, the **level defaults to 1** and is clamped to `[1, 9]`
  (`QwpConstants.java:42-43`). Operator override
  `QWP_EGRESS_COMPRESSION_FORCE_LEVEL` defaults to 0 (honour client).
  No zstd dictionaries are used.
- **Fallback to raw exists at every layer** (codec default NONE; per-batch skip when
  compressed ≥ raw or on error; CCtx alloc failure → ship raw; Rust panic sentinels).
- **Ingestion is not compressed on the wire at all** — no zstd, no permessage-deflate.
  Ingress CPU is bespoke columnar *decode*: Gorilla delta-of-delta timestamps,
  varint/zigzag, symbol delta-dictionary, bit-packed booleans, then WAL append.
- **No TLS in the QWP server path.** QuestDB serves QWP over `PlainSocket`
  (`supportsTls() == false`); `wss://` is terminated by an external proxy. There is no
  in-process crypto to offload.

### The honest offload map

| QWP stage | Always-on? | Format today | Accelerator fit |
|---|---|---|---|
| Egress result **compress** | opt-in (default OFF, L1) | zstd | **QAT** seqprod (drop-in) · **IAA** *iff* a Deflate codec is added |
| Egress/ingress **serialize/decode** | **always** | Gorilla / varint / symbol-dict | none — custom formats, a SIMD problem, not accelerator-shaped |
| Ingress **compress** (client) / **decompress** (server) | — | *nothing compressed today* | **in scope:** add a Deflate codec → server-side decompress offloads to **IAA or QAT** (both do Deflate both directions) |
| **TLS** crypto | — | external proxy only | QAT-at-proxy — an ops note, not a QuestDB change |
| On-disk **Parquet** read | query-time | LZ4_RAW default (zstd/gzip optional) | reads = *decompress*: QAT decompress is Deflate-only, IAA is Deflate-only → only **gzip**-coded Parquet is offloadable; a storage-codec config, separate subsystem |

**Consequence:** the only clean accelerator target inside QWP is egress result
compression. The compelling thesis is not "make level-1 faster" (software zstd L1 is
already ~GB/s and per-descriptor offload latency can eat the win on small batches) but:

> QWP ships query results **uncompressed by default because compression costs server
> CPU**. Can an accelerator make a **high ratio** cheap enough to enable by default —
> cutting egress bytes without burning cores — and **which** accelerator fits QWP's
> small-block, delta-pre-encoded, latency-sensitive regime?

**Why Deflate is worth evaluating across both directions.** Compression has four legs,
and the *codec* choice decides which are hardware-offloadable:

| Leg | Who runs it | zstd | Deflate |
|---|---|---|---|
| Egress compress | server | ✅ QAT (seqprod) | ✅ IAA · ✅ QAT |
| Egress decompress | client | ❌ (QAT has no zstd-decompress) | ✅ IAA · ✅ QAT |
| Ingress compress | client | ✅ QAT (seqprod) | ✅ IAA · ✅ QAT |
| Ingress **decompress** | **server** | ❌ | ✅ IAA · ✅ QAT |

zstd only offloads the *compress* legs (and only via QAT). **Deflate is the single codec
that lets an accelerator offload all four legs** — crucially the **server-side ingress
decompress**, the only way to bring hardware to the write path (ingress is uncompressed
today). That is why the program evaluates a Deflate codec for egress **and** ingress, not
just QAT-accelerating the existing egress zstd.

---

## 2. Accelerator background (verified)

- **QAT → zstd** via `intel/QAT-ZSTD-Plugin` (`libqatseqprod`): registers as a zstd
  **external sequence producer** (API added in zstd 1.5.4; QuestDB bundles 1.5.7 ✅),
  offloading LZ77 match-finding to QAT for levels **L1–L12**. **Compression only** — it
  does not accelerate decompression. No dictionaries, no LDM, no stream history — which
  matches QWP's one-shot per-batch `compress2`. Intel cites up to 3.2× throughput /
  3.8× lower P99 / 3.3× perf-per-watt vs software zstd (to be re-measured here).
- **IAA → Deflate** via `intel/qpl` (`libqpl`, v1.9): Deflate **both** compress and
  decompress, plus fused decompress-and-scan. No official Rust crate — from QWP's Rust
  layer we FFI to the C `libqpl` (a small shim, mirroring the existing zstd binding).
  ClickHouse's `DEFLATE_QPL` codec is precedent for the DB pattern.
- **QAT → Deflate** via QATzip (`libqatzip`) — the QAT hardware **Deflate** engine,
  distinct from the zstd seqprod: **both** compress and decompress, standard RFC-1951
  Deflate. So Deflate can be offloaded by **either** accelerator, in **both** directions.

Key asymmetry that shapes the codec choice: **QAT's *zstd* path is compress-only
(seqprod); its *Deflate* path (QATzip) and *IAA* are both-directions.** So accelerating
the *existing* egress zstd (Phase B) only ever offloads compression; reaching the
*decompress* legs — client egress-decompress and, above all, **server-side ingress
decompress** — requires the **Deflate** codec (Phase C), which either IAA or QAT can drive.

---

## 3. Goal & success criteria

For each `codec × level × block-size` measure:

- **compression ratio** (bytes out / bytes in);
- **compress throughput** (GB/s) and, for IAA/software, **decompress throughput**;
- **per-batch latency** P50 / P99 / P99.9;
- **cores freed** = submitting-thread CPU-time vs wall-time while the accelerator works
  (the actual offload benefit);
- **perf/watt** via RAPL (`/sys/class/powercap/intel-rapl`) if readable — bonus.

**Go/no-go gate after A:** proceed to a wired integration only if some accelerator
delivers a *materially better ratio at ≤ the CPU cost of today's default level-1*, in
QWP's real block-size regime. Specifically:
- **B (QAT)** proceeds if QAT-zstd frees ≥1 core-equiv (host CPU-time drop) at a level
  whose ratio beats software L1, at representative batch sizes.
- **C (IAA)** proceeds if IAA-Deflate lands within a small ratio margin of the chosen
  zstd level while offloading both directions at lower per-op latency than QAT.

Numbers are placeholders until A establishes the software baseline; the gate is
"beats the honest software baseline on the axis that matters," not a fixed magic number.

---

## 4. Non-goals

- Accelerating the always-on columnar serialization (Gorilla/varint/symbol-dict). It is
  the largest CPU cost but is not Deflate/zstd/scan-shaped; it is a SIMD/AVX-512 problem
  and AVX-512 is not an Intel-vs-AMD differentiator. Out of scope.
- QAT-for-TLS: real, but lives at an external reverse proxy (QuestDB terminates no TLS
  in-process). Captured as a one-line ops recommendation, not a phase.
- IAA/QAT for Parquet reads: QuestDB Parquet defaults to **LZ4_RAW** (zstd/gzip optional
  via `cairo.*.parquet.compression.codec`). A query *reads* Parquet = *decompression*;
  QAT decompress is Deflate-only and IAA is Deflate-only, so **only gzip-coded Parquet is
  offloadable** (already a config switch, no code). The real prize there is IAA *fused
  decompress-and-scan* + gzip's ratio, traded against gzip's slower decode vs the LZ4
  default — a **storage** opportunity, separate from the QWP protocol; deserves its own
  spec. Note QAT *can* also accelerate zstd-coded Parquet **writes** via the same
  sequence-producer as Phase B (a cheap potential extension, different call site in
  `parquet_write/jni.rs`).

---

## 5. Phases

### Phase 0 — Detection probe (read-only)

A single read-only script the operator runs on the box. Confirms: CPU generation
(SPR/EMR/GNR), IAA devices + `accel-config` work-queue **enabled** state, QAT devices +
`adf_ctl` **up** + VFs, libraries (`libqatzip` / `libqpl` / `libaccel-config` /
`libzstd` ≥ 1.5.7), and the build toolchain (JDK, Maven, cargo, cc, cmake). Output
tells us what must be configured/installed before building A, B, or C on the box.

### Phase A — Codec bake-off harness (the de-risking core)

A standalone harness that runs the **captured real egress *and* ingress bodies** (see §6)
through all codecs, **both directions**, and reports the §3 metrics into a CSV + printed
table.

- **Codecs under test** (identical input buffers):
  - `sw-zstd` — libzstd 1.5.7, levels 1/3/6/9, one-shot `ZSTD_compress2` with a reused
    CCtx (mirrors QWP exactly);
  - `qat-zstd` — same libzstd + `libqatseqprod` registered via
    `ZSTD_registerSequenceProducer`, `ZSTD_c_enableSeqProducerFallback=1`, levels 1–12
    (compress only — the seqprod cannot decompress);
  - `iaa-deflate` — QPL hardware path (`qpl_path_hardware`, auto-fallback for control),
    compress **and** decompress, fixed vs dynamic Huffman;
  - `qat-deflate` — QATzip hardware Deflate engine, compress **and** decompress, so we
    learn which accelerator wins Deflate in each direction;
  - `sw-deflate` — libdeflate/zlib control at matching levels, to separate *format*
    (Deflate vs zstd) from *hardware* (accelerator vs CPU).

  Each codec is measured **both directions** over **both** corpora — egress (server
  compresses / client decompresses) and ingress (client compresses / **server
  decompresses** — the leg that matters most for server CPU).
- **Sweeps:** block size (8K/16K/32K/64K/128K and whole-body — this decides whether the
  accelerators' per-descriptor latency pays off); thread concurrency (device
  saturation, since accelerators have finite engines/queues).
- **Correctness:** every codec round-trips (compress → decompress → byte-compare)
  before any timing run.
- **Language:** **C** (every codec library exposes a C API; zero binding friction; pure
  measurement). The harness is throwaway measurement code, not product code.
- **Rust spike (de-risks B):** a tiny separate check of whether `zstd-safe 7.2.4`
  exposes sequence-producer registration, or whether B must call the raw symbol via
  `zstd-sys` (experimental) or a small C shim.

### Phase B — Wire the winner (QAT) into egress

Bind the winning level/config from A. In `core/rust/qdbr/src/qwp_zstd.rs`: after CCtx
creation and level-set, register a QAT seqprod state (`QZSTD_createSeqProdState`) via
`ZSTD_registerSequenceProducer` with the fallback param, behind an env/config flag,
with graceful software fallback if device init fails (matching QWP's existing "ship raw
on failure" posture); free the state in `freeCCtx`. Link `libqatseqprod` / `libqatzip`
into the `libquestdbr` build (`build.rs` / Cargo). Flip `QwpEgressReadBenchmark` off
`compression=raw` to `zstd;level=<A-winner>` and measure QAT-on vs off: server CPU,
egress MiB/s, latency. Stretch: assess eliding the compress copy-back (`:1621`/`:1719`).

### Phase C — Deflate codec for QWP egress **and** ingress

Add a negotiated `deflate` codec (hardware-backed by IAA or QAT per the box, software
fallback) across **both** directions, via a new `Deflate` JNI module in `qdbr`
(mirroring `qwp_zstd.rs`: `deflateCompress` / `deflateDecompress` over QPL or QATzip,
hardware path with auto-fallback):
- **Egress:** add `deflate` to `QwpEgressCompressionNegotiator` + a new `FLAG_*` bit;
  server compresses the result body (offloaded), client decompresses.
- **Ingress:** negotiate `deflate` as an ingress content-encoding (today the handshake
  passes `null`) and **decompress server-side via IAA/QAT** before the columnar decode.
  This is the leg zstd can never offload and the main reason to add Deflate — it brings
  hardware to the write path.

Measure the two **server** legs (egress compress, ingress decompress) — where the CPU
offload actually lands — plus standalone round-trips for correctness. **Known
limitation:** the QWP client is a shaded/relocated artifact with no source in this tree,
so full end-to-end *client*-side encode/decode of `deflate` may not be exercisable here;
the server legs are the measurable surface, which is exactly what we care about.

---

## 6. Corpus capture (fidelity-critical)

The compressor must see **real post-serialization QWP bodies — egress *and* ingress**,
because QWP delta-encodes (Gorilla/symbol-dict) *before* any byte-compressor, feeding it
high-entropy input — raw CSV would overstate ratios and mislead the whole go/no-go.

- **Method (egress):** a throwaway dump hook at the pre-compress point in
  `QwpEgressUpgradeProcessor` (the `preludeEnd..bodyLen` buffer) writes each result body,
  driven by `QwpEgressReadBenchmark` / real queries.
- **Method (ingress):** a matching hook in the ingress path (`QwpIngressProcessorState` /
  `addData`) captures the raw columnar payloads the client sends (uncompressed today) —
  what a Deflate ingest codec would compress and the **server** would decompress. Driven
  by `QwpSenderBenchmark` / `QwpEquitiesL1Benchmark`.
- **Where captured:** on the development box (the bodies are byte-identical regardless of
  CPU — it is just serialized data), so the corpus is generated here and **shipped with
  the code via git**; the accelerator box only builds + runs the bench over the `.bin`
  files. Corpus kept to a handful of representative batches per shape to stay
  git-friendly (tarball/LFS only if it grows).
- **Shapes:** narrow L1 quotes (ts, symbol, bid/ask doubles, volume long), wide 15-col,
  and a high-cardinality-symbol case. Also dump the **raw** (pre-Gorilla) columns as a
  control, to quantify how much pre-encoding starves the compressor.

---

## 7. Transport & build model ("you run commands")

No SSH to the box; git is the transport and the operator runs everything.

1. All artifacts (spec, dump-hook patch, corpus, C harness, build/run scripts, and later
   the B/C patches) live on the anonymised `kestrel` branch.
2. Changes are **committed locally on the branch**; the **operator pushes** to their
   chosen remote (the hub's `origin` is the public `questdb/questdb`, so the push target
   and branch naming are deliberately the operator's call — committed content is kept
   neutral/technical for that reason). The box then clones/fetches the branch.
3. Each phase is a small set of **copy-pasteable** scripts that print labelled results;
   no interactive access is needed.
4. **Box build dependency:** A's `qat-zstd`/`iaa-deflate`/`qat-deflate` and all of B/C must be built
   **on the box** (the accelerator libraries and hardware exist only there). So the box
   needs a C toolchain + QAT/QPL dev libraries for A, and the full QuestDB build
   toolchain (JDK/Maven/cargo/cc/cmake) for B/C. Phase 0 detects these; a setup script
   fills gaps.

---

## 8. Risks & open questions

- **Block-size regime is the crux.** If real egress batches are small (few KB), QAT's
  per-descriptor latency may make it net-negative; IAA's lower submission latency may
  win, or neither may beat software L1. A's block sweep resolves this empirically — this
  is *why* A gates B/C.
- **Default level 1.** The interesting result may be "level-9 ratio at ≤ level-1 CPU,"
  reframing the product question as "enable compression by default," not "speed up the
  current path." The bench must report ratio *and* CPU together, not throughput alone.
- **Ingress compression is a client-CPU trade.** Adding Deflate on ingest spends *client*
  CPU to compress in exchange for wire bytes plus an offloadable *server* decompress. On a
  LAN the wire saving may not justify it; the win concentrates on constrained links and on
  freeing server cores — A must report the ingress numbers so this is a data-backed call.
- **Pre-encoding starves the compressor.** Gorilla/symbol-dict output is already
  low-redundancy; marginal compression (and thus the value of accelerating it) may be
  modest. The raw-column control quantifies this.
- **`zstd-safe` API exposure** for sequence-producer registration (B) — mitigated by the
  Rust spike in A.
- **QPL Rust binding** (C) — no official crate; small C-FFI shim required.
- **Shaded client** — limits end-to-end `deflate` client-side validation (C).
- **Accelerator config on the box** — IAA work queues must be *enabled* and QAT VFs
  *up*; Phase 0 catches an unconfigured device before we waste a build.

---

## 9. Deliverables

- **Phase 0:** `kestrel/scripts/probe.sh` + an interpretation of its output.
- **Phase A:** `kestrel/bench/` (C harness + Makefile), the egress+ingress dump-hook
  patches, captured `kestrel/corpus/*.bin`, `kestrel/scripts/run-bench.sh`, a results
  CSV, and a written **go/no-go** with the software baseline.
- **Phase B:** `qwp_zstd.rs` + build-wiring patch, benchmark run, results.
- **Phase C:** `deflate` codec patch set (egress+ingress) + `Deflate` JNI module
  (IAA/QAT), benchmark run, results.

---

## 10. References

- QAT zstd plugin: https://github.com/intel/QAT-ZSTD-Plugin
- Intel QPL: https://github.com/intel/qpl
- zstd sequence-producer API: libzstd ≥ 1.5.4 (`ZSTD_registerSequenceProducer`)
- QWP source: `core/src/main/java/io/questdb/cutlass/qwp/**`, `core/rust/qdbr/src/qwp_zstd.rs`
