# R3V native attended two-pass cell procedure

The two-pass cell is the rung-7 remainder's mechanism on silicon: one
indirect buffer holding two render-shape cells, the first installed and
the second appended through `r3v_native_cmd_buffer_append_ib`, the
primitive a command buffer that records two render passes with a draw
each takes.  Each cell opens with its own first-draw contract and closes
with the `RB3D_DSTCACHE_CTLSTAT` flush, so a submission whose two
targets each hold their own pass's constant over the analytic triangle
proves the concatenation carries no state across the pass boundary and
the command processor reaches the second cell.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the substrate admission
table, the identity freeze, the arming conjunction, the rollback rules,
and the retained-record layout; this document adds only what the
two-pass cell changes.

## Cell identity

- Runner: `r3v_native_attended_multi_pass <dir>` (native build), taking
  the evidence directory as its one argument.
- Arming digest source: `r3v_native_arming_runner --multi-pass <dir>`
  emits the stream through `r300_tcl_bypass_triangle_multi_pass_emit`,
  whose output is the bound form: the first cell's payloads name its own
  slots, which are merged indices 0 and 1, and the second cell's are
  bound to indices 2 and 3.  The attended runner prints the same digest
  on its `[shape]` line.
- Cell kind: `R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS`.  The arming
  gate's geometry predicate for this kind is frozen: two to four merged
  references, each a vertex page read alone or a color target written
  alone, with every deferred draw executed; an entry carrying both
  directions is a role alias no binding admits and reports unfrozen.
- Recording: `r3v_native_record_multi_pass` installs the first pass's
  cell and appends the second's, refusing a role alias and a declared
  binding that disagrees with the one the four handles produce under
  the winsys first-add rule.  The drm-shim contract runs as
  `r3v-native-multi-pass-cell-bound` (the emitted stream equals the
  installed one and the armed gate admits it), `-shared` (the (0, 1)
  binding over one page and one target), `-alias` (refusals), and
  `-mutated-flush` / `-mutated-second-state` (a digest of a stream the
  recorder never installs refuses at the gate).  The public route's own
  two-draw command buffer reproduces the emitter's stream dword for
  dword in `r3v-native-public-surface`.
- Kernel replay: `r300-multi-pass-cs-track-replay` walks the stream
  through `replay_r300_cs_track` over a four-entry relocation list.
- Shapes: both passes take the reference render shape, 64x64 at pitch
  64 in `B8G8R8A8` at offset zero of their own allocations; the second
  pass's fragment constant is opaque green (`0xff00ff00`), the first's
  the reference draw color (`0xdf20609f`).
- Vertex payloads: the runner writes the reference four-dword position
  records into both pages.
- Seed: both targets carry `0xa5a5a5a5` before the submission.

## Declarations

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner
  --multi-pass reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`

## Arming

1. `r3v_native_arming_runner --multi-pass "$dir"` with the declarations
   unset; retain the closed report.
2. Export the declarations from that report.
3. Rerun the arming runner and require `verdict: armed`.
4. Record the dmesg window, then run
   `r3v_native_attended_multi_pass "$dir"`.

## Predictions

If the concatenation is correct, both `[oracle]` lines for a target
under its own constant report `coverage_exact=1 canary=1
interior=1152 analytic=1152 exterior=2944 mismatch=0`, both crossed
lines (each target under the other pass's constant) report
`coverage_exact=0`, and the dmesg delta is zero.

## Falsifiers

- A target exact under the other pass's constant: state crossed the
  pass boundary; the concatenation's contract is the finding.
- The second target holding the sentinel over the whole extent: the
  command processor never reached the second cell.
- `canary_pass=0` on either target: a write outside the render extent.
- A nonzero dmesg delta or a lockup ends the boot under the shared
  procedure's rollback rules.

## Result

The cell holds its silicon receipt from one attended submission on
RS482, boot `e5fc857e-4aa3-42e7-b3e5-7f31e2250f53`, mesa main
`64fa102e611`, under an arming report matching all five declarations
against cell blake3 `6ff86047`.  Every predicted value held: both
targets `coverage_exact=1 canary=1 interior=1152 analytic=1152
exterior=2944 mismatch=0` under their own constants and
`coverage_exact=0 mismatch=1152` under the other pass's, the second
centroid `0xff00ff00`, `vkQueueSubmit` 0, an empty dmesg delta, an
unchanged boot, and a 99 us guarded interval.  So the concatenation
carries no state across the pass boundary and the command processor
executes the appended cell after the installed one inside one
indirect buffer.  The same stream replayed clean through
`replay_r300_cs_track` over four relocations on the host before the
run.  Bundle: steinmarder-r300
`src/re/r300/results/r3v-native-two-pass-concatenation-first-delivery-rs482`.
The shared bindings hold on the drm-shim and are unrun on silicon, and
this arm ran the recorder route; the public-API two-draw command buffer
on hardware is the next step.

## Retained record

The shared procedure's record plus `first_target.bin` and
`second_target.bin`, each the shape's full footprint including the
canary row.
