#!/usr/bin/python3 -I
# make-key-tap.py [OUTPUT.oga]: synthesizes sounds/key-tap.oga, the click under each key of the on-screen keyboard
# (sounds.js). Nothing is sampled from anywhere: the click is arithmetic, so the file is the VibeOS project's own
# (Expat, like the rest of the repository; CC0 for anyone who wants it on its own).
#
# 18 ms, mono, 48 kHz, 16 bit. A key's click is a short body and a short air of noise under it:
#  - a sine at 1800 Hz and one at 950 Hz, each with an exponential decay (the time constants are 2.2 ms and 3.5 ms),
#    which is the "tick" of a plastic key;
#  - a burst of noise, low-passed once (one pole, 4 kHz), decaying in 1.2 ms, which is its edge;
#  - a 0.4 ms rise at the start and a 3 ms fall to zero at the end, so that it starts and ends on zero (no pop).
# The peak is -9 dBFS: soft, as Gboard's is; the volume is the output's own.
#
# The samples are written as a WAV (the standard library) and encoded to Ogg Vorbis (what libcanberra decodes
# without further libraries) with ffmpeg's libvorbis, or oggenc.
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import wave

RATE = 48000
LENGTH_MS = 18
PEAK = 10 ** (-9 / 20)


def synthesize():
    n = RATE * LENGTH_MS // 1000
    rng = random.Random(1)      # the same noise every time: the same file
    samples = []
    lowpass = 0.0
    alpha = 1 - math.exp(-2 * math.pi * 4000 / RATE)
    for i in range(n):
        t = i / RATE
        body = 0.7 * math.sin(2 * math.pi * 1800 * t) * math.exp(-t / 0.0022) \
            + 0.5 * math.sin(2 * math.pi * 950 * t) * math.exp(-t / 0.0035)
        lowpass += alpha * (rng.uniform(-1, 1) - lowpass)
        edge = 0.8 * lowpass * math.exp(-t / 0.0012)
        rise = min(1.0, t / 0.0004)
        fall = min(1.0, (n - 1 - i) / RATE / 0.003)
        samples.append((body + edge) * rise * fall)
    top = max(abs(s) for s in samples)
    return [s / top * PEAK for s in samples]


def write_wav(path, samples):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(int(round(s * 32767)).to_bytes(2, 'little', signed=True) for s in samples))


def encode(wav, out):
    if shutil.which('ffmpeg'):
        cmd = ['ffmpeg', '-v', 'error', '-y', '-i', wav, '-map_metadata', '-1', '-fflags', '+bitexact',
               '-flags:a', '+bitexact', '-c:a', 'libvorbis', '-q:a', '5', '-f', 'ogg', out]
    elif shutil.which('oggenc'):
        cmd = ['oggenc', '-q', '5', '-Q', '-o', out, wav]
    else:
        sys.exit('make-key-tap.py: needs ffmpeg (libvorbis) or oggenc')
    subprocess.run(cmd, check=True)


here = os.path.dirname(os.path.abspath(__file__))
output = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'sounds', 'key-tap.oga')
with tempfile.TemporaryDirectory() as tmp:
    wav = os.path.join(tmp, 'key-tap.wav')
    write_wav(wav, synthesize())
    encode(wav, output)
