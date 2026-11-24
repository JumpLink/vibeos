/* The battery's charge since it was last full, as Android's battery page draws it, from UPower's own history (the
 * battery device's GetHistory "charge": a time, a percentage and a state per change; the display device keeps none).
 * The battery is the first device UPower lists that is a battery powering the phone and keeps a history, whatever
 * the kernel calls it.
 *
 * Last full: the newest point UPower marked "fully charged", or charging at 99 % or more. The chart runs from there
 * (from 24 hours ago when the week UPower keeps has none) to now: the charge as an area, the times it was charging
 * tinted green, and nothing where UPower knew nothing (it marks "unknown" when it starts again after the phone was
 * off). It reads the history again every minute while it is shown. */
#include "ns-charge.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

#define UPOWER "org.freedesktop.UPower"
#define DEVICE UPOWER ".Device"
/* as far back as UPower keeps it, and that many points at most (it averages when there are more) */
#define HISTORY_SPAN (7 * 24 * 3600)
#define HISTORY_POINTS 4000
#define DAY (24 * 3600)

enum { STATE_UNKNOWN = 0, STATE_CHARGING = 1, STATE_DISCHARGING = 2, STATE_FULL = 4 };

typedef struct {
  gint64 time;
  double percent;
  guint  state;
} Point;

typedef struct {
  GtkWidget       *box;                  /* the whole thing; it keeps this */
  GtkWidget       *area, *note, *empty;
  char            *start;                /* the time under the chart's start */
  GDBusConnection *bus;
  char            *battery;              /* UPower's object path */
  char           **devices;              /* while looking for it */
  guint            next;
  GArray          *points;               /* Point, oldest first, from `from` on */
  gint64           from, now, full;      /* full: when it was last full, or 0 */
  guint            timer;
  gboolean         loading;
} Charge;

static void
charge_free (Charge *c)
{
  g_clear_handle_id (&c->timer, g_source_remove);
  g_clear_object (&c->bus);
  g_free (c->battery);
  g_strfreev (c->devices);
  g_clear_pointer (&c->points, g_array_unref);
  g_free (c->start);
  g_free (c);
}

/* ---- words ---- */

/* "13:20", "Yesterday 13:20", "Mon 13:20" (or 1:20 PM), as the clock shows times, in the user's language */
static char *
when_text (gint64 time)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (time);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *clock = vibeos_format_time (t);
  g_autoptr (GDate) d0 = g_date_new_dmy (g_date_time_get_day_of_month (t), g_date_time_get_month (t),
                                         g_date_time_get_year (t));
  g_autoptr (GDate) d1 = g_date_new_dmy (g_date_time_get_day_of_month (now), g_date_time_get_month (now),
                                         g_date_time_get_year (now));
  int days = g_date_days_between (d0, d1);
  if (days == 0)
    return g_steal_pointer (&clock);
  if (days == 1)
    /* TRANSLATORS: %s is a time ("13:20") */
    return g_strdup_printf (_("Yesterday %s"), clock);
  /* the weekday and the time, in the language's order */
  return vibeos_format_date (t, "EEEjm");
}

/* "just now", "25 min", "3 h 12 min", "2 d 5 h" */
static char *
ago_text (gint64 seconds)
{
  gint64 minutes = seconds / 60, hours = minutes / 60;
  if (minutes < 1)
    return g_strdup (_("just now"));
  int m = (int) MIN (minutes, G_MAXINT), h = (int) MIN (hours, G_MAXINT);
  if (hours < 1)
    /* TRANSLATORS: minutes ago, abbreviated */
    return g_strdup_printf (g_dngettext (NULL, "%d min ago", "%d min ago", m), m);
  if (hours < 48)
    /* TRANSLATORS: hours and minutes ago, abbreviated; then hours ago */
    return minutes % 60 ? g_strdup_printf (_("%d h %d min ago"), h, m % 60)
                        : g_strdup_printf (g_dngettext (NULL, "%d h ago", "%d h ago", h), h);
  /* TRANSLATORS: days and hours ago, abbreviated; then days ago */
  return hours % 24 ? g_strdup_printf (_("%d d %d h ago"), h / 24, h % 24)
                    : g_strdup_printf (g_dngettext (NULL, "%d d ago", "%d d ago", h / 24), h / 24);
}

/* ---- the chart ---- */

static void
rgba (cairo_t *cr, const GdkRGBA *c, double alpha)
{
  cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha * alpha);
}

/* the charge so far as one stretch of line and area: from the last gap to here */
static void
stroke_stretch (cairo_t *cr, GArray *xy, double bottom, const GdkRGBA *accent)
{
  if (xy->len == 0)
    return;
  const graphene_point_t *p = (const graphene_point_t *) xy->data;
  if (xy->len == 1) {
    /* one point between two gaps: a dot */
    rgba (cr, accent, 1);
    cairo_arc (cr, p[0].x, p[0].y, 2.5, 0, 2 * G_PI);
    cairo_fill (cr);
    g_array_set_size (xy, 0);
    return;
  }
  cairo_move_to (cr, p[0].x, bottom);
  for (guint i = 0; i < xy->len; i++)
    cairo_line_to (cr, p[i].x, p[i].y);
  cairo_line_to (cr, p[xy->len - 1].x, bottom);
  cairo_close_path (cr);
  rgba (cr, accent, 0.18);
  cairo_fill (cr);
  cairo_move_to (cr, p[0].x, p[0].y);
  for (guint i = 1; i < xy->len; i++)
    cairo_line_to (cr, p[i].x, p[i].y);
  rgba (cr, accent, 1);
  cairo_set_line_width (cr, 2.5);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_stroke (cr);
  g_array_set_size (xy, 0);
}

static void
draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Charge *c = data;
  if (!c->points || c->points->len == 0 || c->now <= c->from)
    return;
  /* the text's colour (light or dark), VibeOS's accent, and the green of a battery charging */
  GdkRGBA fg, accent = { 0.243, 0.482, 0.941, 1 }, green = { 0.259, 0.690, 0.353, 1 };
  gtk_widget_get_color (GTK_WIDGET (area), &fg);

  /* the percentages to the right, as Android has them, and the times under it */
  PangoLayout *label = gtk_widget_create_pango_layout (GTK_WIDGET (area), "100%");
  int lw, lh;
  pango_layout_get_pixel_size (label, &lw, &lh);
  double top = lh / 2.0 + 1, bottom = height - lh - 10 - lh / 2.0, left = 1, right = width - lw - 10;
  if (right <= left + 10 || bottom <= top + 10) {
    g_object_unref (label);
    return;
  }
#define X(t) (left + (double) ((t) - c->from) / (double) (c->now - c->from) * (right - left))
#define Y(v) (top + (1 - CLAMP ((v), 0, 100) / 100.0) * (bottom - top))

  /* charging: green behind the line, from a point that says so to the next one (to now for the last) */
  Point *p = (Point *) c->points->data;
  guint n = c->points->len;
  for (guint i = 0; i < n; i++) {
    if (p[i].state != STATE_CHARGING)
      continue;
    gint64 end = i + 1 < n ? p[i + 1].time : c->now;
    rgba (cr, &green, 0.16);
    cairo_rectangle (cr, X (p[i].time), top, MAX (X (end) - X (p[i].time), 1), bottom - top);
    cairo_fill (cr);
  }

  /* 0, 50 and 100 % */
  static const int lines[] = { 100, 50, 0 };
  for (guint i = 0; i < G_N_ELEMENTS (lines); i++) {
    double y = round (Y (lines[i])) + 0.5;
    rgba (cr, &fg, 0.25);
    cairo_set_line_width (cr, 1);
    cairo_move_to (cr, left, y);
    cairo_line_to (cr, right, y);
    cairo_stroke (cr);
    g_autofree char *text = g_strdup_printf ("%d%%", lines[i]);
    pango_layout_set_text (label, text, -1);
    int tw, th;
    pango_layout_get_pixel_size (label, &tw, &th);
    rgba (cr, &fg, 1);
    cairo_move_to (cr, width - tw, y - th / 2.0);
    pango_cairo_show_layout (cr, label);
  }
  const char *times[] = { c->start ? c->start : "", _("Now") };
  for (guint i = 0; i < G_N_ELEMENTS (times); i++) {
    pango_layout_set_text (label, times[i], -1);
    int tw, th;
    pango_layout_get_pixel_size (label, &tw, &th);
    rgba (cr, &fg, 1);
    cairo_move_to (cr, i == 0 ? left : right - tw, height - th);
    pango_cairo_show_layout (cr, label);
  }
  g_object_unref (label);

  /* the charge: a stretch from each gap to the next; the last level holds until now */
  g_autoptr (GArray) xy = g_array_new (FALSE, FALSE, sizeof (graphene_point_t));
  for (guint i = 0; i < n; i++) {
    if (p[i].state == STATE_UNKNOWN) {
      stroke_stretch (cr, xy, bottom, &accent);
      continue;
    }
    graphene_point_t q = GRAPHENE_POINT_INIT (X (p[i].time), Y (p[i].percent));
    g_array_append_val (xy, q);
  }
  if (xy->len && p[n - 1].state != STATE_UNKNOWN) {
    graphene_point_t q = GRAPHENE_POINT_INIT (right, Y (p[n - 1].percent));
    g_array_append_val (xy, q);
  }
  stroke_stretch (cr, xy, bottom, &accent);
#undef X
#undef Y
}

/* ---- the history ---- */

static void
show (Charge *c)
{
  guint valid = 0;
  for (guint i = 0; c->points && i < c->points->len; i++)
    valid += g_array_index (c->points, Point, i).state != STATE_UNKNOWN;
  /* UPower stores a level when it changes: one point is a level held since, which the chart carries on to now (a
   * tablet that stays on its charger at 100%) */
  gboolean some = valid >= 1;
  gtk_widget_set_visible (c->area, some);
  gtk_widget_set_visible (c->empty, !some);
  if (some) {
    g_autofree char *when = when_text (c->from);
    g_free (c->start);
    /* TRANSLATORS: %s is when ("14:05", "Yesterday 14:05", "Tue 14:05") */
    c->start = c->full && c->from == c->full ? g_strdup_printf (_("Full charge %s"), when) : g_steal_pointer (&when);
  }
  if (c->full) {
    g_autofree char *ago = ago_text (c->now - c->full);
    /* TRANSLATORS: %s is how long ago ("2 h 5 min ago") */
    g_autofree char *note = g_strdup_printf (_("Last full charge: %s"), ago);
    gtk_label_set_text (GTK_LABEL (c->note), note);
  } else {
    gtk_label_set_text (GTK_LABEL (c->note), _("Not fully charged in the last 24 h"));
  }
  gtk_widget_set_visible (c->note, some || c->full);
  gtk_widget_queue_draw (c->area);
}

/* a(udu), newest first: the points since it was last full (or in the last 24 hours), oldest first */
static void
history_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GtkWidget) box = data;
  Charge *c = g_object_get_data (G_OBJECT (box), "ns-charge");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  c->loading = FALSE;
  g_autoptr (GArray) all = g_array_new (FALSE, FALSE, sizeof (Point));
  if (v) {
    g_autoptr (GVariantIter) it = NULL;
    g_variant_get (v, "(a(udu))", &it);
    guint32 time, state;
    double percent;
    while (g_variant_iter_next (it, "(udu)", &time, &percent, &state)) {
      Point q = { time, percent, state };
      g_array_append_val (all, q);
    }
  }
  c->now = g_get_real_time () / G_USEC_PER_SEC;
  c->full = 0;
  for (guint i = 0; i < all->len && !c->full; i++) {
    Point *q = &g_array_index (all, Point, i);
    if (q->state == STATE_FULL || (q->state == STATE_CHARGING && q->percent >= 99))
      c->full = q->time;
  }
  /* an hour at least, so that a phone still on its charger shows something */
  c->from = c->full ? MIN (c->full, c->now - 3600) : c->now - DAY;
  g_clear_pointer (&c->points, g_array_unref);
  c->points = g_array_new (FALSE, FALSE, sizeof (Point));
  for (guint i = all->len; i-- > 0;) {
    Point q = g_array_index (all, Point, i);
    if (q.time > c->now)
      continue;
    if (q.time < c->from) {
      /* the level when the chart starts: the newest point before it, moved to its start */
      gboolean later = i > 0 && g_array_index (all, Point, i - 1).time <= c->from;
      if (later || q.state == STATE_UNKNOWN)
        continue;
      q.time = c->from;
    }
    g_array_append_val (c->points, q);
  }
  show (c);
}

static void
read_history (Charge *c)
{
  c->loading = TRUE;
  g_dbus_connection_call (c->bus, UPOWER, c->battery, DEVICE, "GetHistory",
                          g_variant_new ("(suu)", "charge", HISTORY_SPAN, HISTORY_POINTS), G_VARIANT_TYPE ("(a(udu))"),
                          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, history_got, g_object_ref (c->box));
}

static void look_at_next (Charge *c);

/* a battery that powers the phone and keeps a history */
static void
device_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GtkWidget) box = data;
  Charge *c = g_object_get_data (G_OBJECT (box), "ns-charge");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (v) {
    g_autoptr (GVariant) props = g_variant_get_child_value (v, 0);
    guint32 type = 0;
    gboolean supply = FALSE, history = FALSE;
    g_variant_lookup (props, "Type", "u", &type);
    g_variant_lookup (props, "PowerSupply", "b", &supply);
    g_variant_lookup (props, "HasHistory", "b", &history);
    if (type == 2 && supply && history) {
      c->battery = g_strdup (c->devices[c->next - 1]);
      g_clear_pointer (&c->devices, g_strfreev);
      read_history (c);
      return;
    }
  }
  look_at_next (c);
}

static void
look_at_next (Charge *c)
{
  if (!c->devices || !c->devices[c->next]) {
    /* no battery with a history: nothing to draw */
    g_clear_pointer (&c->devices, g_strfreev);
    c->loading = FALSE;
    show (c);
    return;
  }
  g_dbus_connection_call (c->bus, UPOWER, c->devices[c->next++], "org.freedesktop.DBus.Properties", "GetAll",
                          g_variant_new ("(s)", DEVICE), G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000,
                          NULL, device_got, g_object_ref (c->box));
}

static void
devices_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GtkWidget) box = data;
  Charge *c = g_object_get_data (G_OBJECT (box), "ns-charge");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_strfreev (c->devices);
  c->devices = NULL;
  c->next = 0;
  if (v)
    g_variant_get (v, "(^ao)", &c->devices);
  look_at_next (c);
}

static void
load (Charge *c)
{
  if (c->loading || !c->bus)
    return;
  if (c->battery) {
    read_history (c);
    return;
  }
  c->loading = TRUE;
  g_dbus_connection_call (c->bus, UPOWER, "/org/freedesktop/UPower", UPOWER, "EnumerateDevices", NULL,
                          G_VARIANT_TYPE ("(ao)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, devices_got,
                          g_object_ref (c->box));
}

/* the timer holds no reference: the box's going removes it (charge_free) */
static gboolean
tick (gpointer data)
{
  load (data);
  return G_SOURCE_CONTINUE;
}

static GtkWidget *
small_label (const char *text, float xalign)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  gtk_widget_add_css_class (l, "ns-sub");
  return l;
}

GtkWidget *
ns_charge_history_new (void)
{
  Charge *c = g_new0 (Charge, 1);
  c->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  g_object_set_data_full (G_OBJECT (c->box), "ns-charge", c, (GDestroyNotify) charge_free);

  c->area = gtk_drawing_area_new ();
  gtk_widget_add_css_class (c->area, "ns-sub");
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (c->area), 220);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (c->area), draw, c, NULL);
  gtk_box_append (GTK_BOX (c->box), c->area);

  c->empty = small_label (_("Charge history appears after some use"), 0);
  gtk_label_set_wrap (GTK_LABEL (c->empty), TRUE);
  gtk_box_append (GTK_BOX (c->box), c->empty);

  c->note = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (c->note), 0);
  gtk_label_set_wrap (GTK_LABEL (c->note), TRUE);
  gtk_widget_set_margin_top (c->note, 8);
  gtk_box_append (GTK_BOX (c->box), c->note);

  gtk_widget_set_visible (c->area, FALSE);
  gtk_widget_set_visible (c->empty, FALSE);
  gtk_widget_set_visible (c->note, FALSE);

  c->bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  load (c);
  c->timer = g_timeout_add_seconds (60, tick, c);
  return c->box;
}
