/* the spec's reference curves (SPEC.md 4.2) and a few sanity checks */
#include <math.h>
#include <stdio.h>
#include "spongy.h"

int
main (void)
{
  int bad = 0;
  /* release from rest: x/x0 at 25, 50 … ms, H = 1894, d = 0.4 (well away from the linear landing) */
  const double t[] = { 25, 50, 75, 100, 125, 150 };
  const double want[] = { .8718, .6471, .4416, .2858, .1782, .1079 };
  SpongyEdge e;
  spongy_edge_init (&e, 1894);
  spongy_edge_pull (&e, 0.4f, 0);
  spongy_edge_release (&e, 0);
  for (int i = 0; i < 6; i++) {
    bool more;
    spongy_edge_frame (&e, t[i], true, &more);
    double r = e.distance / 0.4;
    printf ("release %3.0f ms: %.4f (want %.4f)\n", t[i], r, want[i]);
    bad += fabs (r - want[i]) > 0.002;
  }
  /* absorb at 1000 px/s: the peak 40.8 ms in at 0.1966 * v px */
  spongy_edge_init (&e, 1894);
  spongy_edge_absorb (&e, 1000, 0);
  double peak = 0, peak_t = 0;
  for (double ms = 1; ms < 200; ms += 0.2) {
    SpongyEdge c = e;
    bool more;
    spongy_edge_frame (&c, ms, true, &more);
    if (c.distance * 1894 > peak) { peak = c.distance * 1894; peak_t = ms; }
  }
  printf ("absorb peak %.1f px at %.1f ms (want 196.6 at 40.8)\n", peak, peak_t);
  bad += fabs (peak - 196.6) > 1 || fabs (peak_t - 40.8) > 0.5;
  printf ("damp(1) = %.5f (want 0.03200), damp(0.1) = %.5f (want 0.01058)\n", spongy_edge_damp (1), spongy_edge_damp (0.1f));
  bad += fabsf (spongy_edge_damp (1) - 0.032f) > 1e-4 || fabsf (spongy_edge_damp (0.1f) - 0.01058f) > 1e-4;
  printf ("fling 5000 px/s: %.0f px in %d ms\n", spongy_fling_distance (5000), spongy_fling_duration (5000));
  float m = spongy_stretch_map (spongy_stretch_unmap (500, -0.02f, 1894), -0.02f, 1894);
  printf ("map(unmap(500)) = %.3f\n", m);
  bad += fabsf (m - 500) > 0.01;
  printf ("emphasized(0.5) = %.4f, fast_out_slow_in(0.5) = %.4f\n", spongy_curve (SPONGY_EMPHASIZED, 0.5f),
          spongy_curve (SPONGY_FAST_OUT_SLOW_IN, 0.5f));
  printf (bad ? "FAILED %d\n" : "ok\n", bad);
  return bad != 0;
}
