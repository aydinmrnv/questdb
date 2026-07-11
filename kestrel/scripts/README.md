# kestrel Phase A: Operator Runbook

## Prerequisites & Setup

1. **Clone/fetch the kestrel branch** on the Intel box (must have real corpus data):
   ```bash
   git clone <repo-url> questdb   # repo root is the QuestDB tree
   cd questdb/kestrel             # kestrel/ is this project's subdir
   ```

2. **Probe the box** (read-only, no sudo needed initially):
   ```bash
   bash scripts/probe.sh
   ```
   - If QAT/IAA sections are empty, re-run with `sudo bash scripts/probe.sh` to check device state & permissions
   - Note: the output shows available accelerators, ISA support, dev libraries, and toolchain

3. **Install the codec dev libraries** (needed for the software baseline):
   ```bash
   sudo apt-get install -y libzstd-dev libdeflate-dev
   ```
   `libaccel-config`, `libqat`, and `libusdm` ship with the distro accel-config /
   qatlib runtime; install their `-dev` variants only if the linker can't find the
   unversioned `.so`.

4. **Accelerator libraries are build-from-source** (there are no `libqpl-dev` /
   `libqatzip-dev` / `libqatseqprod-dev` distro packages):
   - **IAA — `libqpl`** (`intel/qpl`), for `iaa-deflate`. Deps `build-essential cmake nasm`:
     ```bash
     git clone --recursive https://github.com/intel/qpl.git
     cd qpl && mkdir build && cd build
     cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. && cmake --build . -j
     sudo cmake --build . --target install && sudo ldconfig
     ```
   - **QAT — `libqatzip`** (`intel/QATzip`) and **`libqatseqprod`** (`intel/QAT-ZSTD-Plugin`),
     for `qat-deflate` / `qat-zstd`. Need a configured qatlib/qatmgr and, for `qat-zstd`,
     zstd ≥ 1.5.6 (point the Makefile at it with `ZSTD_PREFIX=`). See each project's README.

## Running the Benchmark

5. **Build the benchmark harness** (auto-detects accelerator libs):
   ```bash
   bash scripts/build-bench.sh
   ```
   - Prints `<lib> absent → skipping` for any accelerators not installed
   - Runs selftest to validate codec implementations
   - Builds `bench/bench` binary

6. **Run the benchmark sweep** over the corpus:
   ```bash
   bash scripts/run-bench.sh
   ```
   - Sweeps all codec × level × block × direction (compress/decompress) combinations
   - Produces `bench/results.csv` with schema: `codec,corpus,direction,level,block,ratio,throughput_MBps,p50_us,p99_us,p999_us,cpu_frac,cores_freed`
   - Prints a readable sorted table on stdout

7. **Analyze results** using the template:
   ```bash
   cat RESULTS-TEMPLATE.md
   ```
   - Fill in the go/no-go template with your results
   - Paste `results.csv` into the decision document
   - Each codec has a **compress row** and **decompress row** (where supported)

## Key Metrics

- **ratio**: compressed_bytes / original_bytes (lower is better)
- **throughput_MBps**: compression/decompression speed in MB/s (higher is better)
- **p99_us**: 99th percentile latency in microseconds (lower is better)
- **cores_freed**: (1 - cpu_frac), how many cores are freed vs reference (higher is better; software codecs ≈ 0)
- **direction**: "compress" or "decompress" — compare pairs within each codec

## Example Flow

```bash
# On the Intel box:
cd ~/questdb/kestrel
bash scripts/probe.sh | tee probe-output.txt  # note what's available
# ... install missing libs ...
bash scripts/build-bench.sh
bash scripts/run-bench.sh | tee run-output.txt
# ... review results.csv & fill RESULTS-TEMPLATE.md ...
git add RESULTS-TEMPLATE.md results.csv
git commit -m "Phase A results on <box-name>"
git push
```
