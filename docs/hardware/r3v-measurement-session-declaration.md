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
than being ignored.

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

## What survives a crash

Two facts have two lifetimes.

That a session started is durable. The first admission writes the
evidence directory's attempt token through `r3v_native_arming_disarm`,
fsynced file and directory, so a second process against that directory
finds the token standing and refuses as already attempted.

How much a session spent is process-local. The counters live in the
device and die with it. A run that stops at forty of four hundred and one
that stops at three hundred and ninety-nine leave the same durable
object, so the token bounds restarting rather than accounting, and a
campaign's own spend is read out of its published samples.

## Dispositions

| Condition | Disposition |
| --- | --- |
| undeclared case | refuse before submission |
| role mismatch, route mismatch, epoch mismatch | refuse before submission |
| replaced destination or recycled handle over a new allocation | refuse before submission |
| recomputed identity differs from the bound one | refuse before submission |
| budget exhausted | refuse before submission |
| kernel submission failed | consume that attempt; terminate the session |
| completion or oracle failed | terminate the session |

## What this is not

This is not unrestricted batch authorization. A declaration enumerates
its campaign's cases, and each case stays bound to one realized
destination and a finite repetition count. It is also not a widening of
the one-shot path: that path's checks are untouched, its tests are
unchanged, and a device with no declaration behaves exactly as before.
