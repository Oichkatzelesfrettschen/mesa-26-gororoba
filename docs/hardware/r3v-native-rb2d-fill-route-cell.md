# R3V public RB2D fill route: qualification and attended cell

The public `vkCmdFillBuffer` route emits PM4 the 2D engine performs. This
document holds what must pass before a submission is justified, the
prediction that submission is measured against, and the questions the cell
does not answer.

Nothing here has run on silicon. Every row below is `not run` until its
transcript exists.

## What executes

An application records one `vkCmdFillBuffer` into a command buffer whose
whole content is that command. At queue preparation `r3v_route_policy`
resolves the executor; with `R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL`
set to the exact value `1`, the RB2D route answers, the byte interval
decomposes into ordered fill plans, and one indirect buffer carries them
through `DRM_IOCTL_RADEON_CS`. The host performs no store for the result.

The route is `PRECOMMITTED`. It runs only under that exact opt-in and
reports `experimental_admission` in its provenance, so nothing about this
cell advertises a capability.

## Qualification before arming

Each rung is a gate, not a preference. A rung that has not run blocks the
submission rather than being weighed against the others.

| Rung | State |
|---|---|
| profile 3 build, warning-free under `-Werror` | done |
| profile 4 release build and package identity triple | not run |
| `meson test --suite r300 --suite r3v` | done: 389 pass, 0 fail |
| `r3v-route-policy` gate, use, and provenance arms | done |
| `r3v-fill-route-host-exclusion` source check | done |
| `r300-rb2d-linear-span` coverage replay, sweep, and the pinned cell shape | done |
| `r300-direct-write` golden byte-identical | done |
| drm-shim exact submit object | not run |
| kernel parser replay at the deployed pin `2be21eaa8927` | not run |
| CS-track replay | not run |
| plan capture records the route and its cell kind | not run |
| truncated stream, wrong relocation, wrong pitch, wrong base refused | not run |
| kernel-entering `DRM_RADEON_CS` count zero under the shim | not run |

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
