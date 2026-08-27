# Headless hardware GL on display-manager hosts

Conformance evidence binds every result to one target device, one Mesa build,
and one window-system provider. A render-node EGL/GBM run exercises a direct
client without DRM master. An X11 run adds Xorg, its AIGLX provider, and the
direct-rendering client path. These paths produce distinct evidence classes.

`DRISWRAST` identifies the server-side AIGLX provider. It disqualifies indirect
GLX results as target-hardware evidence. A DRI3 client can still open a render
node and select a hardware client driver, so its renderer and loader trace
determine the direct-client identity. Retain both observations.

## Bind the render node to the target PCI device

Set the target PCI BDF, the expected renderer pattern, and an evidence directory
before selecting a provider. Place retained hardware evidence in the applicable
`steinmarder-r300` result bundle.

```sh
TARGET_PCI_BDF=${TARGET_PCI_BDF:?set a domain:bus:device.function PCI BDF}
EXPECTED_RENDERER_PATTERN=${EXPECTED_RENDERER_PATTERN:?set a renderer pattern}
EVIDENCE_DIR=${EVIDENCE_DIR:?set an absolute retained-evidence directory}

case "$EVIDENCE_DIR" in
   /*) ;;
   *) printf 'evidence directory must be absolute: %s\n' \
      "$EVIDENCE_DIR" >&2; exit 2 ;;
esac
[ ! -e "$EVIDENCE_DIR" ] || {
   printf 'evidence directory already exists: %s\n' "$EVIDENCE_DIR" >&2
   exit 2
}

if ! printf '%s\n' "$TARGET_PCI_BDF" | grep -Eq \
   '^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$'
then
   printf 'invalid PCI BDF: %s\n' "$TARGET_PCI_BDF" >&2
   exit 2
fi
mkdir -m 0700 "$EVIDENCE_DIR"

target_render_sysfs=
for render_sysfs_candidate in \
   "/sys/bus/pci/devices/$TARGET_PCI_BDF"/drm/renderD*
do
   [ -e "$render_sysfs_candidate" ] || continue
   if [ -n "$target_render_sysfs" ]; then
      printf 'multiple render nodes for %s\n' "$TARGET_PCI_BDF" >&2
      exit 2
   fi
   target_render_sysfs=$render_sysfs_candidate
done
[ -n "$target_render_sysfs" ] || {
   printf 'no render node for %s\n' "$TARGET_PCI_BDF" >&2
   exit 2
}

TARGET_RENDER_NODE=/dev/dri/${target_render_sysfs##*/}
target_render_device=$(readlink -f \
   "/sys/class/drm/${TARGET_RENDER_NODE##*/}/device")
target_pci_device=$(readlink -f "/sys/bus/pci/devices/$TARGET_PCI_BDF")
[ "$target_render_device" = "$target_pci_device" ] || exit 2

lspci -Dnnks "$TARGET_PCI_BDF" > "$EVIDENCE_DIR/device-identity.txt"
stat -Lc '%n %t:%T %a %U:%G' "$TARGET_RENDER_NODE" \
   > "$EVIDENCE_DIR/render-node-access.txt"
getfacl -cp "$TARGET_RENDER_NODE" \
   >> "$EVIDENCE_DIR/render-node-access.txt"
if [ ! -r "$TARGET_RENDER_NODE" ] || \
   [ ! -w "$TARGET_RENDER_NODE" ] || \
   ! : <> "$TARGET_RENDER_NODE"
then
   printf '%s requires read-write access for user %s\n' \
      "$TARGET_RENDER_NODE" "$(id -un)" >&2
   exit 2
fi
```

The render-node group or a seat ACL grants access. Add the test user to the
host's designated render group or install a target-scoped ACL, then establish a
new login session and repeat the preflight. Run the GL client as that user;
root execution changes the loader environment and the runtime identity.

Derive Mesa's PCI selection tag from the same BDF. `DRI_PRIME_DEBUG=1` records
the selected render node, while `wflinfo` records the renderer.

```sh
TARGET_DRI_PRIME="pci-$(printf '%s' "$TARGET_PCI_BDF" | tr ':.' '__')"
DRI_PRIME="$TARGET_DRI_PRIME" DRI_PRIME_DEBUG=1 \
   wflinfo --platform gbm --api gl --verbose \
   > "$EVIDENCE_DIR/gbm-renderer.txt" \
   2> "$EVIDENCE_DIR/gbm-loader-selection.txt"
grep -F "selected ($TARGET_RENDER_NODE)" \
   "$EVIDENCE_DIR/gbm-loader-selection.txt"
grep -Ei -- "$EXPECTED_RENDERER_PATTERN" "$EVIDENCE_DIR/gbm-renderer.txt"
```

Both checks form the GBM admission gate. A missing selection line, a different
render node, or a renderer mismatch rejects the run as target-device evidence.

Route platform-neutral EGL, GBM, and surfaceless cases through this path. Route
every GLX, EGL-X11, Xlib, XCB, X protocol, context, visual, drawable, and
default-framebuffer case through the X path. GBM coverage never substitutes for
an X-window-system result.

## Establish an independent recovery session

Enter through SSH from another machine or a text VT. Keep that session open
until the display manager is restored. A terminal owned by the graphical
session exits when the display manager stops and cannot provide recovery.

```sh
if [ -n "${SSH_CONNECTION:-}" ]; then
   CONTROL_SESSION=ssh
elif [ "${XDG_SESSION_TYPE:-}" = tty ] && \
     [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
   CONTROL_SESSION=text-vt
else
   printf 'use remote SSH or an independent text VT\n' >&2
   exit 2
fi
printf '%s\n' "$CONTROL_SESSION" > "$EVIDENCE_DIR/control-session.txt"

DISPLAY_MANAGER_UNIT=display-manager.service
systemctl show "$DISPLAY_MANAGER_UNIT" \
   --property=Id --property=Names --property=FragmentPath \
   > "$EVIDENCE_DIR/display-manager-unit.txt"
systemctl is-active --quiet "$DISPLAY_MANAGER_UNIT" || exit 2
printf 'sudo systemctl start %s\n' "$DISPLAY_MANAGER_UNIT" \
   > "$EVIDENCE_DIR/display-manager-restore-command.txt"
```

The retained restoration command stays available in the independent session.
Stopping the display manager is permitted only after these checks pass.

## Create a target-scoped Xorg configuration

Xorg expresses PCI bus, domain, device, and function numbers in decimal. Derive
that identity from the same validated BDF and materialize the exact
configuration in the evidence directory.

```sh
target_pci_domain_hex=${TARGET_PCI_BDF%%:*}
target_pci_suffix=${TARGET_PCI_BDF#*:}
target_pci_bus_hex=${target_pci_suffix%%:*}
target_pci_slot_function=${target_pci_suffix#*:}
target_pci_device_hex=${target_pci_slot_function%%.*}
target_pci_function_hex=${target_pci_slot_function#*.}
TARGET_XORG_BUS_ID=$(printf 'PCI:%d@%d:%d:%d' \
   "0x$target_pci_bus_hex" "0x$target_pci_domain_hex" \
   "0x$target_pci_device_hex" "0x$target_pci_function_hex")
XORG_CONFIG_PATH=$EVIDENCE_DIR/xorg-target.conf

sed "s|@TARGET_XORG_BUS_ID@|$TARGET_XORG_BUS_ID|g" \
   > "$XORG_CONFIG_PATH" <<'EOF'
Section "ServerFlags"
    Option "AutoAddGPU" "off"
EndSection

Section "Device"
    Identifier "Target GPU"
    Driver "modesetting"
    BusID "@TARGET_XORG_BUS_ID@"
    Option "AccelMethod" "glamor"
EndSection

Section "Screen"
    Identifier "Target Screen"
    Device "Target GPU"
EndSection

Section "ServerLayout"
    Identifier "Target Layout"
    Screen "Target Screen"
    Option "IsolateDevice" "@TARGET_XORG_BUS_ID@"
EndSection
EOF
```

`BusID`, `AutoAddGPU`, and `IsolateDevice` keep discovery and reset ownership on
the selected GPU. The modesetting driver initializes glamor explicitly. A bare
server runs without a window manager or compositor, which removes their XRender
workloads from the hardware-conformance session.

## Launch and verify the X provider

Stop the display manager, then launch Xorg as a transient service. `Type=exec`
makes launch success depend on executing Xorg. The system service manager owns
the process after the initiating command returns. The display, VT, and unit
preflight prevents collision with an existing graphical service.

```sh
XORG_DISPLAY=:2
XORG_DISPLAY_NUMBER=${XORG_DISPLAY#:}
XORG_VT=vt9
XORG_VT_NUMBER=${XORG_VT#vt}
XORG_RUN_ID=$(tr -d '-' < /proc/sys/kernel/random/uuid)
if ! printf '%s\n' "$XORG_RUN_ID" | grep -Eq '^[[:xdigit:]]{32}$'; then
   printf 'invalid Xorg run identity: %s\n' "$XORG_RUN_ID" >&2
   exit 2
fi
XORG_UNIT=mesa-headless-gl-target-$XORG_RUN_ID.service
XORG_LOG=$EVIDENCE_DIR/xorg-provider.log
printf '%s\n' "$XORG_UNIT" > "$EVIDENCE_DIR/xorg-unit-name.txt"

xorg_unit_name_snapshot=$(systemctl show "$XORG_UNIT" \
   --property=LoadState --property=ActiveState)
xorg_unit_name_load_state=$(printf '%s\n' "$xorg_unit_name_snapshot" | \
   sed -n 's/^LoadState=//p')
xorg_unit_name_active_state=$(printf '%s\n' "$xorg_unit_name_snapshot" | \
   sed -n 's/^ActiveState=//p')

if [ -e "/tmp/.X11-unix/X$XORG_DISPLAY_NUMBER" ] || \
   [ -e "/tmp/.X$XORG_DISPLAY_NUMBER-lock" ] || \
   systemctl is-active --quiet "getty@tty$XORG_VT_NUMBER.service" || \
   [ "$xorg_unit_name_load_state:$xorg_unit_name_active_state" != \
     not-found:inactive ]
then
   printf 'Xorg display, VT, or run identity is already owned\n' >&2
   exit 2
fi

sudo systemctl stop "$DISPLAY_MANAGER_UNIT"
if systemctl is-active --quiet "$DISPLAY_MANAGER_UNIT"; then
   printf 'display manager remains active\n' >&2
   exit 2
fi

sudo systemd-run --unit="$XORG_UNIT" --property=Type=exec \
   -- Xorg "$XORG_DISPLAY" -config "$XORG_CONFIG_PATH" \
   -logfile "$XORG_LOG" -nolisten tcp "$XORG_VT" -novtswitch -keeptty

xorg_launch_snapshot=$(sudo systemctl show "$XORG_UNIT" \
   --property=LoadState --property=ActiveState --property=SubState \
   --property=MainPID --property=InvocationID)
XORG_LOAD_STATE=$(printf '%s\n' "$xorg_launch_snapshot" | \
   sed -n 's/^LoadState=//p')
XORG_ACTIVE_STATE=$(printf '%s\n' "$xorg_launch_snapshot" | \
   sed -n 's/^ActiveState=//p')
XORG_MAIN_PID=$(printf '%s\n' "$xorg_launch_snapshot" | \
   sed -n 's/^MainPID=//p')
XORG_INVOCATION_ID=$(printf '%s\n' "$xorg_launch_snapshot" | \
   sed -n 's/^InvocationID=//p')
[ "$XORG_LOAD_STATE:$XORG_ACTIVE_STATE" = loaded:active ] || {
   printf 'Xorg unit did not reach loaded:active: %s:%s\n' \
      "$XORG_LOAD_STATE" "$XORG_ACTIVE_STATE" >&2
   exit 2
}
case "$XORG_MAIN_PID" in
   ''|*[!0-9]*|0|1) printf 'invalid Xorg MainPID: %s\n' \
      "$XORG_MAIN_PID" >&2; exit 2 ;;
esac
if ! printf '%s\n' "$XORG_INVOCATION_ID" | \
   grep -Eq '^[[:xdigit:]]{32}$'
then
   printf 'invalid Xorg InvocationID: %s\n' "$XORG_INVOCATION_ID" >&2
   exit 2
fi
printf '%s\n' "$xorg_launch_snapshot" \
   > "$EVIDENCE_DIR/xorg-unit-state.txt"
printf '%s\n' "$XORG_MAIN_PID" > "$EVIDENCE_DIR/xorg-main-pid.txt"
printf '%s\n' "$XORG_INVOCATION_ID" \
   > "$EVIDENCE_DIR/xorg-invocation-id.txt"
sudo cat "/proc/$XORG_MAIN_PID/cmdline" | tr '\0' ' ' \
   > "$EVIDENCE_DIR/xorg-command-line.txt"

xorg_ready_attempts=30
until DISPLAY="$XORG_DISPLAY" xdpyinfo >/dev/null 2>&1; do
   xorg_ready_snapshot=$(systemctl show "$XORG_UNIT" \
      --property=LoadState --property=ActiveState --property=MainPID \
      --property=InvocationID)
   xorg_ready_load_state=$(printf '%s\n' "$xorg_ready_snapshot" | \
      sed -n 's/^LoadState=//p')
   xorg_ready_active_state=$(printf '%s\n' "$xorg_ready_snapshot" | \
      sed -n 's/^ActiveState=//p')
   xorg_ready_main_pid=$(printf '%s\n' "$xorg_ready_snapshot" | \
      sed -n 's/^MainPID=//p')
   xorg_ready_invocation_id=$(printf '%s\n' "$xorg_ready_snapshot" | \
      sed -n 's/^InvocationID=//p')
   if [ "$xorg_ready_load_state:$xorg_ready_active_state" != loaded:active ] || \
      [ "$xorg_ready_main_pid" != "$XORG_MAIN_PID" ] || \
      [ "$xorg_ready_invocation_id" != "$XORG_INVOCATION_ID" ]
   then
      printf 'Xorg transient-unit identity changed before readiness\n' >&2
      exit 2
   fi
   if [ "$xorg_ready_attempts" -le 0 ]; then
      printf 'Xorg display readiness timed out\n' >&2
      exit 2
   fi
   xorg_ready_attempts=$((xorg_ready_attempts - 1))
   sleep 1
done
```

The exact random unit name, `MainPID`, systemd `InvocationID`, unit state, and
command line prove which server this run owns. The 128-bit name is created once
inside the mode-0700 evidence directory and is never reused. That run-scoped
unit capability and its live invocation define the only teardown target.

Capture the server provider and direct-client identities separately. The GLX
and EGL-X11 checks must select the same target render node and match the expected
renderer.

```sh
grep -iE 'glamor|GLX|AIGLX|DRISWRAST' "$XORG_LOG" \
   > "$EVIDENCE_DIR/xorg-provider-selection.txt"

for waffle_platform in glx x11_egl
do
   DISPLAY="$XORG_DISPLAY" DRI_PRIME="$TARGET_DRI_PRIME" DRI_PRIME_DEBUG=1 \
      wflinfo --platform "$waffle_platform" --api gl --verbose \
      > "$EVIDENCE_DIR/$waffle_platform-renderer.txt" \
      2> "$EVIDENCE_DIR/$waffle_platform-loader-selection.txt"
   grep -F "selected ($TARGET_RENDER_NODE)" \
      "$EVIDENCE_DIR/$waffle_platform-loader-selection.txt"
   grep -Ei -- "$EXPECTED_RENDERER_PATTERN" \
      "$EVIDENCE_DIR/$waffle_platform-renderer.txt"
done
```

A `DRISWRAST` AIGLX line rejects indirect GLX results as hardware evidence. The
direct GLX and EGL-X11 results retain their independently verified client
identity. An `llvmpipe` or `swrast` client renderer rejects that client run.

## Run the suite without privilege

Capture kernel state through dedicated privileged commands. Run Piglit and
other test clients in the configured user environment so `LD_LIBRARY_PATH`,
`LIBGL_DRIVERS_PATH`, `MESA_LOADER_DRIVER_OVERRIDE`, `DRI_PRIME`, and the Piglit
platform keep their intended values.

```sh
# The unprivileged shell owns the evidence file. Privilege covers dmesg only.
# shellcheck disable=SC2024
sudo dmesg --color=never > "$EVIDENCE_DIR/kernel-before.txt"

PIGLIT_PROFILE=${PIGLIT_PROFILE:?set a Piglit profile}
TARGET_PIGLIT_PLATFORM=${TARGET_PIGLIT_PLATFORM:?set glx or x11_egl}
case "$TARGET_PIGLIT_PLATFORM" in
   glx|x11_egl) ;;
   *) printf 'invalid X-window-system Piglit platform: %s\n' \
      "$TARGET_PIGLIT_PLATFORM" >&2; exit 2 ;;
esac
DISPLAY="$XORG_DISPLAY" DRI_PRIME="$TARGET_DRI_PRIME" \
   PIGLIT_PLATFORM="$TARGET_PIGLIT_PLATFORM" \
   piglit run "$PIGLIT_PROFILE" "$EVIDENCE_DIR/piglit-results"
suite_status=$?

# shellcheck disable=SC2024
sudo dmesg --color=never > "$EVIDENCE_DIR/kernel-after.txt"
diff -u "$EVIDENCE_DIR/kernel-before.txt" \
   "$EVIDENCE_DIR/kernel-after.txt" \
   > "$EVIDENCE_DIR/kernel-movement.diff"
kernel_diff_status=$?
case "$kernel_diff_status" in
   0|1) ;;
   *) exit "$kernel_diff_status" ;;
esac
printf '%s\n' "$suite_status" > "$EVIDENCE_DIR/suite-status.txt"
```

`sudo` applies to the two `dmesg` captures and the Xorg lifecycle commands. It
never prefixes Piglit or a GL client. A new DRM validation rejection, GPU reset,
ring stall, lockup, or kernel fault invalidates the run and opens hardware RCA.

## Terminate the recorded server and restore the display manager

Verify that the live transient unit still owns the recorded invocation and PID,
request its stop through systemd, and wait for the unit to become inactive
before restoring the display manager. An inactive or removed unit means the
recorded server has already stopped. A live unit with another invocation or PID
fails closed without signaling either process.

```sh
XORG_UNIT=${XORG_UNIT:-$(cat "$EVIDENCE_DIR/xorg-unit-name.txt")}
if ! printf '%s\n' "$XORG_UNIT" | grep -Eq \
   '^mesa-headless-gl-target-[[:xdigit:]]{32}\.service$'
then
   printf 'invalid recorded Xorg unit name: %s\n' "$XORG_UNIT" >&2
   exit 2
fi
XORG_MAIN_PID=${XORG_MAIN_PID:-$(cat "$EVIDENCE_DIR/xorg-main-pid.txt")}
XORG_INVOCATION_ID=${XORG_INVOCATION_ID:-$(
   cat "$EVIDENCE_DIR/xorg-invocation-id.txt"
)}
case "$XORG_MAIN_PID" in
   ''|*[!0-9]*|0|1) printf 'invalid recorded Xorg MainPID: %s\n' \
      "$XORG_MAIN_PID" >&2; exit 2 ;;
esac
if ! printf '%s\n' "$XORG_INVOCATION_ID" | \
   grep -Eq '^[[:xdigit:]]{32}$'
then
   printf 'invalid recorded Xorg InvocationID: %s\n' \
      "$XORG_INVOCATION_ID" >&2
   exit 2
fi

xorg_unit_snapshot=$(systemctl show "$XORG_UNIT" \
   --property=LoadState --property=ActiveState --property=MainPID \
   --property=InvocationID)
xorg_load_state=$(printf '%s\n' "$xorg_unit_snapshot" | \
   sed -n 's/^LoadState=//p')
xorg_active_state=$(printf '%s\n' "$xorg_unit_snapshot" | \
   sed -n 's/^ActiveState=//p')
xorg_live_main_pid=$(printf '%s\n' "$xorg_unit_snapshot" | \
   sed -n 's/^MainPID=//p')
xorg_live_invocation_id=$(printf '%s\n' "$xorg_unit_snapshot" | \
   sed -n 's/^InvocationID=//p')

case "$xorg_load_state:$xorg_active_state" in
   not-found:inactive|loaded:inactive|loaded:failed)
      [ "$xorg_live_main_pid" = 0 ] || {
         printf 'inactive Xorg unit retains MainPID %s\n' \
            "$xorg_live_main_pid" >&2
         exit 2
      }
      ;;
   loaded:active|loaded:activating|loaded:reloading|loaded:deactivating)
      if [ "$xorg_live_main_pid" != "$XORG_MAIN_PID" ] || \
         [ "$xorg_live_invocation_id" != "$XORG_INVOCATION_ID" ]
      then
         printf 'Xorg transient-unit identity mismatch; refusing stop\n' >&2
         exit 2
      fi
      if [ "$xorg_active_state" != deactivating ]; then
         sudo systemctl stop --no-block "$XORG_UNIT"
      fi
      ;;
   *)
      printf 'unsupported Xorg unit state: %s:%s\n' \
         "$xorg_load_state" "$xorg_active_state" >&2
      exit 2
      ;;
esac

xorg_exit_attempts=30
while :
do
   xorg_unit_snapshot=$(systemctl show "$XORG_UNIT" \
      --property=LoadState --property=ActiveState --property=MainPID \
      --property=InvocationID)
   xorg_load_state=$(printf '%s\n' "$xorg_unit_snapshot" | \
      sed -n 's/^LoadState=//p')
   xorg_active_state=$(printf '%s\n' "$xorg_unit_snapshot" | \
      sed -n 's/^ActiveState=//p')
   xorg_live_main_pid=$(printf '%s\n' "$xorg_unit_snapshot" | \
      sed -n 's/^MainPID=//p')
   xorg_live_invocation_id=$(printf '%s\n' "$xorg_unit_snapshot" | \
      sed -n 's/^InvocationID=//p')
   case "$xorg_load_state:$xorg_active_state" in
      not-found:inactive|loaded:inactive|loaded:failed)
         [ "$xorg_live_main_pid" = 0 ] || {
            printf 'stopped Xorg unit retains MainPID %s\n' \
               "$xorg_live_main_pid" >&2
            exit 2
         }
         break
         ;;
      loaded:active|loaded:activating|loaded:reloading|loaded:deactivating)
         if [ "$xorg_live_invocation_id" != "$XORG_INVOCATION_ID" ]; then
            printf 'Xorg transient unit changed while stopping\n' >&2
            exit 2
         fi
         case "$xorg_live_main_pid" in
            0|"$XORG_MAIN_PID") ;;
            *) printf 'Xorg MainPID changed while stopping: %s\n' \
                  "$xorg_live_main_pid" >&2; exit 2 ;;
         esac
         ;;
      *)
         printf 'unsupported Xorg stop state: %s:%s\n' \
            "$xorg_load_state" "$xorg_active_state" >&2
         exit 2
         ;;
   esac
   if [ "$xorg_exit_attempts" -le 0 ]; then
      printf 'Xorg unit stop timed out for invocation %s\n' \
         "$XORG_INVOCATION_ID" >&2
      exit 2
   fi
   xorg_exit_attempts=$((xorg_exit_attempts - 1))
   sleep 1
done
printf '%s\n' "$xorg_unit_snapshot" \
   > "$EVIDENCE_DIR/xorg-unit-final-state.txt"
if [ "$xorg_load_state:$xorg_active_state" = loaded:failed ]; then
   sudo systemctl reset-failed "$XORG_UNIT"
fi
sudo systemctl start "$DISPLAY_MANAGER_UNIT"
systemctl is-active --quiet "$DISPLAY_MANAGER_UNIT"
```

The display manager restarts only after systemd reports that the recorded,
run-scoped unit invocation is inactive, failed with no main process, or removed.
PID reuse and another normal headless run have no effect on that decision. A
host with untrusted privileged software creating system units concurrently is
outside this operator procedure; suspend hardware testing on that host. If the
Xorg launch or suite aborts, execute this same unit-scoped restoration sequence
from the independent control session.

## Preserve silicon safety and negative controls

Limited GPUs can hang when a glamor 2D or XRender shader exceeds a silicon
limit, such as a 104-ALU shader on a 64-ALU-maximum part. The bare X server
removes window-manager and compositor workloads from this session. That
containment reduces the known trigger surface; kernel-log monitoring and an
independent reboot path remain mandatory.

A hardware-only Mesa build without swrast supplies an independent negative
control. The client either loads the expected hardware driver or fails at
provider creation, so llvmpipe cannot mask the result. This control complements
the loader-selection and renderer gates; it never substitutes for them.

## Retain evidence by class

Keep device identity, render-node access, loader selection, renderer identity,
Xorg provider selection, Xorg command line, run-scoped unit name, `MainPID`,
`InvocationID`, initial and final unit states, suite results, and kernel logs as
separate artifacts. Record the Mesa source SHA, build identity, tool paths,
tool versions, suite command, and environment allowlist beside them, then hash
the admitted bundle. Build success, provider identity, conformance movement,
and kernel or silicon behavior remain separate claims.
