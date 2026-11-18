#!/bin/sh
# Compile shaders/*.{vert,frag} for both backends; run after editing a shader:
#  - Vulkan: SPIR-V C arrays, shaders/<name>_<stage>.h
#  - OpenGL ES: the GLSL ES 3.00 text as a C string, shaders/<name>_<stage>_gles.h: the same source with its includes
#    inlined, "#version 450" made "#version 300 es" with high precision throughout, and VULKAN undefined (the sources
#    spell the few differences between the dialects under #ifdef VULKAN). glslang checks that text too.
set -e
cd "$(dirname "$0")/../shaders"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for s in *.vert *.frag; do
  n=$(echo "$s" | tr . _)
  stage=${s##*.}
  glslangValidator -V --target-env vulkan1.0 --vn "$n" -o "$n.h" "$s" >/dev/null

  awk '
    /^#version 450/ {
      print "#version 300 es"
      print "precision highp float;"
      print "precision highp int;"
      print "precision highp sampler2D;"
      next
    }
    /^#extension GL_GOOGLE_include_directive/ { next }
    /^#include "/ {
      f = $2
      gsub(/"/, "", f)
      while ((getline line < f) > 0)
        print line
      close(f)
      next
    }
    { print }
  ' "$s" > "$tmp/$n.$stage"
  glslangValidator -S "$stage" "$tmp/$n.$stage" >/dev/null || { glslangValidator -S "$stage" "$tmp/$n.$stage"; exit 1; }
  {
    echo "/* GLSL ES 3.00 from $s (tools/shaders.sh) */"
    echo "#pragma once"
    echo "static const char ${n}_gles[] ="
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$tmp/$n.$stage"
    echo "  ;"
  } > "${n}_gles.h"
done
ls -1 *.h
