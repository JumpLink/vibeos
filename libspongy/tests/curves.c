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

  /* the fling's speed for a distance: the inverse of its distance for a speed */
  const double distances[] = { 3, 40, 300, 1500, 9000 };
  for (int i = 0; i < 5; i++) {
    double back = spongy_fling_distance (spongy_fling_velocity (distances[i]));
    printf ("fling for %.0f px: %.0f px/s, runs %.2f px\n", distances[i], spongy_fling_velocity (distances[i]), back);
    bad += fabs (back - distances[i]) > distances[i] * 1e-3;
  }
  bad += spongy_fling_velocity (0) != 0;

  /* snapping to items: a grid of 300 px columns in a 360 px wide strip (any axis: the list does not know which),
   * flung from the middle of a column, comes to rest on a column's edge, the nearest to where it would have stopped */
  float grid = 24 + 10 * 296 + 9 * 4 + 24, snapped, would, at;
  spongy_list_init (&l, 360, grid);
  spongy_list_down (&l, 300.f, 0);
  for (int i = 1; i <= 8; i++)
    spongy_list_move (&l, 300.f - 15.f * i, 8.33 * i);
  spongy_list_up (&l, 70);
  would = l.offset + l.fling.final - l.fling.position;
  snapped = spongy_list_snap (&l, SPONGY_SNAP_ITEMS, 300, 70);
  for (double ms = 73; ms < 3000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("items, flung: would stop at %.1f, snapped to %.1f, rests at %.3f\n", would, snapped, l.offset);
  bad += fabsf (l.offset - snapped) > 0.01f || fmodf (snapped, 300) != 0 || fabsf (snapped - would) > 150;
  /* a slow let-go more than half way into the next column goes on to it */
  spongy_list_init (&l, 360, grid);
  spongy_list_down (&l, 300.f, 0);
  for (int i = 1; i <= 40; i++)
    spongy_list_move (&l, 300.f - 4.5f * i, 40.0 * i);
  at = l.offset;
  spongy_list_up (&l, 1610);
  snapped = spongy_list_snap (&l, SPONGY_SNAP_ITEMS, 300, 1610);
  for (double ms = 1613; ms < 4000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("items, let go slowly at %.1f: rests at %.3f (want 300)\n", at, l.offset);
  bad += fabsf (l.offset - 300) > 0.01f || snapped != 300;
  /* flung to the end, it rests at the end, not on the column before it */
  spongy_list_init (&l, 360, grid);
  spongy_list_scroll_to (&l, 2400);
  spongy_list_down (&l, 300.f, 0);
  for (int i = 1; i <= 8; i++)
    spongy_list_move (&l, 300.f - 30.f * i, 8.33 * i);
  spongy_list_up (&l, 70);
  spongy_list_snap (&l, SPONGY_SNAP_ITEMS, 300, 70);
  for (double ms = 73; ms < 3000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("items, flung to the end: rests at %.3f of %.0f\n", l.offset, l.max_offset);
  bad += fabsf (l.offset - l.max_offset) > 0.01f;

  /* pages: a short quick flick goes one page on */
  spongy_list_init (&l, 360, 5 * 360);
  spongy_list_down (&l, 300.f, 0);
  for (int i = 1; i <= 4; i++)
    spongy_list_move (&l, 300.f - 20.f * i, 8.33 * i);
  spongy_list_up (&l, 36);
  snapped = spongy_list_snap (&l, SPONGY_SNAP_PAGES, 360, 36);
  for (double ms = 39; ms < 3000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("pages, a flick: rests at %.3f (want 360)\n", l.offset);
  bad += fabsf (l.offset - 360) > 0.01f || snapped != 360;
  /* dragged slowly a third of the way, it comes back; past half, it goes on */
  spongy_list_down (&l, 300.f, 4000);
  for (int i = 1; i <= 30; i++)
    spongy_list_move (&l, 300.f - 5.f * i, 4000 + 40.0 * i);
  at = l.offset;
  spongy_list_up (&l, 5210);
  spongy_list_snap (&l, SPONGY_SNAP_PAGES, 360, 5210);
  for (double ms = 5213; ms < 8000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("pages, let go slowly at %.1f: rests at %.3f (want 360)\n", at, l.offset);
  bad += fabsf (l.offset - 360) > 0.01f;
  spongy_list_down (&l, 300.f, 9000);
  for (int i = 1; i <= 50; i++)
    spongy_list_move (&l, 300.f - 5.f * i, 9000 + 40.0 * i);
  at = l.offset;
  spongy_list_up (&l, 11010);
  spongy_list_snap (&l, SPONGY_SNAP_PAGES, 360, 11010);
  for (double ms = 11013; ms < 14000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("pages, let go slowly at %.1f: rests at %.3f (want 720)\n", at, l.offset);
  bad += fabsf (l.offset - 720) > 0.01f;
  /* dragged a little on, then flung back: back to the page it was on */
  spongy_list_down (&l, 300.f, 20000);
  for (int i = 1; i <= 4; i++)
    spongy_list_move (&l, 300.f - 16.f * i, 20000 + 30.0 * i);
  for (int i = 1; i <= 3; i++)
    spongy_list_move (&l, 236.f + 10.f * i, 20300 + 8.33 * i);
  at = l.offset;
  spongy_list_up (&l, 20330);
  spongy_list_snap (&l, SPONGY_SNAP_PAGES, 360, 20330);
  for (double ms = 20333; ms < 23000 && l.flinging; ms += 8.33)
    spongy_list_frame (&l, ms, NULL, NULL);
  printf ("pages, flung back from %.1f: rests at %.3f (want 720)\n", at, l.offset);
  bad += fabsf (l.offset - 720) > 0.01f;
  /* nothing to snap while a stretch shows */
  spongy_list_init (&l, 360, 5 * 360);
  spongy_list_down (&l, 100.f, 0);
  for (int i = 1; i <= 20; i++)
    spongy_list_move (&l, 100.f + 8.f * i, 8.33 * i);
  spongy_list_up (&l, 170);
  snapped = spongy_list_snap (&l, SPONGY_SNAP_PAGES, 360, 170);
  printf ("pages, stretched at the start: rests at %.1f, no fling: %s\n", snapped, l.flinging ? "no" : "yes");
  bad += fabsf (l.offset) > 0.01f || l.flinging;

  printf (bad ? "FAILED %d\n" : "ok\n", bad);
  return bad != 0;
}
