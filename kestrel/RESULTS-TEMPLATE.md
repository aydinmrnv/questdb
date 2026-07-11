# kestrel Phase A Results & Go/No-Go

## Box Configuration

- **Box**: <CPU model / socket / NUMA config>
- **QAT Device**: <PCI ID and state from `adf_ctl status`, if present>
- **IAA / DSA Config**: <accel-config device state and WQ count, if present>
- **Kernel / IOMMU**: <kernel version and IOMMU support>

## Software Baseline (sw-zstd L1)

Reference point: **sw-zstd level 1** on the largest single corpus (e.g., egress-7 or ingress-7):

- **Compress**:
  - Ratio: __ (e.g., 0.27)
  - Throughput: __ MB/s
  - p99: __ us
  
- **Decompress**:
  - Throughput: __ MB/s
  - p99: __ us

## Egress (Server Compresses)

### Best QAT-zstd

Compare qat-zstd against sw-zstd L1 baseline:

- **Codec**: qat-zstd
- **Level**: __ (typically 3 or 6; may differ from sw-zstd L1 for better throughput/ratio trade)
- **Ratio**: __ (should match sw-zstd at same level; if not, compression error or different codec)
- **Throughput (compress)**: __ MB/s (vs sw-zstd: __ MB/s = __x speedup)
- **Cores Freed**: __ (vs sw-zstd: ~0.0)
- **p99 (compress)**: __ us

### Best IAA-deflate or QAT-deflate

Deflate is a different codec family (egress-specific):

- **Codec**: iaa-deflate [or qat-deflate]
- **Ratio**: __ (expect ~0.30–0.45 for JSON/text)
- **Throughput (compress)**: __ MB/s
- **Cores Freed**: __
- **Notes**: deflate-vs-zstd trade-off (ratio worse, throughput better?), decoder throughput at ingress

## Ingress (Server Decompresses)

### IAA-deflate or QAT-deflate Decompression

The **ingress leg is critical**: zstd cannot offload decompression to accelerators in Phase A (no uarc support). Deflate via IAA can.

- **Codec**: iaa-deflate [or qat-deflate]
- **Decompress throughput**: __ MB/s
- **p99 (decompress)**: __ us
- **Cores Freed**: __ (zstd decompress: ~0.0 for reference)

## Decision

### B (QAT into egress zstd)

QAT-zstd for egress compression; server handles decompression in software.

- [ ] **GO** — reason: ____
  - Speedup: __ vs sw-zstd L1
  - Cores freed: __
  - Viable for production egress offload

- [ ] **NO** — reason: ____
  - Not enough speedup (< 1.5x? < 2x?)
  - Cores freed marginal (< 0.2?)
  - Ratio penalty too high
  - Other: ____

### C (Deflate codec egress + ingress)

Deflate for both directions; potential IAA acceleration on ingress decompression.

- [ ] **GO** — reason: ____
  - Egress ratio acceptable (≤ 0.40?)
  - Egress throughput acceptable
  - **Ingress decompress speedup** significant (> 1.5x sw-zstd decompress?)
  - Cores freed meaningful (> 0.3?)
  - Overall latency & throughput SLA met

- [ ] **NO** — reason: ____
  - Egress ratio too high (compression factor bad)
  - Ingress decompress not accelerated (no IAA device / QPL not available)
  - Ingress decompress throughput worse than sw-zstd
  - Latency SLA not met
  - Other: ____

## Winning Codec & Parameters (if GO)

If either B or C was marked GO:

- **Chosen codec**: [qat-zstd | iaa-deflate | qat-deflate | sw-zstd + qat-zstd | deflate+IAA decompress]
- **Egress**: codec __, level __, block size __
- **Ingress**: codec __, level __, block size __
- **Ratio vs sw-zstd L1**: __
- **Throughput gain (compress)**: __x
- **Throughput gain (decompress, if applicable)**: __x
- **Cores freed**: __
- **Next steps**: wire the winning codec into the QWP egress compression path (Phase B)

## Raw Results

Paste `results.csv` here or attach as artifact:

```
[results.csv contents or link to artifact]
```

### How to Filter/Sort

```bash
# Just sw-zstd (baseline):
grep 'sw-zstd,' results.csv | column -s, -t

# Just qat-zstd egress (compress):
grep 'qat-zstd,.*,compress' results.csv | column -s, -t

# Just iaa-deflate (both directions):
grep 'iaa-deflate,' results.csv | column -s, -t

# Best throughput within qat-zstd compress:
grep 'qat-zstd,.*,compress' results.csv | sort -t, -k7,7nr | head -5 | column -s, -t
```

## Notes

- **Ratio**: compressed size / original size; lower is better; should be stable across directions (compress != decompress doesn't change ratio).
- **throughput_MBps**: uncompressed bytes per second; higher is better.
- **p50_us / p99_us / p999_us**: latency percentiles in microseconds; lower is better.
- **cpu_frac**: CPU time / wall time; ~1.0 = full cores busy; < 1.0 = idle/stalled.
- **cores_freed**: 1.0 - cpu_frac; offload effectiveness; software codecs ≈ 0.0; accelerated ≈ 0.3–0.8.
- **direction**: "compress" or "decompress"; filter by this when comparing codec pairs.
- **block**: "0" means whole-buffer; other values are block sizes (8192, 16384, etc.). Typically best throughput at a mid-range block size; whole-buffer might be worse (cache effects).
