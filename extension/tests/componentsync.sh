#!/bin/sh
# componentsync.sh SRC: componentsync.js against a stand-in of the shell's ComponentManager (componentsync.js beside it)
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$src/componentsync.js" "$t/componentsync.js"
cp "$here/componentsync.js" "$t/test.js"
gjs -m "$t/test.js"
