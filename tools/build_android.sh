#!/bin/bash
# build_android.sh -- build Furbtendulator for Android: bin-android/Furbtendulator.apk
# for phones, tablets, TVs and Chromebooks (64- and 32-bit ARM and x86,
# Android 5.0 or newer).
#
#     tools/build_android.sh [--accept-sdk-license] [WORKDIR]   (default WORKDIR: ./build-android)
#
# Needs the Android SDK with an NDK (r27 or newer): ANDROID_HOME (or
# ANDROID_SDK_ROOT) and ANDROID_NDK_HOME, or an SDK whose ndk/ folder has one.
# Without an SDK, --accept-sdk-license downloads Google's command-line tools
# into WORKDIR and installs the platform, build tools and NDK there, which
# means accepting the Android SDK license (https://developer.android.com/studio/terms)
# -- only pass it if you do.  Also needs git, cmake, ninja, python3, a JDK 17
# or newer, and network access to dl.google.com, maven.google.com and github.com.
#
# Steps: SDL 2 (fetched with git) is built for each ABI with the NDK;
# build.py --android builds the emulator, the front end (android/native) and
# the mapper packs; Gradle packages them with the Java activity
# (android/app) and SDL's Java half into a signed APK.  FURB_ABIS limits the
# ABIs (e.g. FURB_ABIS=arm64-v8a for a quicker test build).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ACCEPT=0
if [ "${1:-}" = --accept-sdk-license ]; then ACCEPT=1; shift; fi
WORK=$(realpath -m "${1:-$HERE/build-android}")
ABIS=${FURB_ABIS:-arm64-v8a armeabi-v7a x86 x86_64}
SDL_TAG=release-2.32.10
NDK_VERSION=27.2.12479018
PLATFORM=35
BUILD_TOOLS=35.0.0
JOBS=$(nproc 2>/dev/null || echo 4)
mkdir -p "$WORK"

# ---- the SDK and NDK
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
if [ -z "$SDK" ]; then
	SDK=$WORK/sdk
	if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
		if [ $ACCEPT != 1 ]; then
			echo "build_android.sh: no Android SDK (set ANDROID_HOME), or pass --accept-sdk-license to download one" >&2
			exit 1
		fi
		echo "fetching the Android command-line tools ..."
		curl -fL -o "$WORK/cmdline-tools.zip" https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
		rm -rf "$SDK/cmdline-tools" && mkdir -p "$SDK/cmdline-tools"
		(cd "$SDK/cmdline-tools" && unzip -q "$WORK/cmdline-tools.zip" && mv cmdline-tools latest)
	fi
fi
SDKMANAGER=$SDK/cmdline-tools/latest/bin/sdkmanager
if [ -x "$SDKMANAGER" ] && [ $ACCEPT = 1 ]; then
	yes | "$SDKMANAGER" --sdk_root="$SDK" --licenses > /dev/null || true
	"$SDKMANAGER" --sdk_root="$SDK" "platforms;android-$PLATFORM" "build-tools;$BUILD_TOOLS" "ndk;$NDK_VERSION" "platform-tools"
fi
NDK=${ANDROID_NDK_HOME:-}
if [ -z "$NDK" ]; then
	NDK=$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)
fi
if [ -z "$NDK" ] || [ ! -d "$NDK/toolchains/llvm" ]; then
	echo "build_android.sh: no NDK (set ANDROID_NDK_HOME, or install one with sdkmanager \"ndk;$NDK_VERSION\")" >&2
	exit 1
fi
echo "SDK $SDK"
echo "NDK $NDK"

# ---- SDL 2
if [ ! -d "$WORK/SDL2/.git" ]; then
	git clone --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL "$WORK/SDL2"
fi

OUT=$HERE/android/native-out
rm -rf "$OUT" && mkdir -p "$OUT/jniLibs" "$OUT/assets/furb"
for abi in $ABIS; do
	echo "==== $abi"
	cmake -S "$WORK/SDL2" -B "$WORK/sdl-$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
		-DANDROID_PLATFORM=android-21 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
		-DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > "$WORK/sdl-$abi.log"
	cmake --build "$WORK/sdl-$abi" -j "$JOBS" >> "$WORK/sdl-$abi.log"
	python3 "$HERE/build.py" --android "$abi" --ndk "$NDK" --sdl "$WORK/SDL2" --sdl-lib "$WORK/sdl-$abi" \
		--build "$WORK/build-$abi" -j "$JOBS"
	mkdir -p "$OUT/jniLibs/$abi"
	cp "$WORK/sdl-$abi/libSDL2.so" "$WORK/build-$abi"/android/*.so "$OUT/jniLibs/$abi/"
	DATA=$WORK/build-$abi
done
# Furbtendulator's data files (the same for every ABI)
cp "$DATA"/*.cfg "$OUT/assets/furb/"
cp -r "$DATA/BIOS" "$DATA/samples" "$OUT/assets/furb/"
# SDL's Java half, which must match the libSDL2.so built above
cp -r "$WORK/SDL2/android-project/app/src/main/java" "$OUT/sdl-java"

# ---- the signing key: FURB_KEYSTORE, or one made here once (keep it: updates need the same key)
if [ -z "${FURB_KEYSTORE:-}" ] && [ ! -f "$HERE/android/furb-release.keystore" ]; then
	keytool -genkeypair -keystore "$HERE/android/furb-release.keystore" -alias furb -keyalg RSA -keysize 2048 \
		-validity 10000 -storepass furbtendulator -keypass furbtendulator -dname "CN=Furbtendulator" -noprompt
	echo "made a signing key: android/furb-release.keystore (password furbtendulator) -- keep it"
fi

# ---- the APK
echo "sdk.dir=$SDK" > "$HERE/android/local.properties"
(cd "$HERE/android" && ANDROID_HOME="$SDK" ./gradlew --no-daemon assembleRelease)
mkdir -p "$HERE/bin-android"
cp "$HERE/android/app/build/outputs/apk/release/app-release.apk" "$HERE/bin-android/Furbtendulator.apk"
echo "built $HERE/bin-android/Furbtendulator.apk"
