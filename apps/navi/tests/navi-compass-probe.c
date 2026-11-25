/* navi-compass-probe [SECONDS] [--landscape]: the compass as Navi reads it, without Navi's window.
 *
 * Claims iio-sensor-proxy's compass through NaviCompass, prints the screen top's heading every half second (and
 * each "changed"), then releases the claim and exits. Turn the phone while it runs: the heading should follow.
 * With --landscape it reads it as a screen turned to landscape (the accelerometer says which side is up). */
#include "navi-compass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static int changes;

static void
on_changed (NaviCompass *compass, gpointer data)
{
  double h;
  changes++;
  if (navi_compass_get_heading (compass, &h))
    printf ("changed: %.1f\n", h);
  else
    printf ("changed: no heading\n");
  fflush (stdout);
}

static gboolean
tick (gpointer data)
{
  double h;
  if (navi_compass_get_heading (data, &h))
    printf ("heading %.1f\n", h);
  else
    printf ("heading: none yet\n");
  fflush (stdout);
  return G_SOURCE_CONTINUE;
}

static gboolean
stop (gpointer data)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  int seconds = 10;
  gboolean landscape = FALSE;
  for (int i = 1; i < argc; i++) {
    if (!strcmp (argv[i], "--landscape"))
      landscape = TRUE;
    else
      seconds = CLAMP (atoi (argv[i]), 1, 600);
  }
  loop = g_main_loop_new (NULL, FALSE);
  NaviCompass *compass = navi_compass_new ();
  g_signal_connect (compass, "changed", G_CALLBACK (on_changed), NULL);
  navi_compass_set_landscape (compass, landscape);
  navi_compass_set_active (compass, TRUE);
  g_timeout_add (500, tick, compass);
  g_timeout_add_seconds (seconds, stop, NULL);
  g_main_loop_run (loop);
  /* the release goes out before the process leaves */
  navi_compass_set_active (compass, FALSE);
  GMainContext *ctx = g_main_context_default ();
  for (int i = 0; i < 20; i++)
    g_main_context_iteration (ctx, FALSE), g_usleep (10000);
  printf ("released after %d changes\n", changes);
  g_object_unref (compass);
  return 0;
}
