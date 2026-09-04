# R3V public RB2D fill route: qualification and attended cell

The public `vkCmdFillBuffer` route emits PM4 the 2D engine performs. This
document holds what must pass before a submission is justified, the
prediction that submission is measured against, and the questions the cell
does not answer.

Nothing here has run on silicon. Every row below is `not run` until its
transcript exists.

## What executes

An application records one `vkCmdFillBuffer` into a command buffer whose
whole content is that command. At the submission boundary `r3v_route_policy`
resolves the executor; with `R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL`
set to the exact value `1`, the RB2D route answers, the byte interval
decomposes into ordered fill plans, and one indirect buffer carries them
through `DRM_IOCTL_RADEON_CS`. The host performs no store for the result.

The route is `PRECOMMITTED`. It runs only under that exact opt-in and
reports `experimental_admission` in its provenance, so nothing about this
cell advertises a capability. Automatic selection is the separate fact a
promotion does not buy: no crossover between the host store loop and any
GPU route is measured, so `AUTO` reaches this route through its gate alone.

Every gate runs before the stream installs. The route builds its plan,
holds the destination to the memory contract, resolves the executor, emits
and validates the stream, then asks the frozen-cell predicate, the arming
verdict over that exact stream, the operator's declared submission
identity, and the provenance record. Only then does the commit phase
install the stream and mark the record. A route that declines at any of
those leaves the command buffer as it found it, so the host store loop
performs the fill it was going to perform, and the ordinary state -- the
submission gate closed -- is one of those declines.

The install is scoped to one submission. A Vulkan command buffer is
submittable more than once, and the arming authorization, the declared
identity, and the one-shot evidence directory each describe one
submission, so a buffer arriving with a previous submission's stream still
installed is returned to its recorded shape and re-admitted in full. A
spent evidence directory therefore refuses the second submission and leaves
the fill to the host store loop rather than carrying a spent authorization
to the device.

The declared submission identity is
`R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3`, over the allocation, the
buffer binding, the range and its pattern, the carrier pitch and format,
the segment and rectangle lists, the stream length and digest, the
relocation sites, the buffer object's name and its domains, and the running
kernel release and radeon module srcversion. The stream digest alone would
admit the same registers against a different destination through a
different relocation, and a shape digest alone would admit a second
allocation of the same size bound at the same offset, so the identity
carries the destination buffer object by name.

## Qualification before arming

Each rung is a gate, not a preference. A rung that has not run blocks the
submission rather than being weighed against the others.

| Rung | State |
|---|---|
| profile 3, 4, and 5 builds, warning-free under `-Werror` | done |
| `meson test --suite r300 --suite r3v` on all three profiles | done |
| `r3v-route-policy` gate, use, provenance, and automatic-selection arms | done |
| `r3v-fill-route` memory contract, cell predicate, identity, authority | done |
| `r3v-native-fill-route` decline arms leave the command buffer untouched | done |
| route-local host semantic writes zero, against a known-bad host leg | done |
| every refusal calibrated by removing it and failing its test | done: 43 rows, 41 refused, 1 internal guard, 1 reached by no fixture |
| the route observed from `vkQueueSubmit` rather than a direct call | done: `r3v-native-loader-fill-application`, under the drm-shim presenting the declared board |
| the prepared plan mutated field by field and refused | done |
| kernel-entering `DRM_RADEON_CS` count zero for a loader-only application | done |
| `r300-rb2d-linear-span` coverage replay, sweep, and the pinned cell shape | done |
| `r300-direct-write` golden byte-identical | done |
| profile-4 ICD sha256, build id, exports, and Gallium separation triple | not run |
| kernel parser replay at the deployed pin `2be21eaa8927` | done on the host tree at the pin: `r3v-native-rb2d-fill-submit-object-replay`; the board's provenance-bound tool run is not run |
| CS-track replay | done: the same replay tool walks the parser and the tracker; `Kernel replay classes` below records what each owns |
| plan capture records the route and its cell kind | unreachable: the device refuses a capture session while the hazard gate is open, and this route runs only with it open |
| drm-shim submission of this exact cell, with the submit object retained | done: the loader application's armed leg retains ib.bin, relocs.bin, manifest.json, submit_relocs.bin, submit_manifest.json, and the token |
| a non-submitting arming runner on the attended board | not run |
| the sealed prediction | not run |

The mutation matrix `r3v-fill-route` runs, and the check each mutation
lands on:

| Mutation | Refused by |
|---|---|
| wrong pitch | the span layout's 64-byte grid |
| wrong base offset | the fill plan's 1 KiB offset grid |
| wrong rectangle extent | the fill plan's empty and outside-surface arms |
| wrong fill value | the submission identity alone |
| wrong relocation site | the relocation-site validator |
| wrong destination buffer object | the submission identity alone |
| wrong buffer-object role | the frozen-cell predicate |
| wrong write domain | the memory contract |
| truncated stream | the emitter's capacity |
| one extra segment | this route's one-segment contract |
| 32-bit address wrap | the memory contract, and the span's own bound |

`DP_BRUSH_FRGD_CLR` takes any 32-bit pattern and the relocation names
whichever buffer object the recording bound, so neither the fill value nor
the destination has a structural owner and the submission identity is the
only catcher for both. That is the reason the identity covers more than the
stream digest.

## Loader-path qualification under the drm-shim

`r3v_native_loader_fill_application` links libvulkan and libc alone and
records the attended cell the way an application does: instance, physical
device, device, queue, a 64 KiB `TRANSFER_DST` buffer bound at offset zero
in host-visible GTT, one `vkCmdFillBuffer`, one `vkQueueSubmit` with a
fence, one wait.  The radeon drm-shim absorbs the one `DRM_RADEON_CS` and
counts it, so the count its handler exports is the number of submissions
the process made and a kernel-entering count of zero holds by
construction.

The shim presents the board the arming gate compares.  Three declared
facts override the sysfs files the driver reads on a real host:
`R3V_DRM_SHIM_SUBSYSTEM_ID` the subsystem pair beside the PCI id,
`R3V_DRM_SHIM_DMI_PRODUCT_NAME` the firmware product name, and
`R3V_DRM_SHIM_MODULE_SRCVERSION` the radeon module srcversion.  The
physical device resolves `R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M` from
the same reads it performs on the Vostro 1000, so the admission the
loader-path run reaches is the admission the attended run reaches, with
every ioctl ending in the process.  On the board itself the DMI product
and the srcversion are real and only the subsystem pair is declared,
because the shim's placeholder pair would otherwise resolve to no
platform.

`r3v-native-loader-fill-application` runs each leg in a fresh evidence
directory:

- closed: the ordinary state.  The route declines the deployment,
  `GPU_ONLY` refuses the submit, the shim sees no CS, nothing is retained.
- armed: hazard gate, stream digest, kernel release, module srcversion,
  and submission identity declared.  The submit returns `VK_SUCCESS`, the
  fence signals, the shim sees exactly one CS, the read-only destination
  mapping is byte-for-byte as the initialization phase left it, the
  retained ib.bin is byte-identical to the arming runner's independent
  emission, both manifests bind that digest, and the token lands.
- refusals: every fact the public API can vary, wrong one at a time --
  fill value, range, destination object, stream digest, kernel release,
  module srcversion, subsystem pair, DMI product, evidence directory,
  spent token, and the mapped ICD -- refuses ahead of the shim's CS
  handler and leaves the directory unspent.  The write domain, the
  relocation site, and the pitch belong to the route and are mutated in
  process by `r3v-native-fill-route`.
- host known-bad: the same record on the host path over the read-only
  mapping terminates by `SIGSEGV`, so the protection judges stores.
- host control: the host path over a writable mapping fills exactly the
  interval and nothing else, so the sweep judges bytes.

The submission identity binds the destination by the name the kernel
gives it for the submitting process.  The application allocates one
`VkDeviceMemory` before it submits and objects are named from 1 in
allocation order, so the destination is object 1 and the completion
buffer object 2; the wrong-destination arm declares object 2 and refuses
on the identity.  The retained submit object keeps three counts apart:
one relocation site in the stream, two relocation entries in the chunk,
two buffer-object references in the table.

`r3v_native_rb2d_fill_arming_runner` is the non-submitting preview of
that admission.  It creates no Vulkan object, opens no DRM node, allocates
nothing, issues no ioctl, and writes nothing into the evidence directory.
It resolves the board from a sysfs tree (`R3V_NATIVE_RUNNER_SYSFS_ROOT`,
default `/sys`, and `R3V_NATIVE_RUNNER_PCI_SLOT`, default `0000:01:05.0`),
builds the same one-segment stream the route builds, and derives the
stream digest and the submission identity for the declared destination
name.  Its report carries the platform, kernel release, module srcversion,
Mesa source, route, geometry, rectangles, dword count, digest, relocation
sites, buffer-object role schema, identity, directory freshness, and token
state, then the verdict.  `r3v-native-rb2d-fill-arming-runner` drives a
fixture tree through the armed leg and every single-fact refusal.

## Kernel replay classes

`r3v-native-rb2d-fill-submit-object-replay` replays the retained 38-dword
stream through the kernel decision code built from the pinned radeon tree,
with the retained buffer-object table as the bundle.  The parser admits
the stream `ACCEPT-NO-DRAW` and its trace names every register class
admitted: the `DST_PITCH_OFFSET` relocation resolving to entry 0, the
scissor registers, the brush and master-control registers, the rectangle
registers, the destination cache flush, and the wait state.

The parser owns three things on this stream, and each rejects when
mutated: the relocation protocol (a `PACKET3` NOP after
`DST_PITCH_OFFSET`; its absence rejects), the stream framing (a dropped
final dword or an appended dword rejects), and the `PACKET0` register
admission (`0x1430` in place of the destination-cache register or of
`DP_CNTL` rejects through the safe-register bitmap).

The 2D destination geometry passes the kernel unchecked.  `r100_cs_track`
tracks 3D color and depth targets and no 2D destination, so a relocation
naming the four-byte completion object, a swapped relocation order, a
pitch of zero, a base offset past the object, a rectangle past the safe
scissor or past the object, a scissor widened past `0x1fff`, a
`WAIT_UNTIL` of `0xffffffff`, a stream cut before its final wait, and a
1024-byte destination all replay `ACCEPT-NO-DRAW`.  The replay asserts
those as the accept they earn, so a kernel that starts refusing one moves
its row and the test names it.  The consequence for this route is that
the fill plan's grids, the memory contract's containment rule, and the
submission identity are the sole owners of where the 2D engine writes;
the kernel is a grammar check, not a bounds check, for this engine.

## The oracle

A 64 KiB destination buffer, initialized through a separately identified
host phase and published before the submission.

```text
offset        12 bytes          column 3 of row 0 on the 256-byte carrier
size          4992 bytes        1248 dwords: a partial first row, whole
                                rows, and a partial last row
value         0x11223344        four distinct bytes, so a byte-order fault
                                is visible rather than symmetric
prefix        bytes 0 .. 11     canary
interval      bytes 12 .. 5003  sentinel before, fill value after
suffix        bytes 5004 ..     canary
tail          last 64 bytes     canary, past every carrier row the plan
                                declares
```

`r3v_native_cmd_buffer_route_deferred_fill` builds this route's layout at
`R300_RB2D_SPAN_PITCH_DIRECT_WRITE` (256 bytes), the pitch the retained
direct-write control stream exercises, so the oracle below decomposes on
that carrier and not the library's tightest 64-byte grid.

The decomposition of that interval is one segment of three rectangles:

```text
(x=3,  y=0,  w=61, h=1)    the remainder of the first row
(x=0,  y=1,  w=64, h=18)   the whole rows
(x=0,  y=19, w=35, h=1)    the remainder of the last row
```

`test_attended_cell_shape` in `r300_rb2d_linear_span_test` pins those
rectangles, the 20-row surface, and the 5120-byte footprint that leaves the
tail canary outside it, on both the 256-byte carrier this route uses and
the 64-byte carrier no route yet exercises. The prediction is therefore the
decomposition the planner produces rather than one a reader worked out, and
a change to either that separates them fails before a submission is armed
against the wrong bytes.

## Prediction

If the route is correct, after one submission:

- every dword in bytes 12 through 5003 reads `0x11223344`;
- every byte before 12 is unchanged;
- every byte from 5004 to the end of the allocation is unchanged;
- the allocation tail canary is unchanged;
- `vkQueueSubmit` returns `VK_SUCCESS` and the completion retires;
- the `dmesg` delta is empty: no CS rejection, no ring timeout, no reset;
- the boot id is unchanged;
- the retained provenance reads executor GPU, unit `rb2d_fill`, route
  `rb2d_const_fill`, `host_semantic_node` false, `device_submission` true,
  `experimental_admission` true.

The hypothesis is falsified if any dword inside the interval differs from
the fill value, if any byte outside it changes, if the parser rejects the
stream, or if the provenance reports a host semantic node.

A partial fill -- a prefix of the interval correct and a suffix still
sentinel -- falsifies the decomposition rather than the register contract,
and names which rectangle stopped short by where the boundary lands.

The route leaves the record at `prepared` with `device_submission` false,
which is what a prepared stream is, and the submission boundary walks it
through `committed`, `ioctl_entered`, `ioctl_accepted`, and
`completion_retired` as its transport runs, stopping where the transport
stopped. `r3v-native-fill-route` drives that walk directly over each of
the three transport outcomes; the wiring at the submission tail runs only
under an armed submission, so the prediction's provenance line is read for
the first time on the attended run itself.

## What this cell does not answer

**Scissor inclusivity stays open.** `SC_BOTTOM_RIGHT` is programmed at
`0x1fff` and `radeon_reg.h` gives the field width without its inclusivity,
so whether a far edge of exactly `0x2000` is legal is undecided. This cell
cannot decide it: the fill plan refuses a far edge past `0x1fff`, so no
rectangle it produces ever asks for row 8191, and a fill reaching row 8190
lands under either reading. Settling it needs a separate probe whose bound
is deliberately relaxed by one, which is a known-bad-shaped experiment with
its own token. Until then the conservative bound stands and, on this
route's 256-byte carrier, costs 256 bytes of the 2 MiB single-segment
window `R3V_NATIVE_FILL_ROUTE_MAX_SEGMENTS` admits.

**Performance is unmeasured.** No crossover between the host store loop and
the RB2D route exists, so `AUTO` keeps the host path. Expect the host to
win for small fills; a threshold follows measurement, not this receipt.

**One command shape only.** Mixed host and device transfers in one command
buffer are refused rather than ordered. `vkCmdPipelineBarrier` remains what
it was; an execution graph owns that when GPU transfer nodes multiply.

## Arming

The submission is attended, spends one token, and is not retried. On any
deviation the first attempt is preserved, an RCA opens if an ioctl
occurred, and a further attempt requires a new token with the prior
transcript retained.

A pre-ioctl failure -- an arming refusal, a usage error, a stale token --
is classified apart from a hardware or semantic deviation and leaves the
attempt unspent.
