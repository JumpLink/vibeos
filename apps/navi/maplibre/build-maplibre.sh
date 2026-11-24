#!/bin/sh
# Fetches the pinned MapLibre Native release and builds its core (OpenGL ES 3 renderer, EGL, Wayland) as
# static archives for the navi map widget, then stages what the widget's meson build needs:
#
#   STAGE/lib/*.a           mbgl-core and the libraries it bundles (harfbuzz, freetype, sqlite, ...)
#   STAGE/include/          MapLibre's public headers and the header-only libraries they use
#   STAGE/maplibre.defines  the definitions mbgl-core was built with (they change class layouts)
#   STAGE/maplibre.libs     its link line: the archives above, then the system libraries
#   STAGE/VERSION           the tag and commit
#
# The widget links all of it into one private shared library, so nothing of MapLibre lands in the
# system library directory and only the widget's C API is exported.
#
# usage: build-maplibre.sh WORKDIR            cross-compile for arm64 with the host's clang + lld against
#                                             the Debian arm64 root in $NAVI_SYSROOT
#        build-maplibre.sh --native WORKDIR   build with the system compiler (an arm64 chroot or device)
#
# WORKDIR gets src-TAG/ (the download, in a directory of its own), build-TAG-ARCH/ and stage-TAG-ARCH/.
# An existing src-TAG/ is reused only if it holds the pinned commit. Build dependencies on the target:
# libcurl4-openssl-dev (or -gnutls-dev) libuv1-dev libicu-dev libpng-dev libjpeg-dev libwebp-dev
# zlib1g-dev libegl-dev libgles-dev; on the build host: cmake ninja-build git (and clang lld llvm for
# the cross build).
set -eu

TAG=android-v13.7.1
COMMIT=4cf121e515d8d065b57cd555a0b231ea27ff0e5d
URL=https://github.com/maplibre/maplibre-native.git
# submodules a Linux OpenGL build never reads (docs, Windows, Vulkan, Metal, WebGPU)
SKIP_SUBMODULES="docs/doxygen/doxygen-awesome-css platform/windows/vendor/vcpkg vendor/Vulkan-Headers
vendor/VulkanMemoryAllocator vendor/metal-cpp vendor/webgpu-cpp vendor/wgpu-native vendor/glslang"

native=0
if [ "${1:-}" = "--native" ]; then native=1; shift; fi
[ $# -eq 1 ] || { sed -n '2,24p' "$0" >&2; exit 2; }
work=$(realpath -m "$1")
here=$(dirname "$(realpath "$0")")
jobs=${NAVI_JOBS:-8}

if [ $native -eq 1 ]; then
    arch=$(uname -m)
    toolchain=""
else
    arch=aarch64
    : "${NAVI_SYSROOT:?set NAVI_SYSROOT to the arm64 root filesystem}"
    export NAVI_SYSROOT
    toolchain="-DCMAKE_TOOLCHAIN_FILE=$here/aarch64-linux-gnu-clang.cmake"
fi

src="$work/src-$TAG/maplibre-native"
build="$work/build-$TAG-$arch"
stage="$work/stage-$TAG-$arch"
mkdir -p "$work"

# ---- fetch ----
if [ -d "$src" ]; then
    have=$(git -C "$src" rev-parse HEAD)
    [ "$have" = "$COMMIT" ] || { echo "$src holds $have, not $COMMIT: remove it first" >&2; exit 1; }
else
    [ ! -e "$work/src-$TAG" ] || { echo "$work/src-$TAG exists without a checkout: remove it" >&2; exit 1; }
    mkdir "$work/src-$TAG"
    git clone -q --depth 1 --branch "$TAG" "$URL" "$src"
    have=$(git -C "$src" rev-parse HEAD)
    [ "$have" = "$COMMIT" ] || { echo "tag $TAG is $have now, expected $COMMIT" >&2; exit 1; }
    wanted=$(git -C "$src" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}' |
             grep -vxF "$(printf '%s\n' $SKIP_SUBMODULES)")
    # shellcheck disable=SC2086
    git -C "$src" submodule update -q --init --recursive --depth 1 --jobs "$jobs" -- $wanted
fi

src=$(realpath "$src")
build=$(realpath -m "$build")

# ---- build ----
# The project include adds a probe that reports mbgl-core's include directories, definitions and link
# line (export-flags.cmake). Werror stays off: the release's own warnings depend on compiler version.
nice -n 19 cmake -G Ninja -S "$src" -B "$build" $toolchain \
    -DCMAKE_PROJECT_INCLUDE="$here/export-flags.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMLN_WITH_OPENGL=ON -DMLN_WITH_EGL=ON -DMLN_WITH_WAYLAND=ON -DMLN_WITH_X11=OFF -DOPENGL_USE_GLES3=ON \
    -DMLN_WITH_GLFW=OFF -DMLN_WITH_WERROR=OFF -DBUILD_TESTING=OFF >"$build.configure.log"
nice -n 19 ninja -C "$build" -j "$jobs" mbgl-core

# ---- stage ----
rm -rf "$stage"
mkdir -p "$stage/lib" "$stage/include"
link=$(ninja -C "$build" -t commands navi-probe | tail -n 1 | sed 's/ && :$//')
: >"$stage/maplibre.libs"
for word in $link; do
    case "$word" in
        *navi-probe*|-o|-shared|-Wl,*|-Xlinker|--dependency-file=*) ;;
        *.a)
            cp "$build/$word" "$stage/lib/"
            echo "lib/$(basename "$word")" >>"$stage/maplibre.libs" ;;
        *.so)
            name=$(basename "$word" .so); echo "-l${name#lib}" >>"$stage/maplibre.libs" ;;
        -l*|-pthread)
            grep -qxF -- "$word" "$stage/maplibre.libs" || echo "$word" >>"$stage/maplibre.libs" ;;
    esac
done
# Headers only. The bundled harfbuzz and freetype are left out: no public MapLibre header includes them,
# and next to GTK (which brings the system's harfbuzz through pango) a second hb.h is a trap.
while read -r dir; do
    case "$dir" in ""|*/vendor/harfbuzz/*|*/vendor/freetype/*) continue ;; esac
    (cd "$dir" && find . -type f \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.ipp' \) \
        -exec cp --parents -t "$stage/include/" {} +)
done <"$build/navi-includes.txt"
# three private headers the widget uses to turn layer JSON into style layers
for h in mln/util/rapidjson.hpp mln/style/rapidjson_conversion.hpp mln/style/conversion/json.hpp; do
    mkdir -p "$stage/include/$(dirname "$h")"
    cp "$src/src/$h" "$stage/include/$h"
done
grep -v '^$' "$build/navi-defines.txt" >"$stage/maplibre.defines"
printf 'maplibre-native %s %s %s\n' "$TAG" "$COMMIT" "$arch" >"$stage/VERSION"
echo "staged in $stage"
