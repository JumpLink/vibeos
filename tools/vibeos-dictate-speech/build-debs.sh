#!/bin/sh
# build-debs.sh OUTDIR: the packages with the speech recognition Dictate (apps/dictate) uses, none of it in this
# repository. Everything is downloaded from its makers' releases and checked against the sums below.
#
#   vibeos-dictate-moonshine   Moonshine's library (v0.1.6 release: libmoonshine.so, with the ONNX Runtime 1.23.2 it is
#                           built against) in /usr/lib/vibeos-dictate. MIT.
#   vibeos-dictate-vosk        Vosk's library (v0.3.45 release: libvosk.so, with Kaldi, OpenFST and OpenBLAS in it) in
#                           /usr/lib/vibeos-dictate. Apache-2.0, OpenBLAS and CLAPACK BSD-3-Clause.
#   vibeos-dictate-en, -de, -es   Moonshine's tiny streaming models (MIT)          } in /usr/share/vibeos-dictate/models,
#   vibeos-dictate-fr, -it, -ru   Vosk's small models, 0.22 (Apache-2.0)            } a directory each
#
# Each language's package depends on its recogniser's. Needs curl, unzip and dpkg-deb. Another Moonshine model: its
# files' sums and directory in MOONSHINE_MODELS (the sizes and URLs are in Moonshine's
# core/moonshine-model-file-metadata.generated.cpp).
set -eu
umask 022

OUT=${1:?usage: build-debs.sh OUTDIR}
MAINTAINER="pulpoff <pashkovsky@gmail.com>"

MOONSHINE_VERSION=0.1.6
MOONSHINE_DEBVERSION=0.1.6-2
MOONSHINE_RELEASE=https://github.com/moonshine-ai/moonshine/releases/download/v$MOONSHINE_VERSION
MOONSHINE_RAW=https://raw.githubusercontent.com/moonshine-ai/moonshine/v$MOONSHINE_VERSION
MOONSHINE_SHA256=1e0a88f27c428cab43590ce1635fe122406ef9e70236b58d077daf4b95cf162c
MOONSHINE_MODEL_URL=https://download.moonshine.ai/model
# language, the model's directory on Moonshine's server, then its files' sums. decoder_kv_with_attention.ort is left
# out: it is only loaded for word timestamps, which Dictate does not ask for.
MOONSHINE_MODELS="
en tiny-streaming-en/quantized_26_11_02
c5f9f7cc98c27a965d8e200816f7e5c07b892712bb30cf07444bbde4ffd38199 adapter.ort
5f68900da4ea20f40fbe67bbec36399f0d47faedbd7198b29abd9dc5f472ef9f cross_kv.ort
6564f2c9a19de44195a53e336e07d05108f14babab57105d623bb68b79572d75 decoder_kv.ort
617e403b2aae48f2474e0b148bf835deb9510bc8692efeabfe27203959a3256e encoder.ort
5662679f0e5758646d8eade3bc4b5d9ebda875feb6e696c0cc5f1a08fea2219e frontend.model.ort
55c91d7efab9f7506d7d753131cba72145ff016be3447319fe414a31e13f25b4 frontend.weights.ort
74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950 streaming_config.json
6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d tokenizer.bin
de tiny-streaming-de/quantized_26_08_24
23b91142a35cfbc533a7f03ad8387d0b6d32856b465c6fe7a14ede44a254a345 adapter.ort
25e6d9d4f16569c2ede7e23a381fe7c60cc4b0f141adc88580557d4530634f17 cross_kv.ort
768b1464074fc4fb7327e8b863fc13dcd12cb7bbe8d8e9428fa7292f7fd9ca6e decoder_kv.ort
74610b818058e396f4b9d9ffd730cf8e36772122464d989dd3011dfa76bd6baf encoder.ort
4445bb4c2f41b0dd993999e06dc9367a0dde0d42a2e777116e7d93be102dc43d frontend.model.ort
233fda61d583403bbf13ea71b6ba2335fdf57eac71be85a97dcbb90a6c2f4186 frontend.weights.ort
d31afd7c2dd5ee9ae3fa232312c5b42cb3e4fe1c6e9c53d885f2bea63023790a streaming_config.json
5725ad3771a1339802f8cebeda0fa8f7723f5282b1ee726c2544774d7399b9a7 tokenizer.bin
es tiny-streaming-es/quantized_26_08_24
2efd97cd2ee7d89578dd1d429a849be9fe37ac81dd9c4e1d31c8a2e7f85d43e6 adapter.ort
9e268b1b3bb0f9ff79bca6e76055d41298de0a5ba52505ee04907a2642efa3ae cross_kv.ort
7adf56b982df98e385d71823b9a40cf9296ae3a746efcf03f3d7bb4604032db0 decoder_kv.ort
ec9eb43a01fbfe242a2c6a86050cffe4a31050e1c9f084e98560d6c100004505 encoder.ort
9d19434b861de0c1d2aff52556911ae565cf3212ebe6d7f797c76c5c5066c2fc frontend.model.ort
5e4d5471cd5d984d4b94a603d861539ebf1dc72de11b0cf13f38316725d5ce48 frontend.weights.ort
d31afd7c2dd5ee9ae3fa232312c5b42cb3e4fe1c6e9c53d885f2bea63023790a streaming_config.json
5fbb7d4314dcb18e03c5f975609e4a4cd572b22b01d2b2accb1b3e6830696f36 tokenizer.bin
"
# the licences of what libmoonshine.so is built from, at the release's tag, and the ONNX Runtime's notices
MOONSHINE_LICENSES="
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

VOSK_VERSION=0.3.45
VOSK_DEBVERSION=0.3.45-2
VOSK_URL=https://github.com/alphacep/vosk-api/releases/download/v$VOSK_VERSION/vosk-linux-aarch64-$VOSK_VERSION.zip
VOSK_SHA256=45e95d37755deb07568e79497d7feba8c03aee5a9e071df29961aa023fd94541
# libvosk.so's parts' licences, at the commits its build (vosk-api travis/Dockerfile.dockcross) takes them from
VOSK_LICENSES="
bb618af833c817ab01c1564bd3ec75ba4b4f9cf30910a4a2168e30a674ee8cc6 vosk-COPYING https://raw.githubusercontent.com/alphacep/vosk-api/v0.3.45/COPYING
3fd3dd523b1717f1fb2ae40896df943d767fa5e664e46156c2dba7094f0d7172 kaldi-COPYING https://raw.githubusercontent.com/alphacep/kaldi/bc5baf14231660bd50b7d05788865b4ac6c34481/COPYING
dd0c6c9378a98b6a0e96b559b285c8506434fe2691eeeef151e8a8c1c492778e openfst-COPYING https://raw.githubusercontent.com/alphacep/openfst/18e94e63870ebcf79ebb42b7035cd3cb626ec090/COPYING
23014869214a231525b38c39fdb5f82364fa24154ac80fdcab2a79671460fd09 clapack-COPYING https://raw.githubusercontent.com/alphacep/clapack/f4e1647de96343fb20eea08a9d4f0e0d04c31c51/COPYING
190b5a9c8d9723fe958ad33916bd7346d96fab3c5ea90832bb02d854f620fcff OpenBLAS-LICENSE https://raw.githubusercontent.com/OpenMathLib/OpenBLAS/v0.3.20/LICENSE
"
# language, model, its zip's sum (alphacephei.com/vosk/models lists them as Apache-2.0)
VOSK_MODEL_DEBVERSION=0.22-2
VOSK_MODELS="
fr vosk-model-small-fr-0.22 cabf6180e177eb9b3a9a9d43a437bd5e549f3a7d09525e5d69a3fed787be12ad
it vosk-model-small-it-0.22 9ec65e75861d1c6c2e457cccd932705340dcdf233f5b239f00733b4de0bf3267
ru vosk-model-small-ru-0.22 961d5ff98a17f4aa6de69864d0aa71fa5bac682301d2b5d17a3f24c5c99a46d4
"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# fetch URL FILE SHA256
fetch() {
	curl -sfL --retry 3 -o "$2" "$1"
	echo "$3  $2" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
}

language_name() {
	case $1 in
	de) echo German ;; en) echo English ;; es) echo Spanish ;;
	fr) echo French ;; it) echo Italian ;; ru) echo Russian ;;
	esac
}

# package ROOT NAME VERSION ARCH: ROOT/DEBIAN/control's fields after these four come from stdin; the .deb into OUT.
package() {
	mkdir -p "$1/DEBIAN"
	{
		echo "Package: $2"
		echo "Version: $3"
		echo "Architecture: $4"
		echo "Maintainer: $MAINTAINER"
		echo "Installed-Size: $(du -sk --exclude=DEBIAN "$1" | cut -f1)"
		cat
	} > "$1/DEBIAN/control"
	find "$1/usr" -type d -exec chmod 755 {} +
	find "$1/usr" -type f -exec chmod 644 {} +
	dpkg-deb --root-owner-group -Zxz --build "$1" "$OUT/${2}_${3}_$4.deb"
}

# --- vibeos-dictate-moonshine

ROOT=$WORK/moonshine
DOC=$ROOT/usr/share/doc/vibeos-dictate-moonshine
mkdir -p "$ROOT/usr/lib/vibeos-dictate" "$DOC/licenses"
fetch "$MOONSHINE_RELEASE/moonshine-voice-linux-arm64.tar.gz" "$WORK/moonshine.tar.gz" "$MOONSHINE_SHA256"
tar xzf "$WORK/moonshine.tar.gz" -C "$WORK"
cp "$WORK/moonshine-voice-linux-arm64/lib/libmoonshine.so" "$WORK/moonshine-voice-linux-arm64/lib/libonnxruntime.so.1" \
	"$ROOT/usr/lib/vibeos-dictate/"
echo "$MOONSHINE_LICENSES" | while read -r sum path; do
	[ -n "$path" ] || continue
	fetch "$MOONSHINE_RAW/$path" "$DOC/licenses/$(echo "$path" | sed 's|^core/third-party/||; s|/|-|g')" "$sum"
done
fetch "$ORT_NOTICES_URL" "$DOC/licenses/onnxruntime-ThirdPartyNotices.txt" "$ORT_NOTICES_SHA256"
cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Moonshine Voice
Source: https://github.com/moonshine-ai/moonshine (release v$MOONSHINE_VERSION)
Comment: Moonshine's prebuilt library, unmodified. The full licence texts, including those of the code built into
 libmoonshine.so (Eigen, kaldi-native-fbank, kissfft, nlohmann/json, utf-8, utf8proc) and the ONNX Runtime's
 third-party notices, are in /usr/share/doc/vibeos-dictate-moonshine/licenses.

Files: usr/lib/vibeos-dictate/libmoonshine.so
Copyright: Moonshine AI and contributors
License: MIT
 See licenses/LICENSE (section 1).

Files: usr/lib/vibeos-dictate/libonnxruntime.so.1
Copyright: Microsoft Corporation
License: MIT
 See licenses/onnxruntime-LICENSE.txt and licenses/onnxruntime-ThirdPartyNotices.txt.
EOF
package "$ROOT" vibeos-dictate-moonshine "$MOONSHINE_DEBVERSION" arm64 <<EOF
Depends: libc6 (>= 2.34), libstdc++6, libgcc-s1
Enhances: vibeos-dictate
Section: sound
Priority: optional
Homepage: https://github.com/moonshine-ai/moonshine
Description: Moonshine speech recognition for VibeOS's Dictate
 Moonshine Voice's speech to text library from its $MOONSHINE_VERSION release, with
 the ONNX Runtime it is built against, where Dictate (vibeos-dictate) looks for
 it. Dictate hears English, German and Spanish with it; its models are in
 vibeos-dictate-en, vibeos-dictate-de and vibeos-dictate-es. Speech is recognised on the
 phone itself.
EOF

# --- vibeos-dictate-vosk

ROOT=$WORK/vosk
DOC=$ROOT/usr/share/doc/vibeos-dictate-vosk
mkdir -p "$ROOT/usr/lib/vibeos-dictate" "$DOC/licenses"
fetch "$VOSK_URL" "$WORK/vosk.zip" "$VOSK_SHA256"
unzip -q "$WORK/vosk.zip" -d "$WORK"
cp "$WORK/vosk-linux-aarch64-$VOSK_VERSION/libvosk.so" "$ROOT/usr/lib/vibeos-dictate/"
echo "$VOSK_LICENSES" | while read -r sum name url; do
	[ -n "$url" ] || continue
	fetch "$url" "$DOC/licenses/$name" "$sum"
done
cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Vosk
Source: https://github.com/alphacep/vosk-api (release v$VOSK_VERSION, vosk-linux-aarch64-$VOSK_VERSION.zip)
Comment: Vosk's prebuilt library, unmodified. Built into it are Kaldi (alphacep's vosk branch), OpenFST, OpenBLAS
 0.3.20 and CLAPACK 3.2.1; their licence texts are in /usr/share/doc/vibeos-dictate-vosk/licenses.

Files: usr/lib/vibeos-dictate/libvosk.so
Copyright: Alpha Cephei Inc.
 Kaldi: Johns Hopkins University, Microsoft Corporation and the other contributors listed in licenses/kaldi-COPYING
 OpenFST: Google Inc. and others
 OpenBLAS: The OpenBLAS Project; GotoBLAS2: The University of Texas
 CLAPACK: The University of Tennessee, The University of California Berkeley, NAG Ltd.
License: Apache-2.0 and BSD-3-Clause
 Vosk, Kaldi and OpenFST: Apache-2.0, see licenses/vosk-COPYING, licenses/kaldi-COPYING and
 licenses/openfst-COPYING. OpenBLAS and CLAPACK: BSD-3-Clause, see licenses/OpenBLAS-LICENSE and
 licenses/clapack-COPYING.
EOF
package "$ROOT" vibeos-dictate-vosk "$VOSK_DEBVERSION" arm64 <<EOF
Depends: libc6 (>= 2.17), libstdc++6, libgcc-s1, libatomic1
Enhances: vibeos-dictate
Section: sound
Priority: optional
Homepage: https://alphacephei.com/vosk/
Description: Vosk speech recognition for VibeOS's Dictate
 Vosk's speech to text library (Kaldi) from its $VOSK_VERSION release, where
 Dictate (vibeos-dictate) looks for it. Dictate hears French, Italian and Russian
 with it; its models are in vibeos-dictate-fr, vibeos-dictate-it and vibeos-dictate-ru.
 Speech is recognised on the phone itself.
EOF

# --- Moonshine's models

echo "$MOONSHINE_MODELS" | while read -r first second; do
	[ -n "$first" ] || continue
	case $first in
	??)
		lang=$first; model=${second%%/*}
		ROOT=$WORK/model-$lang
		DOC=$ROOT/usr/share/doc/vibeos-dictate-$lang
		mkdir -p "$ROOT/usr/share/vibeos-dictate/models/$model" "$DOC"
		echo "$second" > "$WORK/model-$lang.source"
		continue ;;
	esac
	fetch "$MOONSHINE_MODEL_URL/$(cat "$WORK/model-$lang.source")/$second" \
		"$WORK/model-$lang/usr/share/vibeos-dictate/models/$model/$second" "$first"
done
for lang in $(echo "$MOONSHINE_MODELS" | awk 'length($1) == 2 { print $1 }'); do
	ROOT=$WORK/model-$lang
	DOC=$ROOT/usr/share/doc/vibeos-dictate-$lang
	source=$(cat "$WORK/model-$lang.source")
	cp "$WORK/moonshine/usr/share/doc/vibeos-dictate-moonshine/licenses/LICENSE" "$DOC/LICENSE"
	cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Moonshine Voice
Source: $MOONSHINE_MODEL_URL/$source (as Moonshine's v$MOONSHINE_VERSION release lists it)
Comment: Moonshine's model, unmodified.

Files: usr/share/vibeos-dictate/models/*
Copyright: Moonshine AI and contributors
License: MIT
 See LICENSE, section 1: every streaming model is MIT.
EOF
	package "$ROOT" "vibeos-dictate-$lang" "$MOONSHINE_DEBVERSION" all <<EOF
Depends: vibeos-dictate-moonshine
Enhances: vibeos-dictate
Section: sound
Priority: optional
Homepage: https://github.com/moonshine-ai/moonshine
Description: $(language_name "$lang") speech recognition for VibeOS's Dictate
 Moonshine's tiny streaming $(language_name "$lang") model, where Dictate (vibeos-dictate)
 looks for it. What is said comes with punctuation and capitals. Speech is
 recognised on the phone itself.
EOF
done

# --- Vosk's models

echo "$VOSK_MODELS" | while read -r lang model sum; do
	[ -n "$sum" ] || continue
	ROOT=$WORK/model-$lang
	DOC=$ROOT/usr/share/doc/vibeos-dictate-$lang
	DIR=$ROOT/usr/share/vibeos-dictate/models/vosk-small-$lang
	mkdir -p "$ROOT/usr/share/vibeos-dictate/models" "$DOC"
	fetch "https://alphacephei.com/vosk/models/$model.zip" "$WORK/$model.zip" "$sum"
	unzip -q "$WORK/$model.zip" -d "$WORK"
	mv "$WORK/$model/README" "$DOC/README"
	mv "$WORK/$model" "$DIR"
	cp "$WORK/vosk/usr/share/doc/vibeos-dictate-vosk/licenses/vosk-COPYING" "$DOC/LICENSE"
	cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Vosk
Source: https://alphacephei.com/vosk/models/$model.zip
Comment: Vosk's model, unmodified but for its README, which is in this directory.

Files: usr/share/vibeos-dictate/models/*
Copyright: Alpha Cephei Inc.
License: Apache-2.0
 As https://alphacephei.com/vosk/models lists it. See LICENSE.
EOF
	package "$ROOT" "vibeos-dictate-$lang" "$VOSK_MODEL_DEBVERSION" all <<EOF
Depends: vibeos-dictate-vosk
Enhances: vibeos-dictate
Section: sound
Priority: optional
Homepage: https://alphacephei.com/vosk/models
Description: $(language_name "$lang") speech recognition for VibeOS's Dictate
 Vosk's small $(language_name "$lang") model ($model), where Dictate (vibeos-dictate)
 looks for it. What is said comes in lower case, without punctuation. Speech
 is recognised on the phone itself.
EOF
done
