/* the stock curves against reference values, and a few sanity checks */
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
  /* a list moved by fractional pixels (GTK's logical ones): a fling from the middle runs to the end, and a pull
   * held at the top stays a stretch (float noise once read as an end reached, and as the list scrolled off it) */
  SpongyList l;
  spongy_list_init (&l, 816, 1350);
  spongy_list_down (&l, 663.8f, 0);
  for (int i = 1; i <= 12; i++)
    spongy_list_move (&l, 663.8f - 26.67f * i, 8.33 * i);
  spongy_list_up (&l, 104);
  for (double ms = 107; ms < 1000 && l.flinging; ms += 16.7)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("fling from %.0f: ended at %.1f of %.0f, bottom stretch %s\n", 314.0, l.offset, l.max_offset,
          spongy_edge_is_finished (&l.bottom) ? "no" : "yes");
  bad += l.offset != l.max_offset || spongy_edge_is_finished (&l.bottom);
  spongy_list_init (&l, 816, 1350);
  spongy_list_down (&l, 104.f, 0);
  for (int i = 1; i <= 60; i++)
    spongy_list_move (&l, 104.f + 7.9f * i, 8.33 * i);
  printf ("pull at the top: offset %.3f, stretch %.4f\n", l.offset, l.top.distance);
  bad += l.offset != 0 || l.top.distance < 0.5f;
  printf (bad ? "FAILED %d\n" : "ok\n", bad);
  return bad != 0;
}
