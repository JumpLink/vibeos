#!/bin/sh
# build-voice-en.sh OUTDIR: vibeos-dictate-voice-en, Vibe's English voice (Piper's en_US-lessac-medium) in
# /usr/share/vibeos-dictate/voices/en, where Vibe (vibeos-dictate) looks for a voice after the user's own data
# directory. The files are the ones Vibe downloads when English is chosen (apps/dictate/src/dictate-download.c, the
# same revision of rhasspy/piper-voices and the same sums), so a phone with the package needs no download.
#
# The voice is MIT, as rhasspy/piper-voices is, but it is trained from the Lessac recordings of Blizzard 2013, whose
# research licence forbids any commercial use, the development and sale of speech synthesis products included. The
# owner chose to keep this voice and to handle that licence. So this package goes into VibeOS's apt archive only:
# NEVER attach it to a GitHub release. Its copyright file says all of this.
#
# Needs curl and dpkg-deb.
set -eu
umask 022

OUT=${1:?usage: build-voice-en.sh OUTDIR}
MAINTAINER="pulpoff <pashkovsky@gmail.com>"

PIPER_REVISION=e9c8a39444359388adaa94255becb37a5a2b8e51
PIPER=https://huggingface.co/rhasspy/piper-voices/resolve/$PIPER_REVISION
VOICE_DIR=en/en_US/lessac/medium
# the revision's date and commit
VERSION=0~git20261120.e9c8a39-1
# sum, file (in VOICE_DIR); the .onnx sum is Hugging Face's LFS id, as dictate-download.c has it
FILES="
5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f en_US-lessac-medium.onnx
efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0 en_US-lessac-medium.onnx.json
ce49eb457742208166d399a40cdec2c7fa9db77960930031564ab56f12882645 MODEL_CARD
"
LESSAC_LICENCE=https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# fetch URL FILE SHA256
fetch() {
	curl -sfL --retry 3 -o "$2" "$1"
	echo "$3  $2" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
}

ROOT=$WORK/voice-en
DIR=$ROOT/usr/share/vibeos-dictate/voices/en
DOC=$ROOT/usr/share/doc/vibeos-dictate-voice-en
mkdir -p "$DIR" "$DOC"
echo "$FILES" | while read -r sum name; do
	[ -n "$name" ] || continue
	case $name in
	MODEL_CARD) fetch "$PIPER/$VOICE_DIR/$name" "$DOC/$name" "$sum" ;;
	*) fetch "$PIPER/$VOICE_DIR/$name" "$DIR/$name" "$sum" ;;
	esac
done
cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Piper voices (en_US-lessac-medium)
Source: https://huggingface.co/rhasspy/piper-voices/tree/$PIPER_REVISION/$VOICE_DIR
Comment: Piper's voice, unmodified; its model card is in this directory.
 .
 THE RECORDINGS THIS VOICE IS TRAINED FROM ARE NOT FREE. The model card names
 its dataset: the Lessac data of the Blizzard Challenge 2013 (audio book
 recordings of Voice Factory International, Inc. and associated data of Lessac
 Technologies, Inc.), under a research licence agreement:
 $LESSAC_LICENCE
 That agreement licenses its materials for research only, to the user who
 accepts it, and "excludes ... using the Materials for any commercial purpose,
 including the development, marketing, commercialisation, sale or licencing of
 voice synthesis or speech recognition products or services". Whether its terms
 reach a voice trained from those recordings is not settled here; VibeOS's
 maintainer ships the voice knowing this, and it is not offered for any
 commercial use. Another voice put in ~/.local/share/vibeos-dictate/voices/en
 is the one Vibe reads first.

Files: usr/share/vibeos-dictate/voices/en/*
Copyright: Michael Hansen (rhasspy) and the contributors of rhasspy/piper-voices
License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 .
 The repository is MIT as its README states; the recordings are under the
 Blizzard 2013 Lessac research licence described above.
EOF

mkdir -p "$ROOT/DEBIAN"
{
	echo "Package: vibeos-dictate-voice-en"
	echo "Version: $VERSION"
	echo "Architecture: all"
	echo "Maintainer: $MAINTAINER"
	echo "Installed-Size: $(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)"
	cat <<EOF
Enhances: vibeos-dictate
Section: sound
Priority: optional
Homepage: https://huggingface.co/rhasspy/piper-voices
Description: English voice for VibeOS's Vibe assistant (Piper, Lessac)
 Piper's en_US-lessac-medium voice, where Vibe (vibeos-dictate) looks for it,
 so that English answers need no download. The voice is MIT, but the
 recordings it is trained from are under the Blizzard 2013 Lessac research
 licence, which forbids commercial use: see the copyright file.
EOF
} > "$ROOT/DEBIAN/control"
find "$ROOT/usr" -type d -exec chmod 755 {} +
find "$ROOT/usr" -type f -exec chmod 644 {} +
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$OUT/vibeos-dictate-voice-en_${VERSION}_all.deb"
