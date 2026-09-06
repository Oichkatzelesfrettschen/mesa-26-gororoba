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

Two separate facts make the bracket trustworthy, and neither substitutes
for the other.

That the timer reports elapsed wall time at all is measured.
`--inject-delay-ns N` sleeps a known interval inside the timed region on
every arm; a sweep whose delayed and undelayed medians differ by
substantially less than the injected interval reports a timer that is not
measuring what it brackets. It proves timer sensitivity to an interval
inside the bracket and nothing more: a bracket containing only the sleep
would pass it too. The comparison is also one-directional, because the
measured workload varies between runs and no exact difference is
predicted.

That the bracket contains the submission is read out of the control flow,
not inferred from a duration. `run_one` opens the bracket after recording,
fence reset, and destination initialization have all returned, and closes
it after `vkWaitForFences` and the memory contract's invalidate; at
`--inject-delay-ns=0` the submit sits between them with nothing else, and
a nonzero injected delay is the one other thing the bracket ever
contains. A reader checks that
enclosure by reading the function, which is the only thing that can
establish it.

A predicted floor -- "an 8 MiB host fill takes at least this long" -- is a
performance claim, not a timer oracle, so the duration is whatever the
hardware reports. Delivered bytes carry the rest: the per-repetition
verification below fails a repetition that wrote nothing, so no timing row
survives without its write, though delivery is not by itself evidence that
the delivery fell inside the timer.

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
reproduces byte for byte, and it never enters the threshold. `v1` admits
one window, so a size its contract cannot represent records
`NOT_APPLICABLE` for that cell and the campaign continues; its refusal is
the contract answering, and treating it as a failed sample would end a
host-against-`v2` sweep on a diagnostic arm.

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

The two sides of the step differ in stream shape as well as in carrier:
six dwords, 24 bytes, and one additional rectangle. Every row records the
pitch, window count, rectangle count, relocation-site count, and IB dwords
beside its elapsed time, so a difference measured across the step is
attributed after the fact against the recorded shape rather than assigned
to the ioctl and the engine in advance.

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

Hold the fill value constant within a case and reinitialize before every
repetition. Restore the interval and its guards to a sentinel differing
from every byte of the fill pattern, publish that initialization under the
memory type's coherence contract, then time one submission, then verify
the exact interval and the guards -- all of it outside the timer except
the submission itself. A repetition that delivered nothing leaves the
sentinel standing and fails on its own, which a batch oracle reading only
the final value cannot see: an intermediate submission that did no work
hides behind a successful final fill.

Constant repetition measures performance; it does not cover patterns.
Pattern independence is a correctness fixture with its own arms. Recording
the conditioning matters too, because sentinel initialization leaves the
destination in a written cache state and both arms are measured under it;
a threshold derived here does not transfer to a different cache or
memory-visibility workload without measuring that workload.

Repetition is what a timing campaign is; the one-attempt rule that governs
a receipt cell governs a sealed prediction against an unknown outcome, and
this run has neither. Pin the box epoch before the first sample and read it
again after: kernel release, radeon module srcversion, boot id,
`lockup_timeout`, and the installed ICD's build-id. The production module
stays loaded throughout, because a different module epoch, a reboot, or a
different memory-management state is a different measurement.

## What the measurement decides

The campaign decides the selector for the cache state it measures, and the
selector carries that scope with it. Every repetition writes the sentinel
across the whole destination before the timed interval, so both arms meet a
fully dirtied allocation whatever size they then fill, and each pays a
different consequence for it: the host arm overwrites its range on top of
those dirty lines, while the GPU arm publishes them before the device reads
and invalidates the device's output afterward. That conditioning is
uniform across the arms and constant across the sizes, which is what makes
the two comparable, and it is not the cache state an application arrives
in. A threshold measured here selects for a fully dirtied destination; a
selector for the general case needs the same ladder run over
production-representative cache-state classes, and until those exist the
promoted selector names the class it was measured in.

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
