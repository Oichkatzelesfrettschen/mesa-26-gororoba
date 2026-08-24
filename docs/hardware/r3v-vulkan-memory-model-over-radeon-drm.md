# R3V Vulkan memory model over Radeon DRM

This document assigns each stage of the Vulkan memory model to its owner,
from the Vulkan entry point through the R3V transport to the Radeon DRM
UAPI and the kernel TTM/GART machinery, for the RS480-family UMA target
(`1002:5974`, `CHIP_RS480`).  Kernel-side facts cite row identifiers from
the linux-radeon-gororoba ledger `policy/rs4xx-gart-memory-path.tsv` and
its capacity companion `policy/rs4xx-vram-gtt-capacity-contract.tsv`;
those rows carry their own source/runtime/silicon status, and this
document inherits it rather than promoting it.  The Mesa-side consumer
pin for those rows lives in the kernel-contract pin test (see
`src/amd/r300/vulkan/tests/`), which blocks on a row-content mismatch.

## Domains and platform geometry

RS480-family graphics memory is one physical pool with two windows: the
BIOS carves a fixed shared-VRAM partition out of system DRAM (the
northbridge TOM interval; kernel row `RS482_VRAM_CARVEOUT_ACCOUNTING`),
and the internal GART translates a virtual aperture over ordinary system
pages (rows `RS482_GTT_SIZE_REGISTER_ENCODING`,
`RS482_GTT_ADDRESS_PLACEMENT`).  `r3v_memory_properties_contract.h`
carries the platform ceilings (128 MiB carve-out, 1 GiB aperture) and the
invariants any reported table satisfies; the reported heap size itself is
the kernel's `DRM_RADEON_GEM_INFO` gart_size + vram_size, clamped to the
ceiling (`r3v_GetPhysicalDeviceMemoryProperties2`,
`r3v_native_memory_properties_fill`).  One DEVICE_LOCAL heap carries both
kernel pools because they draw from the same DRAM.

## Memory types and placement

`r3v_native_memory_type_policy` in `r3v_native_memory.c` owns the
Vulkan-type-to-domain translation:

- Type 0: `RADEON_GEM_DOMAIN_GTT` + `RADEON_GEM_CPU_ACCESS`;
  HOST_VISIBLE | HOST_COHERENT | HOST_CACHED | DEVICE_LOCAL.
- Type 1: `RADEON_GEM_DOMAIN_VRAM | GTT` + `RADEON_GEM_NO_CPU_ACCESS`;
  DEVICE_LOCAL only.

The kernel owns realized placement: for a multi-domain request TTM tries
VRAM, then GTT, then CPU (row `RADEON_BO_DOMAIN_PLACEMENT_ORDER`), so a
type-1 allocation may land in either pool and the driver treats the
domains field as a request, never as a residency claim.  Buffer and image
memory requirements admit type 0 alone
(`r3v_GetBufferMemoryRequirements2`): the submission-time gather and the
transfer path read every bound object through a CPU mapping, and type 1
allocates unmappable.

## Allocation and the single-BO ceiling

`r3v_AllocateMemory` creates exactly one GEM BO per `VkDeviceMemory`
through `radeon_drm_vk_bo_create` (`DRM_RADEON_GEM_CREATE`); a nonzero
ioctl result maps to `VK_ERROR_OUT_OF_DEVICE_MEMORY` with no partial
state.  The kernel bounds any single BO by effective gtt_size minus
pinned GTT bytes, because VRAM-system migration itself rides the GART
(row `RADEON_SINGLE_BO_GTT_CEILING`); an oversized request returns
`-ENOMEM` before any allocation.  Commitment is total at allocation: no
native type carries LAZILY_ALLOCATED, so `r3v_GetDeviceMemoryCommitment`
reports zero by the Vulkan definition.

## Migration and eviction

Migration policy is kernel-owned.  Per-IB relocation is bounded by the
anti-thrash threshold (row `RADEON_VRAM_RELOCATION_THRESHOLD`), pinned
bytes are debited from reported capacity (row
`RADEON_PINNED_CAPACITY_ACCOUNTING`), and usage/movement counters ride
`DRM_RADEON_GEM_INFO` / `DRM_RADEON_INFO` (row
`RADEON_CAPACITY_USAGE_AND_MOVE_COUNTERS`).  The driver never pins, so
its allocations remain evictable between submissions; the CS ioctl's
relocation list is the residency request for the one submission.

## Map, caching, and the coherency promise

`r3v_MapMemory` establishes at most one whole-BO mapping per memory
object (`DRM_RADEON_GEM_MMAP` + shared mmap through the transport ops
vtable) and returns `map + offset`; `vkUnmapMemory` and an implicit
unmap in `vkFreeMemory` retire it.  The cache contract is fixed by three
kernel facts: `radeon_bo_create` strips GTT WC/UC requests on non-PCIE
devices (row `NON_PCIE_GTT_ATTRIBUTE_NORMALIZATION`), TTM selects
`ttm_cached` (row `TTM_DEFAULT_CACHED_SELECTION`), and the sole
`RS480_AGP_MODE_CNTL` write retains `REQ_TYPE_SNOOP_DIS` (row
`RS480_GLOBAL_REQUEST_SNOOP_DISABLE`).  A cached CPU mapping over an
aperture whose requests are unsnooped means neither direction is
hardware-coherent; whether the per-PTE snoop request overrides the
global disable is open kernel-side
(`EFFECTIVE_PER_PTE_SNOOP_SEMANTICS`), so the driver assumes it does
not and holds the HOST_COHERENT promise itself with explicit cache-line
publication (`radeon_drm_vk_bo_cache_sync`: MFENCE-bounded CLFLUSH over
the mapped range, 64-byte K8 lines).

Publication and invalidation sites, all in the synchronous submit model:

- Publish: after every host write the device will read -- the deferred
  draw's carrier/vertex writes and load-op clears
  (`r3v_native_cell.c`), and over every live reference mapping
  immediately before `DRM_RADEON_CS`
  (`r3v_native_queue.c`; the transport snapshots
  `submit_boundary_sync_count` at the ioctl).
- Invalidate: after completion (`DRM_RADEON_GEM_WAIT_IDLE`) before any
  host read of device output, and at map establishment, because a fresh
  mapping may alias lines from an earlier map window
  (`r3v_MapMemory`).

`vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` validate
ranges and add no cache work: the one host-visible type reports
HOST_COHERENT, which the sites above discharge.  `nonCoherentAtomSize`
is 64, the same K8 cache-line granule the sync primitive walks.

## Binding, aliasing, and lifetime

`r3v_BindBufferMemory2` and `r3v_BindImageMemory2` own bind admission:
exactly once per object, offset aligned to `R3V_NATIVE_MEMORY_ALIGNMENT`,
object footprint closed inside the allocation, type 0 only.  Aliasing is
admitted by construction -- multiple objects may bind disjoint or
overlapping windows of one allocation, and the pass-target disjointness
proofs at recording refuse the aliases that would fold two relocation
roles into one BO range.  `VK_IMAGE_CREATE_ALIAS_BIT` names that
admission for images and `r3v_CreateImage` accepts it on the linear
transfer family, whose footprint `row_pitch_bytes * height` is the
aliasing window the requirement reports; the render family binds at
offset zero alone, so its one window covers the whole allocation and the
flag refuses there.  Sparse binding is unsupported and reports an empty
sparse-format table; buffers report dedicated allocation neither
preferred nor required (`r3v_native_fill_buffer_dedicated_requirements`)
and the render image family reports it required
(`r3v_GetImageMemoryRequirements2`).  External memory rides PRIME fd
export/import in the transport
(`radeon_drm_vk_bo_export_fd` / `_import_fd`) with a per-device
shared-handle reference count so `DRM_IOCTL_GEM_CLOSE` runs exactly once
per handle; cross-device external sync stays implicit `dma_resv`, the
only mechanism the radeon KMD offers.  Freeing memory unmaps (with a
final publication so BO reuse cannot expose dirty lines), then closes
the GEM handle; destroying a still-bound buffer or image is legal
because the binding stores no back-reference the free path must chase.

## Completion and failure classification

The queue's synchronous model completes every submission before
`vkQueueSubmit` returns: `DRM_RADEON_CS` then `DRM_RADEON_GEM_WAIT_IDLE`
through the same ops vtable.  A refusal before the deferred draw leaves
every mapped byte untouched and reports through the recorded
`VkResult`; a transport failure after the ioctl is classified as device
loss (`vk_queue_set_lost`), never silently retried, and never re-executed
on the CPU.  The submit-order harness proves that matrix; the memory
harness proves the allocation/map/flush/bind failure surface.
