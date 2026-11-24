#!/bin/sh
# The call recording's announcement (call-recorder.c; the language is picked in call-recording.c), "This call is now
# being recorded" in each of VibeOS's languages. A Piper voice reads it through Dictate's own Piper code (voice-bench,
# built from apps/dictate's voice-bench.c, dictate-voice.c and dictate-phonemes.c), and FFmpeg sets its loudness and
# makes it Ogg Opus.
#
# The voices are rhasspy/piper-voices' at revision e9c8a39444359388adaa94255becb37a5a2b8e51, the one
# apps/dictate/src/dictate-download.c pins, each checked against its SHA-256 below. The models are MIT, and each
# was chosen for recordings that may be passed on (their MODEL_CARDs; debian/copyright and the Phone's About >
# Legal credit them):
#   en  en_GB-cori-high        Cori Samuel's LibriVox recordings, public domain; trained from scratch
#   de  de_DE-thorsten-medium  Thorsten-Voice, CC0; trained from the rdh voice (CC0)
#   es  es_ES-davefx-medium    CC0; from rdh
#   fr  fr_FR-siwis-medium     the SIWIS French speech synthesis database, CC BY 4.0; from Hi-Fi TTS reader 92
#                              (Cori Samuel; NVIDIA's Hi-Fi Multi-Speaker English TTS Dataset, CC BY 4.0)
#   it  it_IT-paola-medium     CC0; from Hi-Fi TTS reader 92 (CC BY 4.0)
#   nl  nl_NL-pim-medium       CC0; from rdh
#   ru  ru_RU-denis-medium     CC0; from rdh
# Loudness: -16 LUFS integrated (to a quarter of a dB) with true peaks at -1 dBTP or under (EBU R 128, as FFmpeg
# measures it), the same for every language: as loud as speech gets in a call, so the 6 dB or so the uplink's
# channel mixing can take off it still leaves it at speech level for the other side. 48 kHz, as the call's PCMs
# run, with a quarter of a second of silence before and after.
#
#   make-sounds.sh VOICE_BENCH VOICES [LANG...]   reads the text with each voice (VOICES: a folder with the .onnx
#                                                 files and their .onnx.json), all seven languages by default
#   make-sounds.sh --wav LANG FILE [LANG FILE...] the same from texts read elsewhere (FILE: voice-bench's WAV)
# The call-recording-LANG.opus files it writes here are what is installed.
set -eu

text() {
  case $1 in
  en) echo "This call is now being recorded." ;;
  de) echo "Dieser Anruf wird jetzt aufgezeichnet." ;;
  es) echo "Esta llamada se está grabando." ;;
  fr) echo "Cet appel est désormais enregistré." ;;
  it) echo "Questa chiamata viene ora registrata." ;;
  nl) echo "Dit gesprek wordt nu opgenomen." ;;
  ru) echo "Этот звонок сейчас записывается." ;;
  *) return 1 ;;
  esac
}

voice() {
  case $1 in
  en) echo "en_GB-cori-high 470b4dd634c98f8a4850d7626ffc3dfc90774628eeef6605a6dd8f88f30a5903" ;;
  de) echo "de_DE-thorsten-medium fd270eb3d1ebf91d763f8a0f949b1b1bebdfefaea18c3aaf8577dd34536e064f" ;;
  es) echo "es_ES-davefx-medium d2c9224e122321aac3c33343957fff4d0f465226cb74c66b0f520ca3f8c0dd07" ;;
  fr) echo "fr_FR-siwis-medium b1d946fd68d0347155fa59bce0a018924abbef3c1561181db45ca463d83963ef" ;;
  it) echo "it_IT-paola-medium f9b8658aa8c01f8280aa88488f6e7643a639b6225a81514c40440a01f6364a20" ;;
  nl) echo "nl_NL-pim-medium 1e1ed370c3b60ca734e525bdcd1f05f71b42bfb0a15693f1e89c012d72052d2a" ;;
  ru) echo "ru_RU-denis-medium 879bc365cb14d1380e3f5ed6044162ccf54c99f6161871569b7f830aec3ce841" ;;
  *) return 1 ;;
  esac
}

TARGET=-16                     # LUFS
CEILING=0.708                  # the limiter's, -3 dBFS at 192 kHz: Opus adds up to 2 dB to the peaks

# FILE's integrated loudness (LUFS) and true peak (dBTP), EBU R 128
measure() {
  ffmpeg -hide_banner -nostats -i "$1" -af ebur128=peak=true -f null - 2>&1 |
    awk '/^ +I:/ { i = $2 } /^ +Peak:/ { p = $2 } END { print i, p }'
}

# FILE.wav at TARGET into call-recording-LANG.opus: a gain, the peaks limited (at four times the rate, for the
# peaks between the samples), Opus, and the Opus file measured; again with the gain corrected by what it missed,
# until it is within a quarter of a dB (the limiter and Opus's voice filter both take some loudness off)
encode() {
  gain=0
  limit="aresample=192000,alimiter=limit=$CEILING:attack=2:release=40:level=false,aresample=48000"
  for pass in 1 2 3 4 5 6 7 8 9 10; do
    ffmpeg -loglevel error -y -i "$2" -af "volume=${gain}dB,$limit,adelay=250:all=1,apad=pad_dur=0.25" \
           -c:a libopus -b:a 32k -application voip -map_metadata -1 "call-recording-$1.opus"
    set -- "$1" "$2" $(measure "call-recording-$1.opus")
    off=$(awk -v i="$3" -v target=$TARGET 'BEGIN { printf "%.2f", target - i }')
    if awk -v off="$off" 'BEGIN { exit !(off < 0.25 && off > -0.25) }'; then
      echo "$1: $3 LUFS, true peak $4 dBTP (gain $gain dB)"
      return
    fi
    gain=$(awk -v g="$gain" -v off="$off" 'BEGIN { printf "%.2f", g + off }')
  done
  echo "$1: still at $3 LUFS after 10 passes" >&2
  return 1
}

t=$(mktemp -d)
trap 'rm -r "$t"' EXIT
if [ "${1:-}" = --wav ]; then
  shift
  while [ $# -ge 2 ]; do
    text "$1" >/dev/null
    encode "$1" "$2"
    shift 2
  done
else
  bench=$1 voices=$2
  shift 2
  for lang in ${*:-en de es fr it nl ru}; do
    set -- $(voice "$lang")
    echo "$2  $voices/$1.onnx" | sha256sum -c --quiet
    "$bench" "$voices/$1.onnx" -o "$t/$lang" "$(text "$lang")"
    encode "$lang" "$t/$lang/01.wav"
  done
fi
