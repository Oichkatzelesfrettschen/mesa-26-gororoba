# Headless hardware-GL runner on display-manager hosts

Conformance and driver work needs a hardware GL provider. A GLX client cannot
upgrade itself: when the X server's GLX provider is `DRISWRAST`, every client on
that display gets software regardless of `MESA_LOADER_DRIVER_OVERRIDE` or any
`LIBGL_*` flag it sets. The server has to expose glamor, so the fix belongs to
the server, not the client.

Display-manager hosts make that harder in three ways. The DM holds the DRM
master, so a second X server on the same GPU cannot become master. Its own X
server is often configured without glamor. And Arch-derived hosts commonly set
`dmesg_restrict=1`, which puts kernel-log evidence behind `sudo`.

## Reach the hardware without an X server first

`EGL` plus `GBM` on `/dev/dri/renderD128` reaches the hardware driver with no X
server and no DRM master, because the render node is world-accessible. Run
everything there that does not need a window-system default framebuffer:
`PIGLIT_PLATFORM=gbm`, or `eglGetPlatformDisplayEXT` with
`EGL_PLATFORM_GBM_KHR`. Tests requiring a real default framebuffer or an MSAA
window are the only ones that need the headless X path below.

## Headless X path

Free the GPU, since the DM owns the DRM master. Check
`display-manager.service` for which one is live, then `sudo systemctl stop <dm>`.

Start a headless server with a hardware provider on a free display and VT:

```sh
sudo Xorg :2 -config /path/glamor.conf -nolisten tcp vt9 -novtswitch -keeptty &
```

The config sets `Driver "modesetting"` with glamor enabled.
`Option "AccelMethod" "none"` disables glamor and forces `DRISWRAST`, so it stays
out. Run no window manager and no compositor: a bare X issues no XRender 2D
shaders, which is what keeps glamor clear of per-GPU shader limits.

Confirm the provider is hardware before trusting any result:

```sh
grep -iE "glamor|GLX|AIGLX|DRISWRAST" /var/log/Xorg.2.log
DISPLAY=:2 glxinfo | grep -i "OpenGL renderer"
```

A `DRISWRAST GL provider` line, or an `llvmpipe` or `swrast` renderer string,
means glamor did not come up. Fix the config; a suite run on software reports a
software result whatever the driver under test does.

Run the suite against `DISPLAY=:2`. Under `dmesg_restrict=1` both `dmesg` and
piglit `--dmesg` need `sudo`. Sample the GPU reset count before and after, and
abort the run on a reset.

Tear down and restore the session:

```sh
sudo pkill -f "Xorg :2"
sudo systemctl start <dm>
```

## Silicon and evidence caveats

Old and limited GPUs hang under glamor when a 2D or XRender shader exceeds a
hardware limit, such as a 104-ALU shader on a 64-ALU-max part. The headless
no-compositor session avoids the usual trigger, and GL test rendering reaches
Mesa directly rather than through glamor. Watch dmesg anyway and keep a reboot
path.

A client pointed at a hardware-only Mesa build, one without swrast, is a useful
probe: it either uses the hardware driver or fails loudly, so it cannot fall back
to llvmpipe and mask a software result.

A software-GLX run supports no conclusion about the driver. Confirm the renderer
string first, then read the result.
