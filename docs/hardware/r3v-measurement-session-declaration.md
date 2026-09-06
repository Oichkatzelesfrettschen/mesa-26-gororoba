# The bounded measurement session declaration

A timing campaign submits one fill several hundred times. The ordinary
one-shot authorization admits exactly one submission, and it cannot be
written for a campaign at all: its digest covers the destination's GEM
handle, and a GEM handle is an index into one DRM file's object table
that does not exist when an operator writes a declaration. The session
below resolves that without weakening the one-shot path, which stays
unchanged and stays the ordinary route.

## Two levels of identity

The declaration is written before any device exists. It names the
platform, the deployment epoch, the route, the destination's role, the
exact cases, and the budgets. It is read and hashed once at
`vkCreateDevice`, beside the route gates, so nothing the environment does
afterward moves a decision under a recorded command buffer.

Two layers carry that. `r3v_measurement_session.c` is the predicate: it
parses a declaration, holds a request to it, binds a destination, and
counts executions, and it reads no device and no file. Everything the
predicate is called at -- the device-creation read and hash, the epoch
facts, the route name, the resolved destination, and the consume site
before the ioctl -- is the driver's, and "What the wiring owes the predicate" names each
obligation. A rule stated here without a named owner is a rule nothing
enforces.

The binding happens inside the device. The first authorized preparation
of each case resolves the destination through the recorded `VkBuffer` and
its bound memory, holds that resolution to the declared role, and records
the buffer object's handle, its allocation generation, and the concrete
fill identity the ordinary digest covers. Every later repetition of that
case recomputes the identity and requires it to equal the bound one.

The operator therefore declares the resource and the operation while the
driver records the handle. A self-computed digest never authorizes an
operation the declaration does not name, and no run performs a failing
`vkQueueSubmit` to learn a digest.

The handle alone is not the object. A GEM handle is recycled once the
object it named is destroyed, so the binding carries the allocation's own
generation beside it, and a recycled number over a different object
refuses as a rebound destination.

## Declaration format

Lines of `key = value`, `#` starting a comment, every scalar key
required. A key this reader does not read refuses the declaration rather
than being ignored. A number is decimal or `0x`-prefixed hexadecimal and
nothing else: a leading sign refuses, because `strtoull` would negate it
into the unsigned range, and a leading zero refuses, because base zero
would read `0644` as `420`.

| Key | Meaning |
| --- | --- |
| `schema` | `r3v-measurement-session-v1` exactly |
| `session_nonce` | names this campaign in its published samples |
| `platform` | the specimen the campaign runs on |
| `route` | the executor the campaign measures, held against the route the device resolves |
| `pci_vendor_id`, `pci_device_id` | the device, decimal or `0x` hex |
| `kernel_release`, `module_srcversion` | the deployment epoch |
| `allocation_bytes`, `buffer_bytes`, `binding_offset` | the destination's role, never a handle |
| `memory_property_flags`, `buffer_usage`, `write_domain` | the rest of that role |
| `max_total_submissions` | the campaign ceiling |
| `completion_timeout_ns` | the finite completion bound |
| `case` | `id, offset, bytes, value, warmups, repetitions` |

A case is the exact fill, not a range a request falls inside: a request
matches on offset, size, and value together. Two cases carrying one
identity refuse the declaration, because a request would match both and
consume an ambiguous budget. A case that runs nothing, one whose range
counts no whole dword on a dword boundary, and one reaching past the
declared buffer each refuse.

## Budget

Identity and repetition are separate predicates: two submissions agreeing
in every field are still two executions. The budget is the sum every
case's warmups and repetitions account for, warmups included because both
enter the kernel. A declared `max_total_submissions` below that sum cannot
run the campaign it declares and refuses at open, rather than exhausting
partway through under a refusal that reads as a defect.

One permitted execution is consumed immediately before the kernel
submission boundary and never refunded: an attempt that entered the ioctl
is an execution whatever the ioctl returns. A completion failure, an
identity mismatch, or an oracle failure closes the session, and a closed
session refuses every further request.

## What the wiring owes the predicate

The predicate holds these only when the driver calls it where the table
says. Each is the wiring's obligation, not a guarantee this file's
implementation makes on its own.

| Obligation | Site |
| --- | --- |
| read the declaration, hash it, parse it, open the session exactly once | `r3v_native_device_refresh_delivery_gates`, at `vkCreateDevice` |
| hold the declaration to the running deployment | `r3v_measurement_manifest_epoch_check`, against the arming facts |
| hold the resolved executor to the declared route | `r3v_measurement_session_route_check`, at route admission |
| resolve the destination and hold it to the role | `r3v_measurement_session_role_check`, in the fill route |
| stamp an allocation generation the binding can read | `vkAllocateMemory`, from a device-monotonic counter |
| bind the case before entering the ioctl | `r3v_measurement_session_bind`, after the identity is computed |
| consume one execution immediately before the kernel boundary | `r3v_measurement_session_consume`, at the submission site |
| close the session on a failed completion or a failed oracle | the queue's transport tail |

The predicate closes itself on the two failures it can see: a case that
resolves to another allocation and a recomputed identity that left its
binding both terminate the session rather than refusing alone, because a
refusal would leave the binding standing and admit the next repetition
against it. Device loss, a failed completion, and a failed oracle are
outside what a predicate observes, so the wiring closes on those.

## What survives a crash

Two facts have two lifetimes.

That a session started is durable, once the wiring reaches the disarm
site. The first admission writes the evidence directory's attempt token
through `r3v_native_arming_disarm`, fsynced file and directory, so a
second process against that directory finds the token standing and
refuses as already attempted.

How much a session spent is process-local. The counters live in the
device and die with it. A run that stops at forty of four hundred and one
that stops at three hundred and ninety-nine leave the same durable
object, so the token bounds restarting rather than accounting, and a
campaign's own spend is read out of its published samples.

## Dispositions

| Condition | Disposition | Owner |
| --- | --- | --- |
| undeclared case | refuse before submission | predicate |
| role mismatch, route mismatch, epoch mismatch | refuse before submission | predicate |
| replaced destination or recycled handle over a new allocation | refuse before submission and terminate the session | predicate |
| recomputed identity differs from the bound one | refuse before submission and terminate the session | predicate |
| budget exhausted | refuse before submission | predicate |
| kernel submission failed | consume that attempt; terminate the session | wiring |
| completion or oracle failed | terminate the session | wiring |

The budget bound a campaign meets is the case bound. The session
allowance is the sum of the case allowances, so the session counter
reaches zero only when every case has; session-only exhaustion is
unreachable under this accounting, and the session bound stands against a
future declaration form carrying a smaller total.

## What this is not

This is not unrestricted batch authorization. A declaration enumerates
its campaign's cases, and each case stays bound to one realized
destination and a finite repetition count. It is also not a widening of
the one-shot path: that path's checks are untouched, its tests are
unchanged, and a device with no declaration behaves exactly as before.
