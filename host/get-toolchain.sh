#!/bin/sh
# Fetch the m68k cross toolchain (GCC 14, binutils, gdb-multiarch) from the
# host's Debian/Ubuntu apt repositories and unpack it into a private prefix,
# without root. Wrapper scripts in $PREFIX/bin set the library path.
#
# usage: host/get-toolchain.sh [PREFIX]     (default: ~/opt/m68k-cross)
#
# With root, "apt install gcc-m68k-linux-gnu gdb-multiarch" and
# "make CROSS=m68k-linux-gnu-" works as well.
set -eu

PREFIX=${1:-$HOME/opt/m68k-cross}
GCCVER=14
PKGS="binutils-m68k-linux-gnu gcc-$GCCVER-m68k-linux-gnu cpp-$GCCVER-m68k-linux-gnu
      libgcc-$GCCVER-dev-m68k-cross gdb-multiarch"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
(cd "$tmp" && apt-get download $PKGS)
mkdir -p "$PREFIX/bin"
for deb in "$tmp"/*.deb; do
	dpkg-deb -x "$deb" "$PREFIX"
done

libdir=$PREFIX/usr/lib/x86_64-linux-gnu
wrap() {	# wrap <tool-in-usr/bin> <wrapper-name>
	cat > "$PREFIX/bin/$2" <<EOF
#!/bin/sh
LD_LIBRARY_PATH=$libdir\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} exec $PREFIX/usr/bin/$1 "\$@"
EOF
	chmod +x "$PREFIX/bin/$2"
}
wrap m68k-linux-gnu-gcc-$GCCVER m68k-linux-gnu-gcc
wrap m68k-linux-gnu-cpp-$GCCVER m68k-linux-gnu-cpp
for t in as ld objcopy objdump nm size readelf ar addr2line strip; do
	wrap m68k-linux-gnu-$t m68k-linux-gnu-$t
done
wrap gdb-multiarch gdb-multiarch

"$PREFIX/bin/m68k-linux-gnu-gcc" --version | head -1
echo "toolchain installed in $PREFIX/bin"
