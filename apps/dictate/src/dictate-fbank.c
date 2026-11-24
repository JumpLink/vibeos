/* dictate-fbank: see dictate-fbank.h. In double precision, the tables made once. */
#include "dictate-fbank.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define FFT      512
#define BINS     (FFT / 2)       /* the power spectrum's bins the triangles use: 0 .. 255, 31.25 Hz apart */
#define LOW_HZ   20.0
#define HIGH_HZ  8000.0

typedef struct {
  double window[DICTATE_FBANK_FRAME];
  double cos_[FFT / 2], sin_[FFT / 2];
  guint reversed[FFT];
  /* each band's weights over the spectrum's bins, from its first bin with any */
  guint first[DICTATE_FBANK_BANDS], count[DICTATE_FBANK_BANDS];
  double weight[DICTATE_FBANK_BANDS][BINS];
} Tables;

static double
mel (double hz)
{
  return 1127.0 * log (1.0 + hz / 700.0);
}

static gpointer
tables_new (gpointer data)
{
  Tables *t = g_new0 (Tables, 1);
  double low = mel (LOW_HZ), delta = (mel (HIGH_HZ) - low) / (DICTATE_FBANK_BANDS + 1);
  guint bits = 0;

  for (guint i = 0; i < DICTATE_FBANK_FRAME; i++)
    t->window[i] = pow (0.5 - 0.5 * cos (2 * G_PI * i / (DICTATE_FBANK_FRAME - 1)), 0.85);
  for (guint i = 0; i < FFT / 2; i++)
    {
      t->cos_[i] = cos (2 * G_PI * i / FFT);
      t->sin_[i] = -sin (2 * G_PI * i / FFT);
    }
  while ((1u << bits) < FFT)
    bits++;
  for (guint i = 0; i < FFT; i++)
    {
      guint r = 0;
      for (guint b = 0; b < bits; b++)
        r |= ((i >> b) & 1) << (bits - 1 - b);
      t->reversed[i] = r;
    }
  /* Kaldi's triangles: rising from the band's left edge to its centre and falling to its right, on the mel scale */
  for (guint b = 0; b < DICTATE_FBANK_BANDS; b++)
    {
      double left = low + b * delta, centre = left + delta, right = centre + delta;
      gboolean any = FALSE;

      for (guint k = 0; k < BINS; k++)
        {
          double m = mel (k * (double) DICTATE_FBANK_RATE / FFT), w = 0;

          if (m > left && m < right)
            w = m <= centre ? (m - left) / (centre - left) : (right - m) / (right - centre);
          if (w > 0 && !any)
            t->first[b] = k, any = TRUE;
          if (w > 0)
            t->count[b] = k - t->first[b] + 1;
          t->weight[b][k] = w;
        }
    }
  return t;
}

static const Tables *
tables (void)
{
  static GOnce once = G_ONCE_INIT;

  return g_once (&once, tables_new, NULL);
}

/* in place, re and im of FFT points */
static void
fft (const Tables *t, double *re, double *im)
{
  for (guint i = 0; i < FFT; i++)
    {
      guint j = t->reversed[i];
      if (j > i)
        {
          double r = re[i], m = im[i];
          re[i] = re[j], im[i] = im[j];
          re[j] = r, im[j] = m;
        }
    }
  for (guint size = 2; size <= FFT; size *= 2)
    {
      guint half = size / 2, step = FFT / size;

      for (guint start = 0; start < FFT; start += size)
        for (guint k = 0; k < half; k++)
          {
            guint a = start + k, b = a + half;
            double wr = t->cos_[k * step], wi = t->sin_[k * step];
            double br = re[b] * wr - im[b] * wi, bi = re[b] * wi + im[b] * wr;

            re[b] = re[a] - br, im[b] = im[a] - bi;
            re[a] += br, im[a] += bi;
          }
    }
}

gsize
dictate_fbank_frames (gsize n)
{
  return n < DICTATE_FBANK_FRAME ? 0 : 1 + (n - DICTATE_FBANK_FRAME) / DICTATE_FBANK_HOP;
}

float *
dictate_fbank (const float *samples, gsize n, gboolean normalise, gsize *frames)
{
  const Tables *t = tables ();
  gsize count = dictate_fbank_frames (n);
  float *out;
  double re[FFT], im[FFT], power[BINS];

  *frames = count;
  if (!count)
    return NULL;
  out = g_new (float, count * DICTATE_FBANK_BANDS);
  for (gsize f = 0; f < count; f++)
    {
      const float *x = samples + f * DICTATE_FBANK_HOP;
      double mean = 0;

      for (guint i = 0; i < DICTATE_FBANK_FRAME; i++)
        mean += x[i];
      mean /= DICTATE_FBANK_FRAME;
      for (guint i = 0; i < DICTATE_FBANK_FRAME; i++)
        re[i] = x[i] - mean;
      for (guint i = DICTATE_FBANK_FRAME - 1; i > 0; i--)
        re[i] -= 0.97 * re[i - 1];
      re[0] -= 0.97 * re[0];
      for (guint i = 0; i < DICTATE_FBANK_FRAME; i++)
        re[i] *= t->window[i];
      memset (re + DICTATE_FBANK_FRAME, 0, (FFT - DICTATE_FBANK_FRAME) * sizeof *re);
      memset (im, 0, sizeof im);
      fft (t, re, im);
      for (guint k = 0; k < BINS; k++)
        power[k] = re[k] * re[k] + im[k] * im[k];
      for (guint b = 0; b < DICTATE_FBANK_BANDS; b++)
        {
          double e = 0;
          for (guint k = t->first[b]; k < t->first[b] + t->count[b]; k++)
            e += t->weight[b][k] * power[k];
          out[f * DICTATE_FBANK_BANDS + b] = log (MAX (e, FLT_EPSILON));
        }
    }
  if (normalise)
    for (guint b = 0; b < DICTATE_FBANK_BANDS; b++)
      {
        double mean = 0;
        for (gsize f = 0; f < count; f++)
          mean += out[f * DICTATE_FBANK_BANDS + b];
        mean /= count;
        for (gsize f = 0; f < count; f++)
          out[f * DICTATE_FBANK_BANDS + b] -= mean;
      }
  return out;
}
