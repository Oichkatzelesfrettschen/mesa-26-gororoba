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
then held
to that base's alphabet, so a sign refuses wherever it appears: `strtoull`
negates one into the unsigned range and accepts one after a stripped
prefix too, which would let `0x-1` name 2^64 - 1 on the strength of the
prefix. A leading zero refuses, because base zero would read `0644` as
`420`; the rule is about base selection, so a hexadecimal value carries
leading zeros freely. Magnitude is still decided by the conversion, so a
value above the field refuses there.

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

## Loading the declaration

`r3v_measurement_declaration_open` reads one file into one buffer, hashes
those bytes, parses those same bytes, and opens one session over them. It
takes no device: every fact it compares arrives in a deployment struct, so
each of its boundaries -- the open, the bounded read, the text allocation,
the parse, the epoch, the board, the route, and the session open -- is
reachable from a test with a temporary file and no Vulkan. The buffer
holds one byte more than the text bound, so a read that reaches that byte
establishes the file is above the bound rather than truncating it into a
shorter declaration that parses; a file of exactly the bound loads.

The load stands apart from `r3v_native_device_refresh_delivery_gates`.
That function re-reads the environment and runs many times over one
device, and a session that reopened on each pass would clear its bindings
and restore the allowance the previous pass spent. One device opens one
session, once, at creation. `r3v_measurement_session_open` refuses a
reopen over a live session on its own, and
`r3v_measurement_session_isolation_audit.py` holds the gate refresh's body
to naming no session state.

An absent declaration and a defective one are separate outcomes:

| Declaration condition | Device creation |
| --- | --- |
| no declaration named | ordinary behavior, session inactive |
| named declaration does not open | refuse |
| declaration above the text bound | refuse |
| parse, epoch, board, or route refuses | refuse |
| text allocation fails | refuse as out of host memory |
| valid declaration | one device-local session opens |

One session belongs to one device. Two `VkDevice` objects created under
one declaration open two sessions, each over the declaration's full
allowance, and nothing in the load prevents that: a session's counters
live in the device and the load compares no state outside it. Holding one
campaign to one allowance across devices and processes is the durable
claim's obligation, listed above and not yet wired. A campaign run before
that lands is bounded per device, not per declaration.

An operator who named a declaration asked for a measured campaign, so a
declaration the driver cannot read refuses the device rather than
downgrading to ordinary execution and publishing samples against an
authorization that never opened. A failed allocation is a shortage of
memory rather than a defect in what the operator wrote, so it carries
`VK_ERROR_OUT_OF_HOST_MEMORY` where every declaration defect carries
`VK_ERROR_INITIALIZATION_FAILED`.

The declared board is resolved through `r300_platform_id_from_declaration_name`
and required to equal the board the device resolved. Three facts decide
it and each refuses on its own: a name no platform row carries, a running
board that resolved to no qualified platform, and a resolved pair that
disagrees. Two unresolved names are not a match. The resolver compares
exactly -- it folds no case and strips no space -- because DMI
normalization belongs to the platform resolver and a declaration is text
an operator wrote for one row.

The declared route is held to four facts, each refusing alone: the route
exists in this build's registry, its state is executing, its evidence
reaches its own delivery rather than a unit its family shares, and it
serves a linear transfer destination. Where the route carries an opt-in,
that opt-in must already stand open on this device. The load reads the
device's gate cache and never writes it: reading a declaration opens no
gate, lowers no evidence floor, and resolves no competing pair by
preference. The gate reading is a diagnostic rather than an invariant:
`r3v_native_device_refresh_delivery_gates` rewrites that cache from the
environment after creation, so a gate open at load can be closed by the
first submission. The standing rule is
`r3v_measurement_session_route_check` at route admission, which holds the
executor the device resolved against the declared one for every request;
the load reports the disagreement at creation, where an operator can act
on it, instead of once per command. Opening the predicate grants nothing on its own; measurement
execution becomes available when the claim, the consumption, and the
transport wiring have landed.

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
oracle failure, and a well-shaped identity differing from the bound one
each close the session; a request the predicate cannot read at all --
an identity that is not a digest, an index naming no case, an operation
the named case does not declare -- refuses and leaves the campaign
standing, because an unreadable request is not a contradicted
declaration. A closed session refuses every further request and answers
each with the reason it closed.

## What the wiring owes the predicate

The predicate holds these only when the driver calls it where the table
says. Each is the wiring's obligation, not a guarantee this file's
implementation makes on its own.

| Obligation | Site |
| --- | --- |
| read the declaration, hash it, parse it, open the session | one static helper inside `r3v_native_device.c`, called once from `r3v_CreateDevice`; `r3v_measurement_session_isolation_audit.py` holds `r3v_measurement_declaration_open` to that one call site |
| hold the declaration to the running deployment | `r3v_measurement_manifest_epoch_check`, against the arming facts |
| hold the resolved executor to the declared route | `r3v_measurement_session_route_check`, at route admission |
| resolve the live destination for every repetition | the fill route, from the recorded `VkBuffer` and its currently bound memory, never from the standing binding |
| hold the resolved destination to the declared role | `r3v_measurement_session_role_check`, on that resolution |
| stamp an allocation generation the binding can read | `vkAllocateMemory`, from a device-monotonic counter |
| resolve the recorded operation to a case row | `r3v_measurement_session_find_case`, on the offset, size, and value the command carries |
| bind the case before entering the ioctl | `r3v_measurement_session_bind`, after the identity is computed |
| consume one execution immediately before the kernel boundary | `r3v_measurement_session_consume`, at the submission site, naming the same case, operation, object, and identity the bind carried |
| compute the identity over the bytes the ioctl receives | the submission path, on the immutable command stream, with no mutation between the consume and the ioctl |
| serialize bind and consume | the queue execution path alone, where Vulkan's external synchronization on `vkQueueSubmit` orders the calls |
| serialize the generation counter | `vkAllocateMemory`, through an atomic or the allocation lock |
| claim one session across devices and processes | an exclusive durable claim on the declared arm |
| close the session on a failed completion | the queue's transport tail |
| close the session on a failed oracle | the benchmark application, which is where the destination is read |

Vulkan's external synchronization requirement attaches to the queue
object, so it orders the bind and the consume that sit in one queue's
execution path and nothing else. It is not a device-wide lock:
`vkAllocateMemory` takes no queue lock, and two devices carry two queues,
so the allocation generation and the session claim each carry their own
serialization above rather than inheriting the queue's. Putting a bind or
a consume in a command-buffer recording path would leave them unordered,
because recording distinct command buffers on distinct threads is legal
Vulkan; the measurement path keeps them at the submission boundary.

The declaration is hashed and parsed from one immutable byte buffer read
once. Hashing a path and reopening it for parsing would let the bytes
change between the two, so the digest would name a declaration the
session never read.

The bind and the consume describe one submission, and the wiring makes
that description the submission. The predicate holds the two descriptions
against each other: the bind authorizes an object and a stream, and the
consume names the case, the operation, the object, and the identity again
so every field is held against the binding. What the predicate cannot see
is whether the bytes that reach the ioctl are the bytes the identity
covers, so the wiring computes that identity over the immutable command
stream it is about to submit and enters the ioctl with no step between
that could change it. A consume carrying the
operation alone would let a bind against one allocation stand while the
consume and the submission ran against another the role also admits, and
a standing binding cannot see that substitution. A consumed submission
naming another object or another stream terminates the campaign; one
whose identity is not a digest at all refuses the request and leaves the
campaign open, because an unreadable request is not a contradicted
declaration.

The binding is values, never a pointer. It carries the destination's GEM
handle, its allocation generation, and the concrete fill identity, and no
`VkDeviceMemory` wrapper pointer: `r3v_AllocateMemory` creates the GEM
buffer object inside that wrapper and `r3v_FreeMemory` unmaps it, frees
it, and destroys the wrapper, so a retained owner would change that
lifetime model rather than increment an existing count. Vulkan's own
resource-lifetime contract is what makes the values enough. An application
completes a submitted use before freeing the memory or destroying the
buffer, which keeps the resource valid across its pending submission --
and says nothing about the interval between completed submissions, so a
session that outlives a destroyed allocation holds identity data rather
than a live object.

Two obligations follow, and the submission wiring owes both. Every
repetition resolves its own live `VkBuffer` and bound `VkDeviceMemory`,
reads the current handle and generation, recomputes the identity, and
compares against the binding rather than reusing the values the binding
already holds. An application that completes one submission, destroys the
allocation, allocates a replacement, and submits against the standing
session is then refused even where the replacement's size, role, and
recycled handle all match, because the generation differs. The predicate
carries the comparison; nothing in the driver performs that resolution
until the queue integration lands.

Digests cross this boundary as `struct r3v_measurement_digest`, not as an
array parameter. An array parameter decays to a pointer, so the width a
scan walks would be a promise the caller makes rather than a fact the
type carries, and a shorter object would be read past its end.

What the predicate does not decide: its counters live in one struct and
die with it, so a fresh struct over the same declaration receives the
whole allowance again. Bounding one campaign across devices and processes
is the durable claim's job, and until that wiring exists the predicate
bounds a campaign within one live session rather than across every
session a declaration could open.

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
| kernel submission failed | consume that attempt; terminate the session | queue |
| completion failed | terminate the session | queue |
| oracle read a wrong byte | publish no sample; issue no further submit; destroy the device | benchmark application |

The oracle sits with the process that reads the destination. The queue
observes its own completion result and nothing after it, so a driver-side
comparison would either measure verification inside the timed interval or
name a result the queue never saw. The application invalidates and reads
the destination, verifies the whole interval and its guards, and on a
wrong byte publishes no sample, issues no further submit, and destroys
the device; device destruction closes the remaining session state and
leaves the durable claim standing. One measured operation is outstanding
at a time, so a repetition is verified before the next one is enqueued.

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
