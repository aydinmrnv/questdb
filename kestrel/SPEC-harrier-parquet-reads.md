# Accelerated Parquet reads (codename **harrier**) — design spec

**Status:** draft · **Date:** 2026-07-11 · **Relationship:** follow-on to `kestrel`
(the QWP-wire spec). Separate subsystem — gets its own branch/worktree off `master` at
execution time; the design doc lives with `kestrel` for now.

`kestrel` accelerates the **wire** (QWP compression). `harrier` accelerates the **scan**:
offload QuestDB's Parquet-backed query read path — column-page **decompression**, and
ideally **fused decompress-and-scan** — to Intel IAA/QAT. Together they cover a query
end to end (read decode → wire egress).

---

## 1. Why this is a real, separate opportunity

A query over a Parquet-backed partition **reads = decompresses** column pages, then scans
/ filters them. That decode is a per-query CPU cost that fixed-function accelerators can
take — **but only for gzip (Deflate)-coded Parquet**, because IAA and QAT both decompress
Deflate only (not zstd, not LZ4). QuestDB's Parquet codec is configurable and this is
already reachable with **no code change**:

- Codec enum `ParquetCompression`: `UNCOMPRESSED / SNAPPY / GZIP / BROTLI / ZSTD /
  LZ4_RAW`; write default is **`LZ4_RAW`** (`PropServerConfiguration` — both
  `cairo.parquet.export.compression.codec` and
  `cairo.partition.encoder.parquet.compression.codec` default `LZ4_RAW`). Setting either
  to **`GZIP`** produces Deflate-coded columns.
- Read path is `parquet2` with the `gzip` feature (`flate2`), so gzip Parquet already
  decodes today.

### Verified integration seams

- **Decompress offload point:** `core/rust/parquet2/src/compression.rs:134`
  `pub fn decompress(compression, input_buf, output_buf)` — the central per-page codec
  dispatch — and the qdbr wrappers `decompress_data_page` / `decompress_sliced_data` /
  `decompress_buffer` in `core/rust/qdbr/src/parquet_read/decode_column.rs`. Replacing the
  Deflate branch with an IAA/QAT call is a contained change.
- **Fused-scan seam already exists:** `core/rust/qdbr/src/parquet_read/mod.rs:94-114`
  defines `FILTER_OP_EQ/LT/LE/GT/GE/IS_NULL/IS_NOT_NULL/BETWEEN` and `ColumnFilterPacked`;
  `decode_column.rs` has a `decode_page_filtered` path. **QuestDB already pushes column
  predicates into the Parquet reader.** IAA's fused *decompress + scan* (one descriptor:
  Deflate-decompress a page and emit a match bitmap) maps directly onto this existing
  filtered-decode seam — the predicate representation is already there.

---

## 2. Accelerator fit

- **IAA (QPL):** Deflate **decompress** (both directions) **plus fused
  decompress-and-filter** (scan / extract / select → bitmap). The fused op is the prize —
  it decompresses and applies the predicate in one pass, skipping materialization of
  filtered-out rows. IAA scan is fixed-width/integer-oriented, so it fits QuestDB's most
  common filters best: **timestamp ranges, int/long comparisons, symbol-id equality**
  (`BETWEEN`/`LT`/`GT`/`EQ` on fixed-width columns). Double/varchar predicates fall back
  to decode-then-filter on CPU.
- **QAT (QATzip):** hardware Deflate **decompress** — decode offload only, no fused scan.
  Useful when the goal is simply freeing cores from decompression under load.
- **Neither** helps `LZ4_RAW` (default) or `ZSTD` Parquet — gzip is the enabling codec.

---

## 3. The honest question (why this might *not* pay off)

`LZ4_RAW` (the default) already decompresses at ~GB/s on the CPU. If a query is
**decode-bound**, switching to gzip makes CPU decode *slower*, and an accelerator would be
clawing back a cost we introduced. The wins only materialise in specific regimes, and the
bench must show which one QuestDB is in:

1. **Fused decompress+scan at low selectivity** — few rows pass the predicate, so skipping
   materialization is a large saving (IAA only).
2. **IO-bound scans** — gzip's better ratio shrinks bytes read (cold cache, object-store /
   tiered storage, big historical scans); decode offload keeps CPU free while IO dominates.
3. **Concurrency** — many parallel queries; offloading decode frees cores for the query
   engine.

If none hold (small hot LZ4 partitions, CPU-cheap decode, high selectivity), harrier is a
no-op and we say so. This is why HA is a go/no-go gate, exactly like kestrel's Phase A.

---

## 4. Phases

- **H0 — Detection.** Reuse the `kestrel` Phase-0 probe (same IAA/QAT stack).
- **HA — Micro-bench (go/no-go).** Over **real QuestDB gzip-Parquet column pages**
  (captured by writing a representative table with `...compression.codec=GZIP` and dumping
  page buffers), compare: `sw-flate` vs `iaa-deflate` vs `qat-deflate` **decode**
  (GB/s, cores freed); and **decompress-then-scan (CPU)** vs **IAA fused decompress+scan**
  on a timestamp/int column, swept across **selectivity** and page/row-group size. Also
  record the `LZ4_RAW`-decode baseline so the comparison is honest. Reuses the kestrel C
  harness + methodology.
- **HB — Decode offload.** Wire IAA/QAT Deflate decompress into the page-decompress seam
  (the Deflate branch of `parquet2::decompress` / the qdbr `decompress_data_page`
  wrappers), config-gated with CPU fallback. Benchmark a real `SELECT … WHERE` over a
  gzip-Parquet partition vs CPU-gzip **and vs the LZ4_RAW baseline**.
- **HC — Fused decompress+scan (stretch, the prize).** Map `ColumnFilterPacked` /
  `FILTER_OP_*` onto QPL's scan predicate at the `decode_page_filtered` seam, for
  fixed-width columns (timestamp/int/symbol-id); fall back to decode-then-filter for
  unsupported types/ops. Benchmark low vs high selectivity to locate the crossover.

---

## 5. Risks & open questions

- **Decode-bound vs IO-bound** — determines whether decode offload matters or only ratio.
  HA answers empirically before any wiring.
- **gzip-vs-LZ4 regression** — forcing gzip must not make the common CPU path a net loss;
  HB always includes the LZ4 baseline.
- **IAA scan type/width coverage** — fixed-width int/timestamp are in; double/varchar are
  limited; HC scopes to supported types with CPU fallback.
- **Per-page offload overhead vs page size** — Parquet page / row-group sizing interacts
  with per-descriptor latency; HA sweeps it.
- **Page-cache / mmap interaction** — where decompressed output lands relative to
  QuestDB's mapped partition buffers (avoid an extra copy).

---

## 6. Scope

Storage subsystem, distinct from the QWP protocol. Shares the detection probe, the
accelerator software stack, and the bake-off methodology with `kestrel`, but ships on its
own branch. Not a dependency of `kestrel`; complementary — kestrel frees the wire, harrier
frees the scan.

---

## 7. References

- Intel QPL (IAA, incl. fused decompress+filter): https://github.com/intel/qpl
- QATzip (QAT Deflate): https://github.com/intel/QATzip
- Seams: `core/rust/parquet2/src/compression.rs:134`,
  `core/rust/qdbr/src/parquet_read/decode_column.rs`,
  `core/rust/qdbr/src/parquet_read/mod.rs:94-114` (`ColumnFilterPacked`, `FILTER_OP_*`)
