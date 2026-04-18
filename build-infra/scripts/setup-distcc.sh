#!/bin/sh
# setup-distcc.sh -- recreate /tmp/distcc-wrap/ after an x130e reboot.
# /tmp is cleared on reboot, so the clang wrappers vanish.  This script
# re-materialises them.  Idempotent.

set -eu

DIR="${DISTCC_WRAP_DIR:-/tmp/distcc-wrap}"
CC_REAL="${CC_REAL:-/usr/bin/clang-22}"
CXX_REAL="${CXX_REAL:-/usr/bin/clang++-22}"

mkdir -p "$DIR"

cat > "$DIR/cc" <<EOF
#!/bin/sh
exec distcc $CC_REAL "\$@"
EOF

cat > "$DIR/c++" <<EOF
#!/bin/sh
exec distcc $CXX_REAL "\$@"
EOF

chmod +x "$DIR/cc" "$DIR/c++"

echo "distcc wrappers ready at $DIR (CC=$CC_REAL CXX=$CXX_REAL)"
echo "hosts: $(grep -v "^#" ~/.distcc/hosts 2>/dev/null | tr "\n" " ")"
