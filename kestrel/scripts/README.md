# kestrel Phase A: Operator Runbook

## Prerequisites & Setup

1. **Clone/fetch the kestrel branch** on the Intel box (must have real corpus data):
   ```bash
   git clone <repo> kestrel
   cd kestrel/kestrel
   ```

2. **Probe the box** (read-only, no sudo needed initially):
   ```bash
   bash scripts/probe.sh
   ```
   - If QAT/IAA sections are empty, re-run with `sudo bash scripts/probe.sh` to check device state & permissions
   - Note: the output shows available accelerators, ISA support, dev libraries, and toolchain

3. **Install any missing dev libraries** based on probe output:
   - For QAT: `libqatzip-dev libqatseqprod-dev libqat-dev libusdm-dev`
   - For IAA: `libqpl-dev libaccel-config-dev`
   - For codecs: `libzstd-dev libdeflate-dev`
   - Example (Debian/Ubuntu):
     ```bash
     sudo apt-get install -y libzstd-dev libdeflate-dev libqpl-dev libaccel-config-dev libqatzip-dev libqatseqprod-dev libqat-dev libusdm-dev
     ```

## Running the Benchmark

4. **Build the benchmark harness** (auto-detects accelerator libs):
   ```bash
   bash scripts/build-bench.sh
   ```
   - Prints `<lib> absent → skipping` for any accelerators not installed
   - Runs selftest to validate codec implementations
   - Builds `bench/bench` binary

5. **Run the benchmark sweep** over the corpus:
   ```bash
   bash scripts/run-bench.sh
   ```
   - Sweeps all codec × level × block × direction (compress/decompress) combinations
   - Produces `bench/results.csv` with schema: `codec,corpus,direction,level,block,ratio,throughput_MBps,p50_us,p99_us,p999_us,cpu_frac,cores_freed`
   - Prints a readable sorted table on stdout

6. **Analyze results** using the template:
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
cd ~/kestrel/kestrel
bash scripts/probe.sh | tee probe-output.txt  # note what's available
# ... install missing libs ...
bash scripts/build-bench.sh
bash scripts/run-bench.sh | tee run-output.txt
# ... review results.csv & fill RESULTS-TEMPLATE.md ...
git add RESULTS-TEMPLATE.md results.csv
git commit -m "Phase A results on <box-name>"
git push
```
