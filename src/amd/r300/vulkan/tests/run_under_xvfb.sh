#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs its argv command under a freshly started Xvfb and reports the
# command's own exit status undisturbed by Xvfb lifecycle problems.
#
# xvfb-run -a allocates a display number by scanning the host-wide
# /tmp/.X11-unix lock namespace; concurrent invocations can settle on the
# same number, and the loser's own cleanup kill then becomes the reported
# test status. -displayfd removes the race: Xvfb itself picks a free
# display and writes the number to the given file descriptor only once it
# is accepting connections, so reading that descriptor is both the display
# allocation and the readiness signal, with no separate poll loop needed.
#
# Xvfb startup failure and displayfd timeout exit 125. 125 is outside the
# range a test binary produces under this harness (0 for pass, 77 for a
# Meson skip, any other value an ordinary failure), so a Meson
# should_fail test can never be satisfied by an infrastructure failure
# instead of the intended binary behavior, and 125 never reads as a skip.
#
# Once Xvfb accepts connections, the wrapper's exit status is exactly the
# wrapped command's exit status. A failure while stopping Xvfb afterward is
# reported on stderr and does not change that status.

set -u

xvfb_bin=${R3V_XVFB_BINARY:-Xvfb}

if [ "$#" -eq 0 ]; then
    echo "run_under_xvfb.sh: no command given" >&2
    exit 125
fi

if ! command -v "$xvfb_bin" >/dev/null 2>&1; then
    echo "run_under_xvfb.sh: Xvfb binary '$xvfb_bin' not found" >&2
    exit 125
fi

displayfd_dir=$(mktemp -d "${TMPDIR:-/tmp}/r3v-xvfb.XXXXXX") || {
    echo "run_under_xvfb.sh: mktemp -d failed" >&2
    exit 125
}
displayfd_pipe="$displayfd_dir/displayfd"

cleanup_dir() {
    rm -rf "$displayfd_dir"
}

if ! mkfifo -m 600 "$displayfd_pipe" 2>/dev/null; then
    echo "run_under_xvfb.sh: mkfifo failed" >&2
    cleanup_dir
    exit 125
fi

# -displayfd wants an open file descriptor number, so the fifo is opened on
# fd 9 in the Xvfb subshell and Xvfb writes the chosen display number into
# it once (and only once) it is ready to accept connections.
"$xvfb_bin" -displayfd 9 9>"$displayfd_pipe" &
xvfb_pid=$!

display_num=""
exec 8<"$displayfd_pipe"
# read is the readiness gate: Xvfb writes only after it can accept
# connections, so a value on fd 8 means the server is usable. A bounded
# timeout guards a hung or wedged Xvfb that never writes.
# The bounded wait is load-bearing: a wedged Xvfb that never writes the
# display number must surface as infrastructure status 125, so a missing
# timeout utility refuses up front instead of falling back to an
# unbounded read.
if ! command -v timeout >/dev/null 2>&1; then
    echo "run_under_xvfb.sh: timeout utility not found" >&2
    kill "$xvfb_pid" 2>/dev/null
    wait "$xvfb_pid" 2>/dev/null
    exec 8<&-
    cleanup_dir
    exit 125
fi
# shellcheck disable=SC2016  # $n is the inner sh -c's variable, not this shell's.
display_num=$(timeout 15 sh -c 'read -r n <&8 && echo "$n"' 8<&8)
read_status=$?
exec 8<&-

if [ "$read_status" -ne 0 ] || [ -z "$display_num" ]; then
    echo "run_under_xvfb.sh: Xvfb did not report a display on displayfd" >&2
    kill "$xvfb_pid" 2>/dev/null
    wait "$xvfb_pid" 2>/dev/null
    cleanup_dir
    exit 125
fi

if ! kill -0 "$xvfb_pid" 2>/dev/null; then
    echo "run_under_xvfb.sh: Xvfb exited before startup completed" >&2
    cleanup_dir
    exit 125
fi

DISPLAY=":$display_num"
export DISPLAY

"$@"
cmd_status=$?

kill -TERM "$xvfb_pid" 2>/dev/null
wait_count=0
while kill -0 "$xvfb_pid" 2>/dev/null; do
    if [ "$wait_count" -ge 50 ]; then
        echo "run_under_xvfb.sh: Xvfb did not exit after TERM, sending KILL" >&2
        kill -KILL "$xvfb_pid" 2>/dev/null
        break
    fi
    wait_count=$((wait_count + 1))
    sleep 0.1
done
wait "$xvfb_pid" 2>/dev/null
xvfb_wait_status=$?
if [ "$xvfb_wait_status" -ne 0 ] && [ "$xvfb_wait_status" -ne 143 ]; then
    echo "run_under_xvfb.sh: Xvfb cleanup exited with status $xvfb_wait_status" >&2
fi

cleanup_dir

exit "$cmd_status"
