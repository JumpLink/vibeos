/* navi-say: a test's speech, TEXT spoken by a Piper voice into a WAV file (16-bit mono, the voice's rate), followed by
 * a second of silence: the search's microphone hears it as DICTATE_TEST_INPUT.
 *
 *   navi-say VOICE.onnx OUT.wav TEXT */
#include "dictate-voice.h"

#include <math.h>
#include <stdio.h>

static void
put16 (FILE *f, guint v)
{
  fputc (v & 0xff, f);
  fputc ((v >> 8) & 0xff, f);
}

static void
put32 (FILE *f, guint32 v)
{
  put16 (f, v & 0xffff);
  put16 (f, v >> 16);
}

int
main (int argc, char **argv)
{
  if (argc != 4) {
    fprintf (stderr, "usage: navi-say VOICE.onnx OUT.wav TEXT\n");
    return 2;
  }
  g_autoptr (GError) error = NULL;
  DictateVoice *voice = dictate_voice_load (argv[1], NULL, 0, &error);
  if (!voice) {
    fprintf (stderr, "navi-say: %s\n", error->message);
    return 1;
  }
  gsize n = 0;
  float *samples = dictate_voice_speak (voice, argv[3], 1.0, &n, &error);
  if (!samples) {
    fprintf (stderr, "navi-say: %s\n", error->message);
    return 1;
  }
  guint rate = dictate_voice_get_rate (voice);
  gsize total = n + rate; /* a second of silence after: the end of the turn */
  FILE *f = fopen (argv[2], "wb");
  if (!f) {
    perror (argv[2]);
    return 1;
  }
  fwrite ("RIFF", 1, 4, f);
  put32 (f, 36 + total * 2);
  fwrite ("WAVEfmt ", 1, 8, f);
  put32 (f, 16);
  put16 (f, 1);
  put16 (f, 1);
  put32 (f, rate);
  put32 (f, rate * 2);
  put16 (f, 2);
  put16 (f, 16);
  fwrite ("data", 1, 4, f);
  put32 (f, total * 2);
  for (gsize i = 0; i < total; i++) {
    float s = i < n ? samples[i] : 0;
    put16 (f, (guint16) (gint16) lrintf (CLAMP (s, -1.0f, 1.0f) * 32767));
  }
  fclose (f);
  g_free (samples);
  dictate_voice_free (voice);
  return 0;
}
