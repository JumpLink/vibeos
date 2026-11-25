/* Preloaded into an app by vibeos-start-test: writes to $VIBEOS_START_REPORT how many seconds after the program's
 * exec its GtkApplication ran ("run"), its first window was mapped ("map") and that window drew its first frame
 * ("frame"); in a player that uses libmpv, when the film had played 3 s ("play3", looked at every 100 ms) and the
 * sound output mpv had then ("ao": pipewire, null…). Built with: cc -O2 -shared -fPIC -o start-shim.so start-shim.c $(pkg-config --cflags --libs gtk4) -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* /proc/self/stat's 22nd field is when the process started, in clock ticks after boot, which CLOCK_BOOTTIME counts
 * from too */
static double
since_exec (void)
{
  char buf[2048];
  FILE *f = fopen ("/proc/self/stat", "r");
  size_t n = f ? fread (buf, 1, sizeof buf - 1, f) : 0;
  if (f)
    fclose (f);
  buf[n] = '\0';
  char *p = strrchr (buf, ')');
  if (!p)
    return -1;
  unsigned long long start = 0;
  /* the fields after the command's ")" start at the 3rd, each after a space */
  for (int field = 3; p && field <= 22; field++)
    p = strchr (p + 1, ' ');
  if (!p || sscanf (p + 1, "%llu", &start) != 1)
    return -1;
  struct timespec now;
  clock_gettime (CLOCK_BOOTTIME, &now);
  return now.tv_sec + now.tv_nsec / 1e9 - (double) start / sysconf (_SC_CLK_TCK);
}

static void
report (const char *what)
{
  const char *path = g_getenv ("VIBEOS_START_REPORT");
  FILE *f = path ? fopen (path, "a") : NULL;
  if (!f)
    return;
  fprintf (f, "%s %.3f\n", what, since_exec ());
  fclose (f);
}

typedef struct mpv_handle mpv_handle;

static gboolean
watch_play (gpointer data)
{
  int (*get) (mpv_handle *, const char *, int, void *) = dlsym (RTLD_DEFAULT, "mpv_get_property");
  char *(*get_string) (mpv_handle *, const char *) = dlsym (RTLD_DEFAULT, "mpv_get_property_string");
  double pos = 0;
  /* 5: MPV_FORMAT_DOUBLE */
  if (!get || get (data, "time-pos", 5, &pos) < 0 || pos < 3)
    return G_SOURCE_CONTINUE;
  report ("play3");
  char *ao = get_string ? get_string (data, "current-ao") : NULL;
  const char *path = g_getenv ("VIBEOS_START_REPORT");
  FILE *f = path ? fopen (path, "a") : NULL;
  if (f) {
    fprintf (f, "ao %s\n", ao ? ao : "none");
    fclose (f);
  }
  return G_SOURCE_REMOVE;
}

mpv_handle *
mpv_create (void)
{
  mpv_handle *(*create) (void) = dlsym (RTLD_NEXT, "mpv_create");
  mpv_handle *handle = create ();
  if (handle)
    g_timeout_add (100, watch_play, handle);
  return handle;
}

static gboolean
on_tick (GtkWidget *window, GdkFrameClock *clock, gpointer data)
{
  report ("frame");
  return G_SOURCE_REMOVE;
}

static void
on_map (GtkWidget *window, gpointer data)
{
  report ("map");
}

static void
on_window_added (GtkApplication *app, GtkWindow *window, gpointer data)
{
  static gboolean seen;
  if (seen)
    return;
  seen = TRUE;
  g_signal_connect (window, "map", G_CALLBACK (on_map), NULL);
  gtk_widget_add_tick_callback (GTK_WIDGET (window), on_tick, NULL, NULL);
}

int
g_application_run (GApplication *app, int argc, char **argv)
{
  int (*run) (GApplication *, int, char **) = dlsym (RTLD_NEXT, "g_application_run");
  report ("run");
  if (GTK_IS_APPLICATION (app))
    g_signal_connect (app, "window-added", G_CALLBACK (on_window_added), NULL);
  return run (app, argc, argv);
}
