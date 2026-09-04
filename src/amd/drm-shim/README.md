### radeon_noop backend

This implements the minimum of the radeon kernel driver in order to make shader-db work.
The submit ioctl is stubbed out to not execute anything.

Export `MESA_LOADER_DRIVER_OVERRIDE=r300
LD_PRELOAD=$prefix/lib/libradeon_noop_drm_shim.so`. (or r600 for r600-class HW)

By default, rv515 is exposed.  The chip can be selected with an environment
variable like `RADEON_GPU_ID=CAYMAN` or `RADEON_GPU_ID=0x6740`.  A numeric ID
uses exactly four hexadecimal digits and must name an entry in Mesa's Radeon
PCI tables.  The shim uses the selected device and family for the PCI device,
`RADEON_INFO_DEVICE_ID`, and family-specific information queries.
The selected PCI ID and family dispatch carry exact Mesa table identity.  The
PCI slot, subsystem IDs, reported memory capacities, pipe counts, clocks,
queue state, and counters are synthetic deterministic values.  The noop device
models register reads as unsupported, so `RADEON_INFO_READ_REG` returns
`-EINVAL`.  The initialized shim hides the RS480 GART debugfs paths from libc
pathname queries through absolute, relative, normalized, and canonical
aliases.  Memory usage counters return zero, and valid buffer busy queries
report idle in their creation domain because the stubbed command stream
completes without a hardware queue.  Runs through this shim provide source and
runtime evidence.  A silicon claim requires separate execution on the named
Radeon device.

### amdgpu_noop backend

This implements the minimum of the amdgpu kernel driver.  The submit ioctl is
stubbed out to not execute anything.

Export `LD_PRELOAD=$prefix/lib/libamdgpu_noop_drm_shim.so`.

To specify the device to expose, set the environment variable `AMDGPU_GPU_ID`
to

 - `renoir` to expose a `CHIP_RENOIR` device
 - `raven` to expose a `CHIP_RAVEN` device
 - `stoney` to expose a `CHIP_STONEY` device

Further names follow the `CHIP_*` enum values. By default, the `CHIP_RENOIR`
device is exposed.

To add a new device, `amdgpu_devices.c` needs to be modified.
`amdgpu_dump_states` can be used to dump the relevant states from a real
device.
