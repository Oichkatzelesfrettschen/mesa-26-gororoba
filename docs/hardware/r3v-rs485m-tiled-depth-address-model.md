# RS485M ordinary tiled depth address model

The common resolver implements a bounded, uncompressed Z24/S8 address model.
Selecting the arithmetic supports qualification; public Vulkan format support
requires the attachment, transfer, and synchronization implementations as well.

## Provenance and measured scope

The source pair is Mesa `a0a2189bf178a8a06aa42dad7f9b10fdc6fe0cab` and
steinmarder-r300 `5b46bc0581fc8ca144aba06f4b8ad525fdd8d6ef`.
The latter owns `analysis/zb-coordinate-bit-address-observations/` and
`analysis/zb-coordinate-lane-outer-observations/`, including sealed raw captures.
Mesa carries 26 macrotiled and eight microtiled factual vectors in
`src/amd/r300/common/tests/r300_zb_rs485m_address_test.c`.
Ordinary builds require only the public vectors and common source.

The observations measure isolated writes on RS485M, PCI `1002:5974`, subsystem
`1028:022a`, with one sample and compression disabled. The principal geometry
is 64x64 at pitch 64 and surface base 2048. Pitch 96 and base 4096 are separate
positive discriminators. Their combined configuration is mathematical coverage.
Surface base means an offset within a BO; physical BO placement remains a
separate execution fact.

## Forward and inverse

Let `u=(x>>2)&7`, `v=(y>>1)&7`, `mx=x>>5`, and `my=y>>4`.
The local microblock bits are:

```text
l0 = u0
l1 = v0
l2 = u1 XOR v2
l3 = u2 XOR v1
l4 = u2 XOR (my & 1)
l5 = v2 XOR (mx & 1)
lane = 4*(y & 1) + (x & 3)
address = base + 2048*(my*(pitch/32)+mx) + 32*l + 4*lane
```

The local columns are `[1,4,24,2,8,36]`. The fixed-pitch derivation's nine
columns `[1,4,24,96,2,8,36,144,256]` include outer address bits and therefore
do not replace general pitch arithmetic.

After recovering the outer tile, local block, and lane from the byte offset:

```text
u0=l0; v0=l1
u2=l4 XOR (my&1); v2=l5 XOR (mx&1)
u1=l2 XOR v2; v1=l3 XOR u2
x=32*mx+4*u+(lane&3)
y=16*my+2*v+(lane>>2)
```

The inverse labels padding separately from logical pixels. The 65-row parser
allocation rounds to 80 stored rows under macrotiling, while rendering remains
64 rows. Both guards and the unclaimed tail sit outside the storage envelope.
Refused address operations leave their output untouched.

Enumeration of 25,600 storage words establishes bijection and round trips for
the tested mathematical configurations. Agreement with the retained vectors
establishes agreement at those measured coordinates. GPU nonuniform read masks,
physical write scans, and image switching must establish broader execution.

## Public image obligations

The acceptance baseline is Vulkan 1.0 without maintenance1. The checked-in
registry is version 1.4.354, imported by
`8d17c7e282a7b292534931d6ea0063e4223a8c4b`;
`src/vulkan/registry/vk.xml` has SHA256
`80e7394d0e787d6ec78b67aa324add6f96129fdd042ba640cc336a5481a208ee`.
The registry identifies entrypoints and enums; normative behavior comes from
the corresponding Vulkan specification sections on formats, depth/stencil
tests, clears, copies, and synchronization.

| Operation | Required implementation before public admission |
| --- | --- |
| Image creation and binding | Format precision, actual allocation bounds, mip/layer/sample and usage validation |
| Attachment use | Independent depth/stencil load/store operations; shader-dependent early/late tests |
| Depth/stencil state | Every compare/op, masks, disabled-write semantics, primitive-order preservation |
| Image clear | Selected aspects and subresource ranges; preserve the other packed component |
| Attachment clear | Selected aspects, rectangles, and layers within render-pass use |
| Transfer | Bit-preserving image/buffer copies and clears required by the advertised API/features |
| Barrier/dependency | Actual Z-cache publication and consumer invalidation for the declared access scopes |
| Readback | Image-to-buffer route; optimal tiling stays opaque |

The experimental 64x64 geometry is an implementation bound, not a substitute
for Vulkan minimum limits. CPU packing and observation are qualification work;
host semantic copies do not qualify a GPU-only transfer route. Focused CTS
outcomes remain unexecuted until these public paths exist.

ZMASK follows ordinary image correctness. Metadata ownership, clear values,
materialization, and image switching must preserve logical contents before
compression can participate in any admitted operation.
