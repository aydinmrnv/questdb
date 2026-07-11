#!/usr/bin/env bash
# kestrel Phase 0 — Intel IAA/QAT readiness probe. READ ONLY (no writes/config).
# Run once as your user; re-run the QAT/IAA sections with sudo if they print empty/permission.
set -u
h(){ printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }

h "CPU / generation"
grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-
lscpu 2>/dev/null | grep -iE '^Model name|^CPU\(s\)|^Thread|^Socket|^NUMA node\(s\)|^Stepping'
printf 'accel ISA: '
grep -m1 -oE '\b(amx_tile|amx_bf16|amx_int8|avx512f|movdir64b|enqcmd|movdiri)\b' /proc/cpuinfo | sort -u | tr '\n' ' '; echo

h "Kernel / IOMMU"
uname -r
iommu=$(ls -d /sys/class/iommu/* 2>/dev/null); if [ -z "$iommu" ]; then echo "(no iommu class dirs)"; else printf '%s\n' "$iommu" | head; fi

h "Accelerator PCI devices (QAT 494x / IAA 0cfe / DSA 0b25)"
if command -v lspci >/dev/null; then
  lspci -nnD 2>/dev/null | grep -iE '8086:(494[0-7]|0cfe|0b25)' || echo "(none matched)"
else echo "lspci not installed (pciutils)"; fi

h "IAA / DSA config (accel-config)"
if command -v accel-config >/dev/null; then
  accel-config version 2>/dev/null
  accel-config list 2>/dev/null | grep -iE '"dev"|"type"|"state"|"max_work_queues"|"engines"' \
    || echo "(list empty — needs sudo, or no device enabled)"
else echo "accel-config NOT installed"; fi
nodes=$(ls /dev/iax* /dev/dsa* 2>/dev/null); [ -n "$nodes" ] && printf '%s\n' "$nodes" || echo "(no /dev/iax|dsa nodes)"

h "QAT config"
command -v adf_ctl >/dev/null && { adf_ctl status 2>/dev/null || echo "(adf_ctl needs sudo)"; } \
  || echo "adf_ctl NOT installed (qatlib not set up)"
nodes=$(ls /dev/qat_* /dev/vfio/* 2>/dev/null); [ -n "$nodes" ] && printf '%s\n' "$nodes" || echo "(no /dev/qat_* or vfio nodes)"
lsmod 2>/dev/null | grep -iE 'qat|idxd|iaa|dsa|uacce' || echo "(no accelerator modules loaded)"

h "Accelerator + codec libraries"
ldconfig -p 2>/dev/null | grep -iE 'libqatzip|libqatseqprod|libqat\.|libusdm|libqpl|libaccel-config|libzstd|libdeflate' \
  || echo "(none of qatzip/qatseqprod/qpl/accel-config/zstd/deflate in ldconfig)"
command -v zstd >/dev/null && zstd --version || echo "zstd: not installed (cli)"

h "Build toolchain"
for t in "java -version" "mvn -version" "cargo --version" "cc --version" "cmake --version" "make --version"; do
  out=$($t 2>&1 | head -1); printf '%-16s %s\n' "${t%% *}:" "${out:-MISSING}"
done

h "READINESS HINTS"
echo "- qat-zstd/qat-deflate need: a QAT device 'state: up' + libqatzip (+ we build QAT-ZSTD-Plugin)."
echo "- iaa-deflate needs: an accel-config device 'state: enabled' + an enabled work queue + libqpl."
echo "- If QAT/IAA sections were empty, re-run with: sudo bash $0"
