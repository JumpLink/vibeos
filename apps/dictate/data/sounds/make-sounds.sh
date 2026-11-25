#!/bin/sh
# Agent's state cues (dictate-cues.c), made with SoX: short sine notes with a soft attack and decay, quieter than the
# phone's own sounds. Run it here to make them again; the .wav files are what is installed.
set -e
note () {   # FILE FREQ SECONDS: one note
  sox -n -r 24000 -c 1 -b 16 "$1" synth "$3" sine "$2" fade q 0.008 "$3" 0.05 vol 0.35
}
gap () { sox -n -r 24000 -c 1 -b 16 "$1" trim 0 "$2"; }
t=$(mktemp -d)
# listening: two notes going up, a fifth
note $t/a.wav 660 0.07; note $t/b.wav 990 0.09
sox $t/a.wav $t/b.wav agent-listening.wav
# working: one soft low tick
note $t/c.wav 520 0.05
sox $t/c.wav agent-working.wav vol 0.7
# done: a third up, then the octave
note $t/d.wav 784 0.07; gap $t/g.wav 0.02; note $t/e.wav 1175 0.11
sox $t/d.wav $t/g.wav $t/e.wav agent-done.wav
# failed: two low notes going down
note $t/f.wav 440 0.10; note $t/h.wav 330 0.16
sox $t/f.wav $t/g.wav $t/h.wav agent-failed.wav
rm -r "${t:?}"
