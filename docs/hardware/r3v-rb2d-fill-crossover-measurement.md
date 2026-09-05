# RB2D constant-fill crossover measurement

`R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2` executes on three silicon
receipts and its gate stays closed to automatic selection, because
`r3v_route_automatic_selection_admitted` holds an empty set and AUTO
therefore keeps the host store loop. The set stays empty until a
measurement decides where the windowed GPU route beats the host, and this
document is the contract that measurement runs under.

## What the timer contains

`r3v_CmdFillBuffer` records a deferred copy and performs no store, so both
routes do their work inside `vkQueueSubmit`:
`r3v_native_cmd_buffer_execute_deferred_copies` runs the host store loop
there, and `r3v_native_fill_route` lowers, legalizes, builds the indirect
buffer and its relocations, and enters `DRM_IOCTL_RADEON_CS` there. One
bracket therefore contains each arm's own delivery, and the harness opens
it immediately before the submit and closes it after the fence retires and
the memory contract's invalidate returns.

Inside the bracket: route choice, legalization, carrier choice, IB
construction, relocation construction, the ioctl, hardware execution,
completion, and the invalidation a non-coherent memory type requires.

Outside it: instance, device, buffer, memory, mapping, command-pool and
command-buffer creation, the per-batch destination initialization, the
batch oracle, and every line of output. Those are qualification machinery,
and a timing row that carried them would report the harness rather than the
route.

The falsifier for the bracket itself is the largest size: an 8 MiB host
fill is a K8 store loop over 8 MiB and lands in the tens of milliseconds. A
host arm reporting microseconds there proves the bracket missed the work,
and every threshold derived from that sweep is void.

## Arms

The route gates and `R3V_NATIVE_EXECUTION_POLICY` are read once, at
`vkCreateDevice`, by `r3v_native_device_refresh_delivery_gates`. One device
therefore answers for exactly one route for its whole lifetime, and the
harness holds one device per arm in one process, each created with that
arm's gate state installed and the other arms' gates cleared. Two open fill
gates are refused at device creation, so each arm names at most one.

| Arm | Policy | Gate | Role |
|---|---|---|---|
| `host` | `cpu_reference` | none | the AUTO decision's other side |
| `v2` | `gpu_only` | `R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL` | the candidate |
| `v1` | `gpu_only` | `R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL` | the frozen differential |

`cpu_reference` names the executor rather than the policy, so the host arm
measures the store loop whatever the admitted set later holds; with no gate
open and an empty admitted set, AUTO performs that same store loop today.

The AUTO decision is host against `v2`. `v1` is a diagnostic control: it
detects a V2 regression against the route whose stream the legalizer
reproduces byte for byte, and it never enters the threshold.

## Sizes

The declared sweep is 4 B, 64 B, 256 B, 4 KiB, 64 KiB, 512 KiB, 2 MiB, and
8 MiB, plus the two sizes bracketing the chooser's carrier transition.

At the execution floor -- `minimum_evidence` and
`minimum_contract_evidence` both at SILICON_RECEIPT, where the admitted
carriers are the receipted pair 256 and 16320 -- `r300_rb2d_choose_pitch`
takes 256 through 2096896 bytes and 16320 from 2096900 on. 2096896 is
`256 * 8191`, the last interval one window on the 256-byte carrier covers
inside the 13-bit scissor's safe end; the next dword needs a second window,
and a second window costs more than the wider carrier's extra rectangle.
The transition is a single step and the pitch selection is monotone in
size, so two measured sizes bracket it exactly:

| Size | Pitch | Windows | Rects | Sites | IB dwords |
|---|---|---|---|---|---|
| 4 through 2096896 | 256 | 1 | 1 | 1 | 26 |
| 2096900 through 8388608 | 16320 | 1 | 2 | 1 | 32 |

The stream shape barely moves across the whole sweep -- one window, one
relocation site, 26 or 32 dwords -- so the GPU arm's cost is the ioctl,
validation, and engine time, not the indirect buffer. That prediction is
what the measurement tests.

Each row records the shape the interval belongs to. The harness derives it
by calling `r300_rb2d_legalize_linear_span` directly, which reads no
device, so the recorded shape is the legalizer's own answer rather than a
value read back from a submission.

## Sampling

Warm both arms on a size before any sample: the first submission on an
interval pays page faults and allocator work no later one repeats. Then
alternate arm order every repetition, so a thermal or load drift over the
batch lands on each arm in equal measure rather than on whichever ran last.
Report median, median absolute deviation, p10, p90, and the raw samples;
the median and the MAD are the statistics a single stalled repetition
cannot move.

Advance the fill value per repetition. A destination that kept an earlier
repetition's bytes then fails the batch oracle, so a route that stopped
delivering cannot pass by leaving the previous pattern in place.

Verify after each batch, outside the timer: every dword of the interval
carries the last repetition's value and every byte past it carries the
sentinel the batch initialization wrote. A route that wrote nothing, wrote
short, wrote an earlier value, or wrote past the interval fails there, and
every surviving timing row rests on delivered bytes.

Repetition is what a timing campaign is; the one-attempt rule that governs
a receipt cell governs a sealed prediction against an unknown outcome, and
this run has neither. Pin the box epoch before the first sample and read it
again after: kernel release, radeon module srcversion, boot id,
`lockup_timeout`, and the installed ICD's build-id. The production module
stays loaded throughout, because a different module epoch, a reboot, or a
different memory-management state is a different measurement.

## What the measurement decides

Automatic selection is a measured selector over the operation, the use, the
byte size, and the legalized carrier and window shape, not a boolean added
to a table. The legalizer runs first as a prediction-only operation and its
shape selects the executor.

A monotone result encodes one threshold: below `T` the host, at `T` and
above `v2`. A result where the 256-to-16320 transition creates a timing
discontinuity encodes a piecewise policy over the shape regions instead. A
single threshold is not forced onto non-monotone measurements.

Admission requires a margin. `v2` is admitted only where it wins repeatedly
by a guard band that holds across neighboring sizes, not where the two arms
tie. The benchmark corpus and the derivation are retained in
`steinmarder-r300`, and the threshold in Mesa source cites that bundle.

The dual-gate refusal stands unchanged. Explicit V1 and V2 contention is
refused at device creation and again at every route request; an AUTO
fallback never resolves it.
