# Paired Z24 stencil observations

`src/amd/r300/common/tests/r300_zb_stencil_pair.py` classifies the raw
before/after images from `z24-linear-seed-5a` and `z24-linear-seed-a5`.
The two scenarios are defined by `r300_zb_depth_discovery.c`.

## Input contract

Use the measure arm from each scenario. The tool independently states the
fixed linear experiment: a 24,576-byte allocation, storage at byte 2,048,
65 rows of 256 bytes, and 2,048 guard bytes on either side of storage.
Storage occupies `[2048,18688)`, the suffix guard ends at 20,736, and
3,840 unclaimed bytes remain. Every initial non-storage byte is `0xa3`.

Each initial storage word carries depth code `0x800000` and its declared
stencil seed. A depth change must occur exactly once, at
`2048 + 21*256 + 37*4 = 7572`, and leave code `0x400000` there. The scan
locates the change before comparing the linear address. The checker has
no tiled-address equation.

```sh
python3 src/amd/r300/common/tests/r300_zb_stencil_pair.py \
  --seed-5a "$SEED_5A_DIR" --seed-a5 "$SEED_A5_DIR"
```

Each input directory supplies `depth_before.bin` and `depth_after.bin`.
The tool opens those files read-only, bounds each read, and emits JSON on
standard output. Retain that output through the campaign's evidence writer.

## Interpretation

Exit 0 means the bytes satisfy the pair experiment and were classified.
It does not mean that stencil was preserved. Inspect `selected_behavior`
and `unwritten_slots_preserved` independently. Zero replacement, one
replacement, and other output pairs remain observations rather than being
coerced into preservation or silently discarded.

Exit 1 names a violated input/observation constraint. Exit 2 identifies
argument or input-I/O failure. The output carries hashes of the four byte
images, complete slot counts, stencil histograms, and every stencil-only
change outside the depth-written slot.

The tool does not validate the receipt seal, submitted PM4, boot identity,
color target, completion, or hardware execution. Keep those checks in the
existing campaign. Its scope is `raw_byte_comparison_only`; a successful
run neither authorizes a submission nor promotes a driver capability.

## Complementary seeds

Since `0x5a ^ 0xa5 == 0xff`, each stencil bit is presented once as zero
and once as one. `bit_observations` partitions bit positions into four
observed relationships: same as input, opposite to input, zero for both,
and one for both. These describe two observations, not a proved independent
bitwise transfer function.

For example, swapping input bits 1 and 3 preserves both seeds while
changing input `0x02` to `0x08`. Even preservation for both seeds therefore
leaves cross-bit behavior and the other 254 input bytes unresolved. The
test suite carries this counterexample explicitly.

Use the existing zero-seed result as separate evidence. Supplying it as a
substitute for either nonzero seed is refused by the initial-image check.
Do not patch a retained capture to manufacture a new initial seed.

## Host validation

```sh
python3 src/amd/r300/common/tests/r300_zb_stencil_pair_test.py
python3 -O src/amd/r300/common/tests/r300_zb_stencil_pair_test.py
```

Meson registers the same suite as `r300-zb-stencil-pair` when R300 or R3V
and build tests are enabled. Its fixtures construct bytes independently
of the reader. They cover both seed roles, every region boundary, the
wrong-origin address inside storage, a second write at the final slot,
component separation, all 65,536 output-byte pairs, and exact CLI statuses.
