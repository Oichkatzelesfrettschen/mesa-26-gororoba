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
than being ignored. A terminator inside the text refuses, because the
digest covers the whole byte range while a field stops at its first
terminator.

A number is decimal or `0x`- or `0X`-prefixed hexadecimal and nothing
else. The base is chosen from the prefix and every character after it is
then held to that base's alphabet, so a sign refuses wherever it appears:
`strtoull` negates one into the unsigned range and accepts one after a
stripped prefix too, which would let `0x-1` name 2^64 - 1 on the strength
of the prefix. A leading zero refuses, because base zero would read
`0644` as `420`; the rule is about base selection, so a hexadecimal value
carries leading zeros freely. Magnitude is still decided by the
conversion, so a value above the field refuses there.

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

Reading and meaning are separate. The reader decodes text: line shape,
key spelling, repeated keys, number grammar, the six fields of a `case`
row. Every rule about what a declaration means -- name termination and
non-emptiness, the schema, case count, the buffer's fit in its
allocation, each case's alignment, size, execution count, and
containment, case uniqueness, budget arithmetic, and the timeout's
positive finite bound -- lives in one check both the reader and `open`
run. A manifest assembled in place, or edited after parsing, therefore
meets exactly the rules a parsed one meets.

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
| read the declaration, hash it, parse it, open the session | `r3v_native_device_refresh_delivery_gates`, at `vkCreateDevice` |
| hold the declaration to the running deployment | `r3v_measurement_manifest_epoch_check`, against the arming facts |
| hold the resolved executor to the declared route | `r3v_measurement_session_route_check`, at route admission |
| resolve the destination and hold it to the role | `r3v_measurement_session_role_check`, in the fill route |
| stamp an allocation generation the binding can read | `vkAllocateMemory`, from a device-monotonic counter |
| resolve the recorded operation to a case row | `r3v_measurement_session_find_case`, on the offset, size, and value the command carries |
| bind the case before entering the ioctl | `r3v_measurement_session_bind`, after the identity is computed |
| consume one execution immediately before the kernel boundary | `r3v_measurement_session_consume`, at the submission site |
| serialize the calls | one queue family at `queueCount = 1`, so Vulkan's external synchronization on `vkQueueSubmit` orders every bind and consume |
| close the session on a failed completion or a failed oracle | the queue's transport tail |

Three rules the predicate enforces itself, rather than trusting the site
that calls it. A session opens once: a second open over a live session
refuses instead of clearing its bindings and restoring the allowance the
first one spent, and a closed session stays closed and names why. Every
structural rule the reader enforces on declaration text, `open` enforces
again on the struct, so a manifest assembled without the reader or edited
after parsing meets the same rules. And `bind` and `consume` carry the
offset, size, and value the recorded operation fills, held against the
case the index names: the index selects a row, the operation decides
whether that row is the one the request runs, so a self-computed digest
never authorizes an operation the declaration does not name.

The predicate closes itself on the two failures it can see: a case that
resolves to another allocation and a recomputed identity that left its
binding both terminate the session rather than refusing alone, because a
refusal would leave the binding standing and admit the next repetition
against it. Device loss, a failed completion, and a failed oracle are
outside what a predicate observes, so the wiring closes on those.

A role names a shape, not an object, so a second allocation matching the
declared role passes the role check and then terminates the campaign at
`bind` when its handle and generation differ from the bound ones. A
substitution and a caller that routed the wrong buffer are the same
observation here and carry the same terminal disposition, which is the
fail-closed direction: the campaign ends and the operator reads why,
rather than the samples continuing against an object the declaration did
not bind.

The termination reason is copied into the session, so a reason formatted
on a caller's stack is valid for every later refusal.

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
