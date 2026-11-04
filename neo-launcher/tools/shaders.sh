#!/bin/sh
# Compile shaders/*.{vert,frag} to SPIR-V C arrays (shaders/<name>_<stage>.h); run after editing a shader
set -e
cd "$(dirname "$0")/../shaders"
for s in *.vert *.frag; do
  n=$(echo "$s" | tr . _)
  glslangValidator -V --target-env vulkan1.0 --vn "$n" -o "$n.h" "$s" >/dev/null
done
ls -1 *.h
