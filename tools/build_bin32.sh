#!/bin/bash
# build_bin32.sh -- rebuild bin32/: a 32-bit x86 furb_cli (with its mapper
# packs and data files) that runs on glibc 2.29 or newer, for older systems.
#
#     tools/build_bin32.sh [WORKDIR]      (default WORKDIR: ./build32)
#
# The binaries in bin/ are built on a current system and need its glibc.
# This compiles against Ubuntu 20.04's GCC 9 and glibc instead, the setup the
# original 32-bit furb_cli was built with: the packages are downloaded from
# the Ubuntu archive with apt-get into WORKDIR (nothing is installed on the
# system) and used as a --sysroot.  Needs apt-get, dpkg-deb, python3,
# binutils and network access to archive.ubuntu.com.  The script refuses
# binaries that need a glibc newer than 2.29.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${1:-$HERE/build32}")
ROOT=$WORK/sysroot
mkdir -p "$WORK/apt/lists/partial" "$WORK/apt/cache/archives/partial" "$WORK/debs"

if [ ! -x "$ROOT/usr/bin/x86_64-linux-gnu-g++-9" ]; then
	echo "fetching Ubuntu 20.04's GCC 9 and glibc ..."
	cat > "$WORK/apt/sources" <<EOF
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu/ focal main universe
deb [arch=amd64,i386] http://archive.ubuntu.com/ubuntu/ focal-updates main universe
EOF
	APT=(-o Dir::Etc::sourcelist="$WORK/apt/sources" -o Dir::Etc::sourceparts=- -o Dir::State="$WORK/apt"
	     -o Dir::Cache="$WORK/apt/cache" -o Dir::State::status=/dev/null -o APT::Architecture=amd64
	     -o APT::Architectures::=amd64 -o APT::Architectures::=i386)
	apt-get "${APT[@]}" update -qq
	(cd "$WORK/debs" && apt-get "${APT[@]}" download gcc-9 g++-9 cpp-9 libgcc-9-dev libstdc++-9-dev \
		lib32gcc-9-dev lib32stdc++-9-dev libc6-dev libc6-dev-i386 libc6-i386 linux-libc-dev \
		libisl22 libmpc3 libmpfr6 lib32gcc-s1 lib32stdc++6 libgcc-s1 libc6)
	rm -rf "$ROOT" && mkdir -p "$ROOT"
	for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
	ln -sfn x86_64-linux-gnu/asm "$ROOT/usr/include/asm"	# what gcc-multilib provides
	# absolute symlinks (lib32/libdl.so -> /lib32/libdl.so.2) would reach this
	# system's libraries: point them inside the sysroot
	find "$ROOT" -type l | while read -r l; do
		t=$(readlink "$l")
		case "$t" in /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$ROOT$t")" "$l";; esac
	done
fi

# GCC 9's own helpers (cc1plus) need its libisl/libmpc/libmpfr
export LD_LIBRARY_PATH="$ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 "$HERE/build.py" --m32 --no-gui --build "$WORK/out" \
	--cxx "$ROOT/usr/bin/x86_64-linux-gnu-g++-9" --cc "$ROOT/usr/bin/x86_64-linux-gnu-gcc-9" --sysroot "$ROOT"

OUT=$WORK/out
worst=$(for f in "$OUT/furb_cli" "$OUT"/Mappers/*.so; do objdump -T "$f" | grep -o 'GLIBC_[0-9.]*'; done | sort -uV | tail -1)
echo "newest glibc symbol needed: $worst"
if [ "$(printf '%s\nGLIBC_2.29\n' "$worst" | sort -V | tail -1)" != GLIBC_2.29 ]; then
	echo "build_bin32.sh: $worst is newer than glibc 2.29; bin32/ not updated" >&2
	exit 1
fi
rm -rf "$HERE/bin32" && mkdir -p "$HERE/bin32/Mappers"
cp "$OUT/furb_cli" "$HERE/bin32/"
cp "$OUT"/Mappers/*.so "$HERE/bin32/Mappers/"
cp "$OUT"/*.cfg "$HERE/bin32/"
cp -r "$OUT/BIOS" "$OUT/samples" "$HERE/bin32/"
echo "updated $HERE/bin32"
