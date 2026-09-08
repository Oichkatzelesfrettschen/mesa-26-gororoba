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
  --seed-5a "$SEED_5A_DIR" --seed-a5 "$SEED_A5_DIR" \
  --pair-context "$VERIFIED_PAIR_CONTEXT"
```

Each input directory supplies `depth_before.bin` and `depth_after.bin`.
The tool opens those files read-only, bounds each read, and emits JSON on
standard output. Retain that output through the campaign's evidence writer.

## Interpretation

The version 2 raw result uses `status=OBSERVED` and
`qualification=UNJUDGED`. The raw API classifies supplied bytes independently
of execution metadata. `selected_slot_behavior` describes the unique
depth-written slot; `spatially_isolated` and `off_target_stencil_changes`
report whether additional stencil writes occurred. Replacement by zero or
one remains a valid local observation. Additional stencil writes fail the
isolated experiment while retaining that local observation.

`scan_images()` retains safely readable slot counts, histograms, region
counts, hashes, and contextual errors. `observe()` applies the raw geometric
contract and attaches the collected observation to `ObservationRefusal`.
The zero-seed calibration continues to use `observe()` directly. Missing
coverage produces null interpretation fields. CLI input records distinguish
complete-file digests from bounded-prefix digests on oversized inputs.

The CLI returns 1 for geometric or isolation refusal and 2 for input I/O
failure, retaining readable inputs in either case. Raw classification alone
establishes byte relationships; hardware qualification additionally requires
execution metadata and the campaign's sealed receipt authority.

## Execution evidence join

The CLI requires each `zb_depth_discovery_outcome.json` and an explicit
`r300-zb-stencil-pair-context/1` JSON document. Exit 0 means a qualified,
isolated observation under the supplied receipt context. Exit 1 retains a
judged constraint refusal; exit 2 identifies input or hasher infrastructure
failure. The raw Python API remains independent of those external inputs.

Outcome schema `r3v-native-zb-depth-discovery-outcome/1` has strict field
types, exact seed-specific scenario identity, bounded JSON parsing, and
independent raw counter comparisons. PATH-resolved `b3sum --no-names` reads
exactly the immutable before-image bytes through stdin. Successful submission,
completed queue, declared initialization, and the color oracle are required.
Duplicate keys and non-finite JSON values refuse.

The context has a `declaration` identity object and `runs` keyed by
`seed-5a` and `seed-a5`. Each run supplies `identity`,
`authority=retained-bundle-sha256`, the verified `seal_sha256`, and an
`artifacts` map containing SHA-256s for `depth_before.bin`,
`depth_after.bin`, and `zb_depth_discovery_outcome.json`.
`CONTEXT_FIELDS` defines the required identity fields. Every run must match
the independently predeclared identity. Seed-specific image digests and
receipt seals may differ.

The campaign receipt verifier owns seal verification and normalized identity
extraction. The caller must supply its verified context and preserve the
predeclared identity's provenance. The public checker verifies the supplied
joins; a self-consistent JSON document supplies neither hardware attestation
nor authorization. Public tests use synthetic contexts and a real BLAKE3
integration leg, independently of the evidence checkout.

Boot, platform, source/profile, application SHA-256/build ID, and runner
identity come from sealed `identity.txt`. Driver BLAKE3 and kernel/module
fields come from sealed `submit_manifest.json`. Arming alone establishes
readiness rather than completed execution.

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
