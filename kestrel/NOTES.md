# Kestrel Rust/Accelerator Notes

## Rust Spike: zstd-safe 7.2.4 Sequence Producer Registration (Task 5 Step 3)

### Finding

`zstd-safe 7.2.4` **does NOT expose sequence-producer registration**.

Searched:
```
grep -rn "register_sequence_producer\|registerSequenceProducer\|SequenceProducer" \
  ~/.cargo/registry/src/*/zstd-safe-7.2.4/
```

Result: Zero matches. The library only exposes `sequence_bound()` (via `ZSTD_sequenceBound`), not the callback registration mechanism (`ZSTD_registerSequenceProducer`).

### Implication for Phase B (Rust native encoder)

**Phase B cannot use the `zstd-safe` safe wrapper for QAT-ZSTD integration.**

Options:
1. **Raw `zstd-sys` bindings**: zstd-safe re-exports `zstd_sys`, so Phase B can call `zstd_sys::ZSTD_registerSequenceProducer` directly (unsafe Rust). This is the path of least resistance.
2. **C shim wrapper**: Provide a thin C glue layer (similar to codec_qat.c in the C harness) that exposes a Rust-callable interface for QAT sequence producer registration.
3. **Upstream patch to zstd-safe**: Contribute the registration function, but timeline uncertain.

**Recommended**: Option 1 (raw zstd-sys) for Phase B — mirrors the C harness pattern and unblocks quickly.

---

## Phase 0 Status (Accelerator Availability)

All three accelerator backends are now defined in code:
- `codec_qat.c`: QAT-ZSTD (seqprod) + QAT-Deflate (QATzip)
- `codec_iaa.c`: IAA-Deflate (QPL 1.9)

Software-only build (no accelerator libs) validated on AMD box.
Accelerator compilation (`WITH_QAT=1`, `WITH_IAA=1`) validated on Intel box only.
