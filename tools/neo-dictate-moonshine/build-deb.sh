#!/bin/sh
# build-deb.sh OUTDIR: the neo-dictate-moonshine package (arm64), the speech recognition Dictate (apps/dictate) uses.
#
# It holds Moonshine's library from its v0.1.6 release (libmoonshine.so, with the ONNX Runtime 1.23.2 it is built
# against, libonnxruntime.so.1) in /usr/lib/neo-dictate, and Moonshine's tiny streaming English model in
# /usr/share/neo-dictate/models/tiny-streaming-en, where neo-dictate looks for them. Both are MIT (Moonshine Voice,
# github.com/moonshine-ai/moonshine). They are downloaded from Moonshine's GitHub release and its model server and
# checked against the sums below, the licences from the release's tag; none of it is in this repository.
#
# Needs curl and dpkg-deb. Another model: its directory name and files' sums in MODEL and MODEL_FILES (the sizes and
# URLs are in Moonshine's core/moonshine-model-file-metadata.generated.cpp).
set -eu
umask 022

OUT=${1:?usage: build-deb.sh OUTDIR}
VERSION=0.1.6
DEBVERSION=0.1.6-1
RELEASE=https://github.com/moonshine-ai/moonshine/releases/download/v$VERSION
RAW=https://raw.githubusercontent.com/moonshine-ai/moonshine/v$VERSION
LIBRARY_SHA256=1e0a88f27c428cab43590ce1635fe122406ef9e70236b58d077daf4b95cf162c
MODEL=tiny-streaming-en
MODEL_URL=https://download.moonshine.ai/model/$MODEL/quantized_26_11_02
# decoder_kv_with_attention.ort is left out: it is only loaded for word timestamps, which Dictate does not ask for
MODEL_FILES="
c5f9f7cc98c27a965d8e200816f7e5c07b892712bb30cf07444bbde4ffd38199 adapter.ort
5f68900da4ea20f40fbe67bbec36399f0d47faedbd7198b29abd9dc5f472ef9f cross_kv.ort
6564f2c9a19de44195a53e336e07d05108f14babab57105d623bb68b79572d75 decoder_kv.ort
617e403b2aae48f2474e0b148bf835deb9510bc8692efeabfe27203959a3256e encoder.ort
5662679f0e5758646d8eade3bc4b5d9ebda875feb6e696c0cc5f1a08fea2219e frontend.model.ort
55c91d7efab9f7506d7d753131cba72145ff016be3447319fe414a31e13f25b4 frontend.weights.ort
74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950 streaming_config.json
6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d tokenizer.bin
"
# the licences of what libmoonshine.so is built from, at the release's tag, and the ONNX Runtime's notices
LICENSES="
fa7d1174dd8af6a7cd280be20b80d10095ed4c19b5b20b61a7715c3ad790dc5f LICENSE
2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c core/third-party/onnxruntime/LICENSE.txt
fab3dd6bdab226f1c08630b1dd917e11fcb4ec5e1e020e2c16f83a0a13863e85 core/third-party/Eigen/COPYING.MPL2
cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30 core/third-party/kaldi-native-fbank/LICENSE
a2840585f8411be8e6826a31ef15ae65c950bd74a2437a73b013398a934ad0c6 core/third-party/kissfft/COPYING
86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14 core/third-party/nlohmann/LICENSE.MIT
c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566 core/third-party/utf-8/LICENSE.txt
3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256 core/third-party/utf8proc/LICENSE.md
"
ORT_NOTICES_URL=https://raw.githubusercontent.com/microsoft/onnxruntime/v1.23.2/ThirdPartyNotices.txt
ORT_NOTICES_SHA256=e9e90971a8e75a9a8ac0c6412e29c1202d079998389915aa485f46c816c3b4cc

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT=$WORK/root
DOC=$ROOT/usr/share/doc/neo-dictate-moonshine

# fetch URL FILE SHA256
fetch() {
	curl -sfL --retry 3 -o "$2" "$1"
	echo "$3  $2" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
}

mkdir -p "$ROOT/usr/lib/neo-dictate" "$ROOT/usr/share/neo-dictate/models/$MODEL" "$DOC/licenses" "$ROOT/DEBIAN"

fetch "$RELEASE/moonshine-voice-linux-arm64.tar.gz" "$WORK/library.tar.gz" "$LIBRARY_SHA256"
tar xzf "$WORK/library.tar.gz" -C "$WORK"
install -m 644 "$WORK/moonshine-voice-linux-arm64/lib/libmoonshine.so" \
	"$WORK/moonshine-voice-linux-arm64/lib/libonnxruntime.so.1" "$ROOT/usr/lib/neo-dictate/"

echo "$MODEL_FILES" | while read -r sum name; do
	[ -n "$name" ] || continue
	fetch "$MODEL_URL/$name" "$ROOT/usr/share/neo-dictate/models/$MODEL/$name" "$sum"
done
chmod 644 "$ROOT/usr/share/neo-dictate/models/$MODEL"/*

echo "$LICENSES" | while read -r sum path; do
	[ -n "$path" ] || continue
	name=$(echo "$path" | sed 's|^core/third-party/||; s|/|-|g')
	fetch "$RAW/$path" "$DOC/licenses/$name" "$sum"
done
fetch "$ORT_NOTICES_URL" "$DOC/licenses/onnxruntime-ThirdPartyNotices.txt" "$ORT_NOTICES_SHA256"
chmod 644 "$DOC/licenses"/*

cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Moonshine Voice
Source: https://github.com/moonshine-ai/moonshine (release v$VERSION, model $MODEL)
Comment: Moonshine's prebuilt library and model, unmodified. The full licence texts, including those of the code built
 into libmoonshine.so (Eigen, kaldi-native-fbank, kissfft, nlohmann/json, utf-8, utf8proc) and the ONNX Runtime's
 third-party notices, are in /usr/share/doc/neo-dictate-moonshine/licenses.

Files: usr/lib/neo-dictate/libmoonshine.so usr/share/neo-dictate/models/*
Copyright: Moonshine AI and contributors
License: MIT
 See licenses/LICENSE (section 1).

Files: usr/lib/neo-dictate/libonnxruntime.so.1
Copyright: Microsoft Corporation
License: MIT
 See licenses/onnxruntime-LICENSE.txt and licenses/onnxruntime-ThirdPartyNotices.txt.
EOF

cat > "$ROOT/DEBIAN/control" <<EOF
Package: neo-dictate-moonshine
Version: $DEBVERSION
Architecture: arm64
Maintainer: pulpoff <pashkovsky@gmail.com>
Installed-Size: $(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
Depends: libc6 (>= 2.34), libstdc++6, libgcc-s1
Enhances: neo-dictate
Section: sound
Priority: optional
Homepage: https://github.com/moonshine-ai/moonshine
Description: Moonshine speech recognition for GNOME Phone's Dictate
 Moonshine Voice's speech to text library from its $VERSION release, with the
 ONNX Runtime it is built against, and its tiny streaming English model (34
 million parameters), where Dictate (neo-dictate) looks for them. Speech is
 recognised on the phone itself.
EOF

mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$OUT/neo-dictate-moonshine_${DEBVERSION}_arm64.deb"
