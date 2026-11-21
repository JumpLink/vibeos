/* ---- Display: the connected displays and the screen scale, through mutter's DisplayConfig ----
 *
 * With more than one display connected (a TV on the USB-C port), Display lists them as mutter names them ("Built-in
 * display", a TV's EDID name), as GNOME Settings' Displays does: Join (a screen of their own each, side by side) or
 * Mirror (one screen on all of them), the main display (where the status bar, the lock screen and the home screen
 * go), and for each external display its resolution, its refresh rate and whether it is on. With only the built-in
 * panel the cards stay hidden and the page is as it always was. The built-in panel's Screen scale row is here too:
 * one D-Bus client for both, and a scale change keeps the other displays where they are.
 *
 * GetCurrentState lists the displays, their modes (each with the scales it takes) and the logical monitors they are
 * in; ApplyMonitorsConfig sets a new layout (ns-monitors.c builds it). Mutter mirrors only displays at the same
 * resolution, so Mirror is offered only where every display has one resolution in common, as GNOME Settings works its
 * mirror modes out; where none exists Mirror shows why instead. Where one does, mutter is asked first whether it
 * would take the mirror (method 0, which checks a configuration without applying it).
 *
 * A change is applied the way GNOME Settings applies it, "persistent" (2): mutter switches at once and has the shell
 * ask "Keep these display settings?"; it writes ~/.config/monitors.xml once the answer is Keep, and goes back by
 * itself after 20 seconds without one, so a TV at a mode it cannot show comes back. (VibeOS's shell puts that
 * question on the built-in panel: the TV may have nothing to answer it with.)
 *
 * The page follows MonitorsChanged: a display plugged in or out, a change kept or gone back, the panel turned.
 *
 * For screenshots without a display to plug in: NS_DISPLAYS_STATE=FILE shows the displays of FILE (a GetCurrentState
 * answer as gdbus prints it), asks mutter nothing and applies nothing (it logs what it would apply). */
#include "ns-caps.h"
#include "ns-monitors.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <math.h>
#include <string.h>

#define DISPLAY_CONFIG "org.gnome.Mutter.DisplayConfig"
#define DISPLAY_CONFIG_PATH "/org/gnome/Mutter/DisplayConfig"

typedef struct {
  GDBusConnection   *bus;           /* the session's; NULL with NS_DISPLAYS_STATE */
  GCancellable      *cancel;
  guint              changed;       /* the MonitorsChanged subscription */
  NsMonitors        *state;         /* NULL until mutter has answered */
  NsPage *page;          /* Display, NULL once it has gone */
  GtkWidget         *list, *modes, *main, *error;     /* the cards, and a line for what mutter refused */
  GtkWidget         *scale_group, *scale_row;
  GtkWidget         *size_group, *size_row, *size_scale;   /* Display size */
  GArray            *size_stops;    /* NsSizeStop: the sizes the row offers */
  gboolean           size_enabled;
  guint              size_timer;    /* a size picked on the slider, applied once it stops moving */
  GSettings         *iface;         /* vibeos.Interface, NULL where it is not installed */
  GPtrArray         *subs;          /* the displays' own pages open now (Sub, not owned) */
  char              *mirror_refused;  /* mutter's answer to Mirror (method 0), NULL while it would take it */
  guint32            mirror_checked;  /* the serial that answer is for */
} Displays;

/* a display's own page */
typedef struct {
  Displays          *d;             /* a reference */
  char              *connector;
  NsPage *page;
  GtkWidget         *power, *modes, *error;
} Sub;

static void read_state (Displays *d);
static void fill (Displays *d);
static void fill_size (Displays *d);
static Displays *displays_attach (GtkWidget *page);

static void
displays_clear (Displays *d)
{
  ns_monitors_free (d->state);
  g_clear_object (&d->bus);
  g_clear_object (&d->cancel);
  g_ptr_array_unref (d->subs);
  g_free (d->mirror_refused);
  g_clear_pointer (&d->size_stops, g_array_unref);
  g_clear_object (&d->iface);
}

static Displays *
displays_ref (Displays *d)
{
  return g_rc_box_acquire (d);
}

static void
displays_unref (Displays *d)
{
  g_rc_box_release_full (d, (GDestroyNotify) displays_clear);
}

/* the Display page went: nothing more is asked or shown */
static void
displays_detach (Displays *d)
{
  g_cancellable_cancel (d->cancel);
  if (d->changed)
    g_dbus_connection_signal_unsubscribe (d->bus, d->changed);
  d->changed = 0;
  d->page = NULL;
  d->list = d->modes = d->main = d->error = d->scale_group = d->scale_row = NULL;
  d->size_group = d->size_row = d->size_scale = NULL;
  g_clear_handle_id (&d->size_timer, g_source_remove);
  displays_unref (d);
}

/* ---- labels ---- */

/* the aspect ratio as GNOME Settings names it, from the ratio's first decimal */
static const char *
aspect (int w, int h)
{
  if (!w || !h)
    return NULL;
  switch (w > h ? w * 10 / h : h * 10 / w) {
  case 10: return "1:1";
  case 12: return "5:4";
  case 13: return "4:3";
  case 15: return "3:2";
  case 16: return "16:10";
  case 17: return "16:9";
  case 18: return "9:5";
  case 23: return "21:9";
  default: return NULL;
  }
}

static char *
resolution_label (int w, int h)
{
  const char *a = aspect (w, h);
  return a ? g_strdup_printf ("%d × %d (%s)", w, h, a) : g_strdup_printf ("%d × %d", w, h);
}

/* 60 Hz, 59.94 Hz: whole rates without decimals (the panel's 119.99997 is 120); a no-break space keeps "Hz" with its
 * number where a summary wraps */
static char *
rate_label (NsMode *mode)
{
  double r = round (mode->refresh);
  g_autofree char *hz = fabs (mode->refresh - r) < 0.005 ? g_strdup_printf ("%.0f\u00a0Hz", r)
                                                          : g_strdup_printf ("%.2f\u00a0Hz", mode->refresh);
  return mode->interlaced ? g_strdup_printf ("%s, interlaced", hz) : g_steal_pointer (&hz);
}

/* "Main display • 1920 × 1080 • 60 Hz", "Off" */
static char *
summary (NsMonitors *m, NsMonitor *mon)
{
  if (!mon->logical || !mon->mode)
    return g_strdup ("Off");
  g_autofree char *size = g_strdup_printf ("%d\u00a0×\u00a0%d", mon->mode->width, mon->mode->height);
  g_autofree char *rate = rate_label (mon->mode);
  const char *role = mon->logical->monitors->len > 1 ? "Mirrored"
                   : mon->logical->primary && m->logical->len > 1 ? "Main display" : NULL;
  return role ? g_strdup_printf ("%s • %s • %s", role, size, rate) : g_strdup_printf ("%s • %s", size, rate);
}

/* ---- applying ---- */

static void
show_refusal (GtkWidget *where, const char *message)
{
  if (!where || !gtk_widget_get_parent (where))
    return;
  g_autofree char *text = g_strdup_printf ("Not changed: %s", message);
  if (GTK_IS_LABEL (where)) {
    gtk_label_set_text (GTK_LABEL (where), text);
    gtk_widget_set_visible (where, TRUE);
  } else {
    ns_row_set_subtitle (where, text);
  }
}

typedef struct {
  Displays  *d;
  GtkWidget *where;             /* the line or the row to say a refusal in */
} Applying;

static void
applied (GObject *src, GAsyncResult *res, gpointer data)
{
  Applying *a = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!v && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && a->d->page) {
    g_dbus_error_strip_remote_error (err);
    /* the switch or the picker back to how the displays are */
    fill (a->d);
    show_refusal (a->where, err->message);
  }
  g_clear_object (&a->where);
  displays_unref (a->d);
  g_free (a);
}

/* Mutter switches at once and the shell asks whether to keep it (see the top). What comes back is MonitorsChanged,
 * which fills the page again; a refusal is said at `where`. */
static void
apply (Displays *d, NsConfig *c, GtkWidget *where)
{
  if (!d->state || !c)
    return;
  GVariant *args = ns_config_to_variant (d->state, c, NS_MONITORS_PERSISTENT);
  if (!d->bus) {
    g_autoptr (GVariant) sunk = g_variant_ref_sink (args);
    g_autofree char *text = g_variant_print (sunk, TRUE);
    g_message ("NS_DISPLAYS_STATE: not applied: ApplyMonitorsConfig %s", text);
    return;
  }
  Applying *a = g_new0 (Applying, 1);
  a->d = displays_ref (d);
  a->where = where ? g_object_ref (where) : NULL;
  g_dbus_connection_call (d->bus, DISPLAY_CONFIG, DISPLAY_CONFIG_PATH, DISPLAY_CONFIG, "ApplyMonitorsConfig", args,
                          NULL, G_DBUS_CALL_FLAGS_NONE, 10000, d->cancel, applied, a);
}

/* ---- a display's own page: on or off, resolution, refresh rate ---- */

static void
sub_free (Sub *s)
{
  g_ptr_array_remove (s->d->subs, s);
  displays_unref (s->d);
  g_free (s->connector);
  g_free (s);
}

static NsMonitor *
sub_monitor (Sub *s)
{
  return s->d->state ? ns_monitors_find (s->d->state, s->connector) : NULL;
}

static void
power_toggled (GtkWidget *sw, GParamSpec *ps, gpointer data)
{
  Sub *s = data;
  NsMonitor *mon = sub_monitor (s);
  if (ns_switch_syncing (GTK_WIDGET (sw)) || !mon)
    return;
  g_autoptr (NsConfig) c = ns_config_current (s->d->state);
  ns_config_set_on (s->d->state, c, mon, ns_switch_get_active (sw));
  apply (s->d, c, s->error);
}

static void
resolution_picked (GtkWidget *row, guint i, gpointer data)
{
  Sub *s = data;
  NsMonitor *mon = sub_monitor (s);
  GArray *sizes = g_object_get_data (G_OBJECT (row), "ns-sizes");
  if (!mon || !mon->mode || i >= sizes->len)
    return;
  NsSize size = g_array_index (sizes, NsSize, i);
  if (size.width == mon->mode->width && size.height == mon->mode->height)
    return;
  g_autoptr (NsConfig) c = ns_config_current (s->d->state);
  ns_config_set_mode (s->d->state, c, mon, ns_monitor_mode_for_size (mon, size.width, size.height, mon->mode->refresh));
  apply (s->d, c, s->error);
}

static void
rate_picked (GtkWidget *row, guint i, gpointer data)
{
  Sub *s = data;
  NsMonitor *mon = sub_monitor (s);
  const char *const *ids = g_object_get_data (G_OBJECT (row), "ns-modes");
  if (!mon || !mon->mode || i >= g_strv_length ((char **) ids) || !g_strcmp0 (ids[i], mon->mode->id))
    return;
  g_autoptr (NsConfig) c = ns_config_current (s->d->state);
  ns_config_set_mode (s->d->state, c, mon, ns_monitor_find_mode (mon, ids[i]));
  apply (s->d, c, s->error);
}

/* the resolutions a display offers: its modes' sizes, or in a mirror the sizes all the displays have */
static GArray *
sizes_of (NsMonitors *m, NsMonitor *mon)
{
  if (mon->logical && mon->logical->monitors->len > 1)
    return ns_monitors_mirror_sizes (m);
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (NsSize));
  for (guint i = 0; i < mon->modes->len; i++) {
    NsMode *mo = mon->modes->pdata[i];
    gboolean seen = FALSE;
    for (guint k = 0; k < sizes->len && !seen; k++)
      seen = g_array_index (sizes, NsSize, k).width == mo->width && g_array_index (sizes, NsSize, k).height == mo->height;
    if (!seen)
      g_array_append_val (sizes, ((NsSize) { mo->width, mo->height }));
  }
  return sizes;
}

static void
fill_sub (Sub *s)
{
  NsMonitors *m = s->d->state;
  NsMonitor *mon = sub_monitor (s);
  if (!mon) {
    /* unplugged: back to Display */
    if (s->d->page)
      ns_pop_to (s->d->page);
    return;
  }
  ns_group_clear (s->power);
  ns_group_clear (s->modes);
  gtk_widget_set_visible (s->error, FALSE);

  /* on and off for an external display, and for the built-in panel while it is off (turned off, it could only be
   * turned on again from the external display, which may have nothing to touch or click with) */
  gboolean power = !mon->builtin || !mon->logical;
  if (power) {
    GtkWidget *r = ns_row_switch (s->power, "Use this display",
                                  mon->logical && ns_monitors_n_on (m) < 2 ? "The only display that is on" : NULL,
                                  NULL, NULL);
    GtkWidget *sw = ns_row_get_switch (r);
    ns_switch_quiet (sw, mon->logical != NULL);
    gtk_widget_set_sensitive (r, !mon->logical || ns_monitors_n_on (m) > 1);
    g_signal_connect (sw, "notify::active", G_CALLBACK (power_toggled), s);
  }
  ns_group_set_visible (s->power, power);

  if (mon->logical && mon->mode) {
    g_autoptr (GArray) sizes = sizes_of (m, mon);
    g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
    guint sel = 0;
    for (guint i = 0; i < sizes->len; i++) {
      NsSize sz = g_array_index (sizes, NsSize, i);
      g_ptr_array_add (labels, resolution_label (sz.width, sz.height));
      if (sz.width == mon->mode->width && sz.height == mon->mode->height)
        sel = i;
    }
    g_ptr_array_add (labels, NULL);
    GtkWidget *r = ns_row_pick (s->modes, "Resolution", (const char *const *) labels->pdata, sel, resolution_picked, s);
    g_object_set_data_full (G_OBJECT (r), "ns-sizes", g_array_ref (sizes), (GDestroyNotify) g_array_unref);

    g_autoptr (GPtrArray) rates = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
    sel = 0;
    for (guint i = 0; i < mon->modes->len; i++) {
      NsMode *mo = mon->modes->pdata[i];
      if (mo->width != mon->mode->width || mo->height != mon->mode->height)
        continue;
      if (mo == mon->mode)
        sel = ids->len;
      g_ptr_array_add (rates, rate_label (mo));
      g_ptr_array_add (ids, g_strdup (mo->id));
    }
    g_ptr_array_add (rates, NULL);
    g_ptr_array_add (ids, NULL);
    r = ns_row_pick (s->modes, "Refresh rate", (const char *const *) rates->pdata, sel, rate_picked, s);
    g_object_set_data_full (G_OBJECT (r), "ns-modes", g_ptr_array_steal (ids, NULL), (GDestroyNotify) g_strfreev);
  }
  ns_group_set_visible (s->modes, mon->logical != NULL);
}

static void
display_open (GtkWidget *row, gpointer data)
{
  Displays *d = data;
  const char *connector = g_object_get_data (G_OBJECT (row), "ns-connector");
  NsMonitor *mon = d->state ? ns_monitors_find (d->state, connector) : NULL;
  if (!mon)
    return;
  GtkWidget *c;
  Sub *s = g_new0 (Sub, 1);
  s->d = displays_ref (d);
  s->connector = g_strdup (connector);
  s->page = ns_page_new (mon->name, &c);
  s->power = ns_group (c, NULL);
  s->modes = ns_group (c, NULL);
  s->error = ns_note (c, "");
  g_object_set_data_full (G_OBJECT (s->page), "ns-display", s, (GDestroyNotify) sub_free);
  g_ptr_array_add (d->subs, s);
  fill_sub (s);
  ns_push (s->page);
}

/* ---- the cards on Display ---- */

static GtkWidget *
choice_row (GtkWidget *g, const char *title, const char *subtitle, gboolean on, NsActivate picked, gpointer data)
{
  GtkWidget *r = ns_row (g, NULL, title, subtitle);
  GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (mark, "ns-accent");
  gtk_widget_set_opacity (mark, on);
  ns_row_suffix (r, mark);
  ns_row_on_activate (r, picked, data);
  return r;
}

static void
join_picked (GtkWidget *row, gpointer data)
{
  Displays *d = data;
  if (!d->state || !ns_monitors_is_mirror (d->state))
    return;
  g_autoptr (NsConfig) c = ns_config_join (d->state);
  apply (d, c, d->error);
}

/* GNOME Settings' choice of mirror mode: the biggest size they all have */
static NsConfig *
mirror_config (NsMonitors *m)
{
  g_autoptr (GArray) sizes = ns_monitors_mirror_sizes (m);
  if (!sizes->len)
    return NULL;
  return ns_config_mirror (m, g_array_index (sizes, NsSize, 0).width, g_array_index (sizes, NsSize, 0).height);
}

static void
mirror_picked (GtkWidget *row, gpointer data)
{
  Displays *d = data;
  if (!d->state || ns_monitors_is_mirror (d->state))
    return;
  g_autoptr (NsConfig) c = mirror_config (d->state);
  apply (d, c, d->error);
}

static void
main_picked (GtkWidget *row, guint i, gpointer data)
{
  Displays *d = data;
  const char *const *connectors = g_object_get_data (G_OBJECT (row), "ns-connectors");
  NsMonitor *mon = d->state && i < g_strv_length ((char **) connectors) ? ns_monitors_find (d->state, connectors[i])
                                                                       : NULL;
  if (!mon || !mon->logical || mon->logical->primary)
    return;
  g_autoptr (NsConfig) c = ns_config_current (d->state);
  ns_config_set_primary (c, mon);
  apply (d, c, d->error);
}

static gint
left_first (gconstpointer a, gconstpointer b)
{
  const NsLogical *x = *(NsLogical *const *) a, *y = *(NsLogical *const *) b;
  return x->x != y->x ? (x->x < y->x ? -1 : 1) : (x->y > y->y) - (x->y < y->y);
}

static void
fill_cards (Displays *d)
{
  NsMonitors *m = d->state;
  gboolean several = m && m->monitors->len > 1;
  if (!d->list)
    return;
  ns_group_clear (d->list);
  ns_group_clear (d->modes);
  ns_group_clear (d->main);
  ns_group_set_visible (d->list, several);
  ns_group_set_visible (d->modes, several);
  ns_group_set_visible (d->main, FALSE);
  gtk_widget_set_visible (d->error, FALSE);
  if (!several)
    return;

  for (guint i = 0; i < m->monitors->len; i++) {
    NsMonitor *mon = m->monitors->pdata[i];
    g_autofree char *sub = summary (m, mon);
    GtkWidget *r = ns_row (d->list, mon->builtin ? "phone-symbolic" : "video-display-symbolic", mon->name, sub);
    /* the built-in panel has a page only while it is off: its scale is below, and it has one mode */
    if (!mon->builtin || !mon->logical) {
      g_object_set_data_full (G_OBJECT (r), "ns-connector", g_strdup (mon->connector), g_free);
      ns_row_on_activate (r, display_open, d);
    }
  }

  gboolean mirror = ns_monitors_is_mirror (m);
  g_autoptr (GArray) sizes = ns_monitors_mirror_sizes (m);
  choice_row (d->modes, "Join", "A screen of its own on each display", !mirror, join_picked, d);
  const char *why = NULL;
  g_autofree char *refused = NULL;
  if (!mirror && !sizes->len)
    why = "Not available: the displays have no resolution in common";
  else if (!mirror && d->mirror_refused && d->mirror_checked == m->serial)
    why = refused = g_strdup_printf ("Not available: %s", d->mirror_refused);
  GtkWidget *r = choice_row (d->modes, "Mirror", why ? why : "The same screen on every display", mirror,
                             mirror_picked, d);
  gtk_widget_set_sensitive (r, !why);

  /* the main display, among the screens that are on */
  if (m->logical->len > 1) {
    g_autoptr (GPtrArray) logical = g_ptr_array_new ();
    g_ptr_array_extend (logical, m->logical, NULL, NULL);
    g_ptr_array_sort (logical, left_first);
    g_autoptr (GPtrArray) names = g_ptr_array_new ();
    g_autoptr (GPtrArray) connectors = g_ptr_array_new_with_free_func (g_free);
    guint sel = 0;
    for (guint i = 0; i < logical->len; i++) {
      NsLogical *l = logical->pdata[i];
      NsMonitor *mon = l->monitors->len ? l->monitors->pdata[0] : NULL;
      if (!mon)
        continue;
      if (l->primary)
        sel = names->len;
      g_ptr_array_add (names, mon->name);
      g_ptr_array_add (connectors, g_strdup (mon->connector));
    }
    g_ptr_array_add (names, NULL);
    g_ptr_array_add (connectors, NULL);
    r = ns_row_pick (d->main, "Main display", (const char *const *) names->pdata, sel, main_picked, d);
    g_object_set_data_full (G_OBJECT (r), "ns-connectors", g_ptr_array_steal (connectors, NULL),
                            (GDestroyNotify) g_strfreev);
    ns_group_set_visible (d->main, TRUE);
  }
}

/* ---- the screen scale: the built-in panel's (or the main display's, without one) ---- */

static NsMonitor *
scaled_monitor (NsMonitors *m)
{
  NsMonitor *panel = ns_monitors_builtin (m);
  return panel ? panel : ns_monitors_primary (m);
}

static void
scale_picked (GtkWidget *row, guint i, gpointer data)
{
  Displays *d = data;
  GArray *scales = g_object_get_data (G_OBJECT (row), "ns-scales");
  NsMonitor *mon = d->state ? scaled_monitor (d->state) : NULL;
  if (!mon || !mon->logical || i >= scales->len)
    return;
  g_autoptr (NsConfig) c = ns_config_current (d->state);
  ns_config_set_scale (d->state, c, mon, g_array_index (scales, double, i));
  apply (d, c, row);
}

static void
fill_scale (Displays *d)
{
  if (!d->scale_group)
    return;
  NsMonitor *mon = d->state ? scaled_monitor (d->state) : NULL;
  g_autoptr (NsConfig) c = mon && mon->logical ? ns_config_current (d->state) : NULL;
  g_autoptr (GArray) scales = c ? ns_config_scales (c, mon) : NULL;
  if (!scales || scales->len < 2) {
    if (d->scale_row)
      ns_group_remove (d->scale_group, d->scale_row);
    d->scale_row = NULL;
    return;
  }
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  guint sel = 0;
  double cur = mon->logical->scale;
  for (guint i = 0; i < scales->len; i++) {
    double s = g_array_index (scales, double, i);
    g_ptr_array_add (labels, g_strdup_printf ("%.0f%%", s * 100));
    if (fabs (s - cur) < fabs (g_array_index (scales, double, sel) - cur))
      sel = i;
  }
  g_ptr_array_add (labels, NULL);
  /* the same choices: only the one picked moves; other ones: a new row in the old one's place, the last of its card */
  GArray *had = d->scale_row ? g_object_get_data (G_OBJECT (d->scale_row), "ns-scales") : NULL;
  gboolean same = had && had->len == scales->len && !memcmp (had->data, scales->data, scales->len * sizeof (double));
  if (!same) {
    if (d->scale_row)
      ns_group_remove (d->scale_group, d->scale_row);
    d->scale_row = ns_row_pick (d->scale_group, "Screen scale", (const char *const *) labels->pdata, sel, scale_picked,
                                d);
    g_object_set_data_full (G_OBJECT (d->scale_row), "ns-scales", g_array_ref (scales), (GDestroyNotify) g_array_unref);
  } else {
    ns_row_pick_select (d->scale_row, sel);
  }
}

/* ---- Display size: the built-in panel's scale, in steps from its own ---- */

/* the screen's own scale and the step from it, as Display size last set them; the scale there is now and step 0
 * before it ever did */
static double
size_native (Displays *d, double cur, int *step)
{
  double native = 0;
  *step = 0;
  if (d->iface)
    g_settings_get (d->iface, "display-size", "(di)", &native, step);
  if (native <= 0) {
    native = cur;
    *step = 0;
  }
  return native;
}

static gboolean
size_apply (gpointer data)
{
  Displays *d = data;
  d->size_timer = 0;
  NsMonitor *mon = d->state ? scaled_monitor (d->state) : NULL;
  if (!mon || !mon->logical || !d->size_scale || !d->size_stops)
    return G_SOURCE_REMOVE;
  guint i = (guint) round (gtk_range_get_value (GTK_RANGE (d->size_scale)));
  if (i >= d->size_stops->len)
    return G_SOURCE_REMOVE;
  NsSizeStop *stop = &g_array_index (d->size_stops, NsSizeStop, i);
  int was;
  double native = size_native (d, mon->logical->scale, &was);
  if (d->iface)
    g_settings_set (d->iface, "display-size", "(di)", native, stop->step);
  if (fabs (stop->scale - mon->logical->scale) < 1e-6)
    return G_SOURCE_REMOVE;
  g_autoptr (NsConfig) c = ns_config_current (d->state);
  ns_config_set_scale (d->state, c, mon, stop->scale);
  apply (d, c, d->size_row);
  return G_SOURCE_REMOVE;
}

static void
size_moved (GtkRange *range, Displays *d)
{
  if (g_object_get_data (G_OBJECT (range), "ns-syncing"))
    return;
  /* dragged across several sizes, only the one it stops at is applied */
  g_clear_handle_id (&d->size_timer, g_source_remove);
  d->size_timer = g_timeout_add (600, size_apply, d);
}

static void
fill_size (Displays *d)
{
  if (!d->size_group)
    return;
  /* not applied yet: the steps a phone or a tablet has, at the default one, following nothing */
  if (!d->size_enabled) {
    if (!d->size_row) {
      gboolean tablet = g_str_equal (ns_device_noun (), "tablet");
      d->size_row = ns_row_steps (d->size_group, "Display size", tablet ? 7 : 5, 3, &d->size_scale);
      gtk_widget_set_sensitive (d->size_row, FALSE);
    }
    return;
  }
  NsMonitor *mon = d->state ? scaled_monitor (d->state) : NULL;
  g_autoptr (NsConfig) c = mon && mon->logical ? ns_config_current (d->state) : NULL;
  g_autoptr (GArray) scales = c ? ns_config_scales (c, mon) : NULL;
  double cur = mon && mon->logical ? mon->logical->scale : 0;
  int step;
  double native = size_native (d, cur, &step);
  int short_side = mon && mon->mode ? MIN (mon->mode->width, mon->mode->height) : 0;
  g_autoptr (GArray) stops = ns_display_size_stops (scales, native, short_side);
  if (stops->len < 2) {
    if (d->size_row)
      ns_group_remove (d->size_group, d->size_row);
    d->size_row = d->size_scale = NULL;
    g_clear_pointer (&d->size_stops, g_array_unref);
    return;
  }
  /* the size the screen is at: its step when it is one of them, else the one closest to it */
  guint sel = 0;
  for (guint i = 0; i < stops->len; i++)
    if (fabs (g_array_index (stops, NsSizeStop, i).scale - cur) < fabs (g_array_index (stops, NsSizeStop, sel).scale - cur))
      sel = i;
  gboolean same = d->size_stops && d->size_stops->len == stops->len
                  && !memcmp (d->size_stops->data, stops->data, stops->len * sizeof (NsSizeStop));
  if (!same) {
    if (d->size_row)
      ns_group_remove (d->size_group, d->size_row);
    d->size_row = ns_row_steps (d->size_group, "Display size", stops->len, sel, &d->size_scale);
    g_signal_connect (d->size_scale, "value-changed", G_CALLBACK (size_moved), d);
    g_clear_pointer (&d->size_stops, g_array_unref);
    d->size_stops = g_array_ref (stops);
  } else if (!d->size_timer) {
    g_object_set_data (G_OBJECT (d->size_scale), "ns-syncing", GINT_TO_POINTER (1));
    gtk_range_set_value (GTK_RANGE (d->size_scale), sel);
    g_object_set_data (G_OBJECT (d->size_scale), "ns-syncing", NULL);
  }
}

void
ns_displays_size_row (GtkWidget *page, GtkWidget *group, gboolean enabled)
{
  Displays *d = displays_attach (page);
  d->size_group = group;
  d->size_enabled = enabled;
  fill_size (d);
}

void
ns_displays_size_reset (GtkWidget *page)
{
  Displays *d = g_object_get_data (G_OBJECT (page), "ns-displays");
  if (!d || !d->size_enabled || !d->size_scale || !d->size_stops)
    return;
  for (guint i = 0; i < d->size_stops->len; i++)
    if (g_array_index (d->size_stops, NsSizeStop, i).step == 0) {
      /* moved like a drag that stops there: applied once, a moment later */
      gtk_range_set_value (GTK_RANGE (d->size_scale), i);
      return;
    }
}

/* ---- following mutter ---- */

static void
fill (Displays *d)
{
  if (!d->page)
    return;
  fill_cards (d);
  fill_scale (d);
  fill_size (d);
  /* a copy: a page whose display went is popped, and leaves the list */
  g_autoptr (GPtrArray) subs = g_ptr_array_copy (d->subs, NULL, NULL);
  for (guint i = 0; i < subs->len; i++)
    if (g_ptr_array_find (d->subs, subs->pdata[i], NULL))
      fill_sub (subs->pdata[i]);
}

typedef struct {
  Displays *d;
  guint32   serial;             /* the state the check is for */
} Check;

static void
mirror_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  Check *check = data;
  Displays *d = check->d;
  guint32 serial = check->serial;
  g_free (check);
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && d->page && d->state && d->state->serial == serial) {
    g_clear_pointer (&d->mirror_refused, g_free);
    if (!v) {
      g_dbus_error_strip_remote_error (err);
      d->mirror_refused = g_strdup (err->message);
    }
    d->mirror_checked = serial;
    fill_cards (d);
  }
  displays_unref (d);
}

/* whether mutter would take Mirror, asked once per state (method 0 applies nothing) */
static void
check_mirror (Displays *d)
{
  NsMonitors *m = d->state;
  if (!d->bus || !m || m->monitors->len < 2 || ns_monitors_is_mirror (m) || d->mirror_checked == m->serial)
    return;
  g_autoptr (NsConfig) c = mirror_config (m);
  if (!c)
    return;
  Check *check = g_new0 (Check, 1);
  check->d = displays_ref (d);
  check->serial = m->serial;
  g_dbus_connection_call (d->bus, DISPLAY_CONFIG, DISPLAY_CONFIG_PATH, DISPLAY_CONFIG, "ApplyMonitorsConfig",
                          ns_config_to_variant (m, c, NS_MONITORS_VERIFY), NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                          d->cancel, mirror_checked, check);
}

static void
state_got (GObject *src, GAsyncResult *res, gpointer data)
{
  Displays *d = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  NsMonitors *m = v ? ns_monitors_new (v, &err) : NULL;
  if (m && d->page) {
    ns_monitors_free (d->state);
    d->state = m;
    fill (d);
    check_mirror (d);
  } else {
    ns_monitors_free (m);
    if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug ("DisplayConfig: %s", err->message);
  }
  displays_unref (d);
}

static void
read_state (Displays *d)
{
  g_dbus_connection_call (d->bus, DISPLAY_CONFIG, DISPLAY_CONFIG_PATH, DISPLAY_CONFIG, "GetCurrentState", NULL,
                          G_VARIANT_TYPE (NS_MONITORS_STATE_TYPE), G_DBUS_CALL_FLAGS_NONE, 3000, d->cancel, state_got,
                          displays_ref (d));
}

static void
monitors_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
                  GVariant *params, gpointer data)
{
  read_state (data);
}

/* the page's D-Bus client, made with the first part of the page that needs it */
static Displays *
displays_attach (GtkWidget *page)
{
  Displays *d = g_object_get_data (G_OBJECT (page), "ns-displays");
  if (d)
    return d;
  d = g_rc_box_new0 (Displays);
  d->page = page;
  d->cancel = g_cancellable_new ();
  d->subs = g_ptr_array_new ();
  d->iface = ns_settings ("vibeos.Interface");
  if (d->iface && !ns_has_key (d->iface, "display-size"))
    g_clear_object (&d->iface);
  g_object_set_data_full (G_OBJECT (page), "ns-displays", d, (GDestroyNotify) displays_detach);

  const char *file = g_getenv ("NS_DISPLAYS_STATE");
  if (file && *file) {
    g_autofree char *text = NULL;
    g_autoptr (GError) err = NULL;
    if (g_file_get_contents (file, &text, NULL, &err))
      d->state = ns_monitors_new_from_text (text, &err);
    if (err)
      g_warning ("NS_DISPLAYS_STATE: %s", err->message);
    return d;
  }
  d->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!d->bus)
    return d;
  d->changed = g_dbus_connection_signal_subscribe (d->bus, DISPLAY_CONFIG, DISPLAY_CONFIG, "MonitorsChanged",
                                                   DISPLAY_CONFIG_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                   monitors_changed, displays_ref (d),
                                                   (GDestroyNotify) displays_unref);
  read_state (d);
  return d;
}

void
ns_displays_cards (GtkWidget *page, GtkWidget *content)
{
  Displays *d = displays_attach (page);
  d->list = ns_group (content, "Displays");
  d->modes = ns_group (content, NULL);
  d->main = ns_group (content, NULL);
  d->error = ns_note (content, "");
  fill_cards (d);
}

void
ns_displays_scale_row (GtkWidget *page, GtkWidget *group)
{
  Displays *d = g_object_get_data (G_OBJECT (page), "ns-displays");
  if (!d)
    return;
  d->scale_group = group;
  fill_scale (d);
}
