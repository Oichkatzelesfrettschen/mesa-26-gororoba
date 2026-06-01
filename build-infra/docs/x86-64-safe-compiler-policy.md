# x86-64 safe compiler policy

This repo builds artifacts that move between CachyOS and Debian hosts and must
run on the Vostro K8 floor.  The default must therefore describe the machine
contract, not the build host.

## Default

Use this baseline for portable Mesa and r300vk artifacts:

```sh
CPPFLAGS="-D_FORTIFY_SOURCE=2"
CFLAGS="-march=x86-64 -mtune=generic -O2 -fexceptions -fstack-protector-strong -fstack-clash-protection -fcf-protection -Wformat -Werror=format-security"
CXXFLAGS="$CFLAGS -Wp,-D_GLIBCXX_ASSERTIONS"
LDFLAGS="-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro"
RUSTFLAGS="-C target-cpu=x86-64"
```

The same compile-only probe accepts these flags with GCC and Clang on the local
CachyOS host, `cachyos-vostro1000`, and Debian `x130e`.

## Non-default local choices

Do not put these in canonical package, Meson, or hostenv defaults:

- `-march=native`, `-mtune=native`, `x86-64-v2`, `x86-64-v3`, or `x86-64-v4`:
  they encode the build host rather than the runtime floor.
- `-pipe`: it only changes compiler temporary-file I/O and can increase memory
  pressure on small hosts.
- LTO: keep package and conformance lanes non-LTO unless a measurement branch
  proves the exact profile and linker combination.
- `-O3` or `-Ofast`: use only for measured experiments because code size and
  floating-point semantics can move.
- `-fno-plt` and `-Wl,-z,now`: useful hardening or startup tradeoffs in some
  distro profiles, but not part of the portable default until measured on the
  Vostro runtime lane.
- Forced linker flags such as `-fuse-ld=lld` or `-fuse-ld=mold`: select faster
  linkers with `CC_LD` and `CXX_LD` when the build host has them; do not bake
  them into portable codegen flags.
