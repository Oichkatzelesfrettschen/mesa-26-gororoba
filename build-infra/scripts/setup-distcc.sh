#!/bin/sh
# setup-distcc.sh -- recreate /tmp/distcc-wrap/ after an x130e reboot.
#
# Generates the canonical CC/CXX wrapper chain used by build-infra:
#
#     cc(++)  --exec-->  ccache  --exec-->  distcc  --exec-->  clang(++)-22
#
# ccache first so cache hits skip distcc entirely; distcc second so
# cache misses fan out across DESKTOP-CKP9KB6/32 + the MacBook Air.
# Idempotent; safe to run at any time.

set -eu

DIR="${DISTCC_WRAP_DIR:-/tmp/distcc-wrap}"
CC_REAL="${CC_REAL:-/usr/bin/clang-22}"
CXX_REAL="${CXX_REAL:-/usr/bin/clang++-22}"
CCACHE="${CCACHE:-/usr/bin/ccache}"
DISTCC="${DISTCC:-/usr/bin/distcc}"

[ -x "$CCACHE" ] || { echo "ccache not at $CCACHE; apt install ccache" >&2; exit 1; }
[ -x "$DISTCC" ] || { echo "distcc not at $DISTCC; apt install distcc" >&2; exit 1; }

mkdir -p "$DIR"

cat > "$DIR/cc" <<EOF
#!/bin/sh
exec $CCACHE $DISTCC $CC_REAL "\$@"
EOF

cat > "$DIR/c++" <<EOF
#!/bin/sh
exec $CCACHE $DISTCC $CXX_REAL "\$@"
EOF

chmod +x "$DIR/cc" "$DIR/c++"

echo "distcc+ccache wrappers ready at $DIR"
echo "  chain:  ccache -> distcc -> $CC_REAL"
echo "  hosts:  $(grep -v "^#" ~/.distcc/hosts 2>/dev/null | tr "\\n" " ")"
