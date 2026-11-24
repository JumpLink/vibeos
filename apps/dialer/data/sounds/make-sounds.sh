#!/bin/sh
# The call recording's announcement (call-recorder.c), "This call is now being recorded" in English and German: two
# Piper voices read it through Dictate's own Piper code (voice-bench, built from apps/dictate's voice-bench.c,
# dictate-voice.c and dictate-phonemes.c), then SoX and FFmpeg make it Ogg Opus.
#   English: en_US-lessac-medium. German: de_DE-thorsten-medium. Both from rhasspy/piper-voices at the revision
#   apps/dictate/src/dictate-download.c pins, with its checksums; the models are MIT, Thorsten's recordings CC0 and
#   Lessac's under the Blizzard 2013 Lessac licence (debian/copyright).
# Run it here: make-sounds.sh VOICE_BENCH VOICES (a folder with the two .onnx files and their .onnx.json), or, with the
# voices read elsewhere, make-sounds.sh --wav EN.wav DE.wav. The .opus files are what is installed.
set -e
EN="This call is now being recorded."
DE="Dieser Anruf wird jetzt aufgezeichnet."
t=$(mktemp -d)
if [ "$1" = --wav ]; then
  cp "$2" "$t/en.wav"
  cp "$3" "$t/de.wav"
else
  "$1" "$2/en_US-lessac-medium.onnx" -o "$t/en" "$EN"
  "$1" "$2/de_DE-thorsten-medium.onnx" -o "$t/de" "$DE"
  mv "$t/en/01.wav" "$t/en.wav"
  mv "$t/de/01.wav" "$t/de.wav"
fi
for lang in en de; do
  # 48 kHz, as the call's own PCMs run; the peaks at -1 dBFS; a quarter second of silence before and after
  sox "$t/$lang.wav" -r 48000 "$t/$lang-48.wav" gain -n -1 pad 0.25 0.25
  ffmpeg -loglevel error -y -i "$t/$lang-48.wav" -c:a libopus -b:a 32k -application voip -map_metadata -1 \
         "call-recording-$lang.opus"
done
rm -r "${t:?}"
