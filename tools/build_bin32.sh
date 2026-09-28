#!/bin/bash
# build_bin32.sh -- rebuild bin32/: 32-bit x86 builds of furb (the GUI) and
# furb_cli, with their mapper packs and data files, that run on glibc 2.29 or
# newer, for older systems.
#
#     tools/build_bin32.sh [WORKDIR]      (default WORKDIR: ./build32)
#
# The binaries in bin/ are built on a current system and need its glibc.
# This compiles against Ubuntu 20.04's GCC 9, glibc, GTK 3 and SDL 2 instead
# (the setup the original 32-bit furb_cli was built with): the packages are
# downloaded from the Ubuntu archive with apt-get into WORKDIR (nothing is
# installed on the system) and used as a --sysroot.  At run time furb uses the
# system's own GTK 3 (3.22 or newer) and SDL 2.  Needs apt-get, dpkg-deb,
# python3, binutils and network access to archive.ubuntu.com (about 230 MB
# the first time).  The script refuses binaries that need a glibc newer than
# 2.29.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${1:-$HERE/build32}")
ROOT=$WORK/sysroot
STAMP=$ROOT/.furb-sysroot-2	# (1 had no GTK/SDL)
mkdir -p "$WORK/apt/lists/partial" "$WORK/apt/cache/archives/partial" "$WORK/debs"

if [ ! -e "$STAMP" ]; then
	echo "fetching Ubuntu 20.04's GCC 9, glibc, GTK 3 and SDL 2 ..."
	cat > "$WORK/apt/sources" <<EOF
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu/ focal main universe
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu/ focal-updates main universe
EOF
	APT=(-o Dir::Etc::sourcelist="$WORK/apt/sources" -o Dir::Etc::sourceparts=- -o Dir::State="$WORK/apt"
	     -o Dir::Cache="$WORK/apt/cache" -o Dir::State::status=/dev/null -o APT::Architecture=amd64
	     -o APT::Architectures::=amd64 -o APT::Architectures::=i386)
	apt-get "${APT[@]}" update -qq
	# the compiler and C library, then the i386 GTK/SDL development packages
	# with everything they depend on (headers, .pc files and libraries to link)
	pkgs="gcc-9 g++-9 cpp-9 libgcc-9-dev libstdc++-9-dev lib32gcc-9-dev lib32stdc++-9-dev libc6-dev libc6-dev-i386
		libc6-i386 linux-libc-dev libisl22 libmpc3 libmpfr6 lib32gcc-s1 lib32stdc++6 libgcc-s1 libc6"
	gui=$(apt-cache "${APT[@]}" depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
		--no-replaces --no-enhances libgtk-3-dev:i386 libsdl2-dev:i386 | grep -v '^[ <]' | sort -u)
	rm -f "$WORK"/debs/*.deb
	(cd "$WORK/debs" && apt-get "${APT[@]}" download $pkgs $gui)
	rm -rf "$ROOT" && mkdir -p "$ROOT"
	for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
	ln -sfn x86_64-linux-gnu/asm "$ROOT/usr/include/asm"	# what gcc-multilib provides
	# absolute symlinks (lib32/libdl.so -> /lib32/libdl.so.2) would reach this
	# system's libraries: point them inside the sysroot
	find "$ROOT" -type l | while read -r l; do
		t=$(readlink "$l")
		case "$t" in /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$ROOT$t")" "$l";; esac
	done
	touch "$STAMP"
fi

# GCC 9's own helpers (cc1plus) need its libisl/libmpc/libmpfr -- only those,
# so host tools such as pkg-config keep their own libraries
mkdir -p "$WORK/gcclibs"
for l in "$ROOT"/usr/lib/x86_64-linux-gnu/lib{isl,mpc,mpfr}.so*; do ln -sfn "$l" "$WORK/gcclibs/"; done
export LD_LIBRARY_PATH="$WORK/gcclibs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 "$HERE/build.py" --m32 --build "$WORK/out" \
	--cxx "$ROOT/usr/bin/x86_64-linux-gnu-g++-9" --cc "$ROOT/usr/bin/x86_64-linux-gnu-gcc-9" --sysroot "$ROOT"

OUT=$WORK/out
[ -x "$OUT/furb" ] || { echo "build_bin32.sh: the GUI (furb) was not built" >&2; exit 1; }
worst=$(for f in "$OUT/furb" "$OUT/furb_cli" "$OUT"/Mappers/*.so; do objdump -T "$f" | grep -o 'GLIBC_[0-9.]*'; done | sort -uV | tail -1)
echo "newest glibc symbol needed: $worst"
if [ "$(printf '%s\nGLIBC_2.29\n' "$worst" | sort -V | tail -1)" != GLIBC_2.29 ]; then
	echo "build_bin32.sh: $worst is newer than glibc 2.29; bin32/ not updated" >&2
	exit 1
fi
rm -rf "$HERE/bin32" && mkdir -p "$HERE/bin32/Mappers"
cp "$OUT/furb" "$OUT/furb_cli" "$HERE/bin32/"
cp "$OUT"/Mappers/*.so "$HERE/bin32/Mappers/"
cp "$OUT"/*.cfg "$HERE/bin32/"
cp -r "$OUT/BIOS" "$OUT/samples" "$HERE/bin32/"
echo "updated $HERE/bin32"
