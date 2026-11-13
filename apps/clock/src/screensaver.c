/* The screen saver: see screensaver.h. The motion: at the end of each minute the clock fades out and comes back
 * somewhere else. */
#include "screensaver.h"
#include "alarms-client.h"
#include "analog-clock.h"
#include "clock-format.h"

#include <glib/gi18n.h>
#include <neo.h>

#define FADE_MS       3000      /* each fade, out or in */
#define START_DELAY   5         /* ms before every fade-in */
#define SHRUNK        0.85      /* the scale it fades out to, and back in from */
#define TIME_SIZE     80
#define ANALOG_SIZE   294       /* the dial's own size */

typedef enum {
  FIRST_IN,                     /* the first fade-in, where it opened */
  OUT,                          /* fading and shrinking away, the last 3 s of a minute */
  IN,                           /* back, growing, somewhere else */
} Phase;

#define SAVER_TYPE_VIEW (saver_view_get_type ())
G_DECLARE_FINAL_TYPE (SaverView, saver_view, SAVER, VIEW, GtkWidget)

struct _SaverView
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *block, *time, *analog, *date, *alarm_icon, *alarm;
  double x, y;                  /* the block's place in the window */
  double alpha, scale;
  float filter[4];              /* the dimming: what red, green, blue and alpha are multiplied by */
  NeoAnim *anim;
  Phase phase;
  guint start, move, tick;      /* the first placing; the minute's move (3 s before it) and its new time (at it) */
};

G_DEFINE_FINAL_TYPE (SaverView, saver_view, GTK_TYPE_WIDGET)

/* --- what it shows */

/* the time. 24-hour: "Hm", as it is. 12-hour: "hma" with a hair space before the AM/PM, which is 0.4 of the size in
 * plain sans-serif (Roboto, the app's own face), or left out ("Show AM/PM indicator"); "Bold clock text" makes all of
 * it bold. The weights are the ones with the phone's bold text on, as everywhere in the app: the thin face's 100
 * becomes 400, the AM/PM's 400 becomes 700, and bold is as bold as Roboto goes. */
static char *
time_markup (SaverView *self, GDateTime *now)
{
  int hour = g_date_time_get_hour (now), minute = g_date_time_get_minute (now);
  gboolean bold = g_settings_get_boolean (self->settings, "screensaver-bold-text");
  g_autofree char *ampm = NULL;

  if (neo_clock_uses_24h ())
    return g_strdup_printf ("%02d:%02d", hour, minute);
  if (g_settings_get_boolean (self->settings, "screensaver-show-ampm"))
    ampm = g_strdup_printf ("\u200A<span size='40%%' weight='%d'>%s</span>",
                            bold ? 900 : 700, hour < 12 ? "AM" : "PM");
  return g_strdup_printf ("<span weight='%d'>%d:%02d%s</span>", bold ? 900 : 400, hour % 12 ? hour % 12 : 12,
                          minute, ampm ? ampm : "");
}

/* the time, then the date and the next alarm as the Clock tab writes them */
static void
show_time (SaverView *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *time = time_markup (self, now);
  g_autofree char *date = g_date_time_format (now, "%a, %b %-d");
  g_autofree char *upper = g_utf8_strup (date, -1);
  gint64 next = alarms_client_get_next_alarm (alarms_client_get ());

  gtk_label_set_markup (GTK_LABEL (self->time), time);
  gtk_label_set_text (GTK_LABEL (self->date), upper);
  gtk_widget_set_visible (self->alarm_icon, next > 0);
  gtk_widget_set_visible (self->alarm, next > 0);
  if (next > 0)
    {
      g_autoptr(GDateTime) when = g_date_time_new_from_unix_local (next);
      g_autofree char *text = clock_alarm_time_text (when, neo_clock_uses_24h ());
      g_autofree char *alarm = g_utf8_strup (text, -1);

      gtk_label_set_text (GTK_LABEL (self->alarm), alarm);
    }
}

/* GNOME's Do Not Disturb: notification banners off. FALSE where that setting does not exist */
static gboolean
dnd_on (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source,
                                                                                "org.gnome.desktop.notifications",
                                                                                TRUE) : NULL;
  g_autoptr(GSettings) notifications = NULL;

  if (!schema || !g_settings_schema_has_key (schema, "show-banners"))
    return FALSE;
  notifications = g_settings_new_full (schema, NULL, NULL);
  return !g_settings_get_boolean (notifications, "show-banners");
}

/* the clock, drawn in white, is multiplied by a colour. Dimmed (night mode): the night colour at alpha 16 + 176 ×
 * brightness / 100, so 40 % is 0x56. Not dimmed: the clock colour at 0xC0. With "Follow DND state" on, Do Not Disturb
 * alone decides, night mode or not. The Preview starts dimmed, but every fade-in applies this again, the first one 5 ms
 * in. */
static void
dim (SaverView *self)
{
  gboolean dimmed = g_settings_get_boolean (self->settings, "screensaver-night-mode");
  int brightness = g_settings_get_int (self->settings, "screensaver-night-mode-brightness");
  guint rgb;
  int alpha;

  if (g_settings_get_boolean (self->settings, "screensaver-night-mode-dnd"))
    dimmed = dnd_on ();
  rgb = g_settings_get_enum (self->settings, dimmed ? "screensaver-night-mode-color" : "screensaver-clock-color");
  alpha = dimmed ? 16 + 176 * brightness / 100 : 0xC0;
  self->filter[0] = (rgb >> 16 & 0xff) / 255.f;
  self->filter[1] = (rgb >> 8 & 0xff) / 255.f;
  self->filter[2] = (rgb & 0xff) / 255.f;
  self->filter[3] = alpha / 255.f;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- the motion */

/* a whole number from 0 up to @maximum, not including it */
static double
random_point (double maximum)
{
  return (int) (g_random_double () * maximum);
}

static void
block_size (SaverView *self, int *width, int *height)
{
  GtkRequisition size;

  gtk_widget_get_preferred_size (self->block, NULL, &size);
  *width = size.width;
  *height = size.height;
}

/* milliseconds until @offset ms from the next minute, never 0 */
static guint
until_minute (int offset)
{
  gint64 now = g_get_real_time () / 1000;
  gint64 at = (now / 60000 + 1) * 60000 + offset;

  while (at <= now)
    at += 60000;
  return at - now;
}

static gboolean on_move (gpointer data);

static void
fade_in_elsewhere (SaverView *self)
{
  int width, height;

  /* anywhere it fits in the window. It is picked as the fade-in starts, with the new time's width, so that a wider time
   * ("10:00" after "9:59") cannot run off the edge */
  block_size (self, &width, &height);
  self->x = random_point (gtk_widget_get_width (GTK_WIDGET (self)) - width);
  self->y = random_point (gtk_widget_get_height (GTK_WIDGET (self)) - height);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  dim (self);
  self->phase = IN;
  neo_anim_start (self->anim, 0, 1, FADE_MS, START_DELAY, SPONGY_DECELERATE);
}

static gboolean
fade_in_later (gpointer data)
{
  SaverView *self = data;

  /* unless the window closed meanwhile */
  if (self->anim)
    fade_in_elsewhere (self);
  return G_SOURCE_REMOVE;
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  SaverView *self = data;

  switch (self->phase)
    {
    case FIRST_IN:
      self->alpha = value;
      self->scale = 1;
      break;
    case OUT:
      self->alpha = 1 - value;
      self->scale = 1 - (1 - SHRUNK) * value;
      break;
    case IN:
      self->alpha = value;
      self->scale = SHRUNK + (1 - SHRUNK) * value;
      break;
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
  /* gone: back somewhere else, from the main loop rather than from inside the animation's own last call */
  if (!neo_anim_is_running (anim) && self->phase == OUT)
    g_idle_add_full (G_PRIORITY_DEFAULT, fade_in_later, g_object_ref (self), g_object_unref);
}

/* 3 s before each minute: fade out and shrink, speeding up, so that the new time comes in with the clock's new place */
static gboolean
on_move (gpointer data)
{
  SaverView *self = data;

  self->phase = OUT;
  neo_anim_start (self->anim, 0, 1, FADE_MS, 0, SPONGY_ACCELERATE);
  self->move = g_timeout_add (until_minute (-FADE_MS), on_move, self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_tick (gpointer data)
{
  SaverView *self = data;

  show_time (self);
  self->tick = g_timeout_add (until_minute (0), on_tick, self);
  return G_SOURCE_REMOVE;
}

/* the first place: inside the largest square the window holds, whichever way the phone is turned, then a fade-in that
 * slows down; after that, a move every minute */
static gboolean
on_start (gpointer data)
{
  SaverView *self = data;
  int width, height, smallest = MIN (gtk_widget_get_width (GTK_WIDGET (self)),
                                     gtk_widget_get_height (GTK_WIDGET (self)));

  self->start = 0;
  block_size (self, &width, &height);
  self->x = random_point (smallest - width);
  self->y = random_point (smallest - height);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  dim (self);
  self->phase = FIRST_IN;
  neo_anim_start (self->anim, 0, 1, FADE_MS, START_DELAY, SPONGY_DECELERATE);
  self->move = g_timeout_add (until_minute (-FADE_MS), on_move, self);
  self->tick = g_timeout_add (until_minute (0), on_tick, self);
  return G_SOURCE_REMOVE;
}

/* --- the widget: the block at its own size, wherever it was put */

static void
saver_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  SaverView *self = SAVER_VIEW (widget);
  int block_width, block_height;

  block_size (self, &block_width, &block_height);
  gtk_widget_allocate (self->block, block_width, block_height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (self->x, self->y)));
  /* placed once the window has its size */
  if (!self->anim && width > 0 && height > 0)
    {
      self->anim = neo_anim_new (widget, on_anim, self);
      self->start = g_idle_add (on_start, self);
    }
}

/* the block faded, shrunk about its centre, and multiplied by the dimming colour (the hardware layer's MULTIPLY) */
static void
saver_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SaverView *self = SAVER_VIEW (widget);
  float cx = self->x + gtk_widget_get_width (self->block) / 2.0;
  float cy = self->y + gtk_widget_get_height (self->block) / 2.0;
  graphene_matrix_t matrix;

  if (self->alpha <= 0)
    return;
  graphene_matrix_init_from_float (&matrix, (float[16]) { self->filter[0], 0, 0, 0,
                                                          0, self->filter[1], 0, 0,
                                                          0, 0, self->filter[2], 0,
                                                          0, 0, 0, self->filter[3] });
  gtk_snapshot_push_opacity (snapshot, self->alpha);
  gtk_snapshot_push_color_matrix (snapshot, &matrix, graphene_vec4_zero ());
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy));
  gtk_snapshot_scale (snapshot, self->scale, self->scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-cx, -cy));
  gtk_widget_snapshot_child (widget, self->block, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
saver_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                    int *min_baseline, int *nat_baseline)
{
  /* the window decides: the block floats in whatever it is given */
  *min = *nat = 0;
}

static void
saver_view_dispose (GObject *object)
{
  SaverView *self = SAVER_VIEW (object);

  g_clear_handle_id (&self->start, g_source_remove);
  g_clear_handle_id (&self->move, g_source_remove);
  g_clear_handle_id (&self->tick, g_source_remove);
  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->block, gtk_widget_unparent);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (saver_view_parent_class)->dispose (object);
}

static void
saver_view_class_init (SaverViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = saver_view_dispose;
  widget_class->size_allocate = saver_view_size_allocate;
  widget_class->snapshot = saver_view_snapshot;
  widget_class->measure = saver_view_measure;
  gtk_widget_class_set_css_name (widget_class, "screensaver");
}

static void
saver_view_init (SaverView *self)
{
}

/* the analog clock (no second hand) or the digital time, then the date and the next alarm, which keep to the start */
static void
build (SaverView *self)
{
  gboolean analog = g_settings_get_enum (self->settings, "screensaver-clock-style") == 0;
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  self->block = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->block, GTK_WIDGET (self));

  /* the analog dial, with a gap under it before the date */
  self->analog = analog_clock_new (ANALOG_SIZE);
  analog_clock_set_seconds (CLOCK_ANALOG_CLOCK (self->analog), FALSE);
  gtk_widget_set_size_request (self->analog, ANALOG_SIZE, -1);
  gtk_widget_set_halign (self->analog, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (self->analog, 14);
  gtk_widget_set_visible (self->analog, analog);
  gtk_box_append (GTK_BOX (self->block), self->analog);

  /* the digital time, centred; a negative margin pulls the date up under it */
  self->time = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->time, "saver-time");
  gtk_widget_set_halign (self->time, GTK_ALIGN_CENTER);
  neo_label_set_font_padding (self->time, TIME_SIZE);
  gtk_widget_set_margin_bottom (self->time, gtk_widget_get_margin_bottom (self->time) - 8);
  gtk_widget_set_visible (self->time, !analog);
  gtk_box_append (GTK_BOX (self->block), self->time);

  gtk_widget_set_halign (line, GTK_ALIGN_START);
  self->date = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->date, "body");
  neo_label_set_font_padding (self->date, 14);
  gtk_box_append (GTK_BOX (line), self->date);
  self->alarm_icon = gtk_image_new_from_icon_name ("clock-alarm-small-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->alarm_icon), 14);
  gtk_widget_set_margin_start (self->alarm_icon, 6);
  gtk_widget_set_margin_end (self->alarm_icon, 6);
  gtk_widget_add_css_class (self->alarm_icon, "next-alarm");
  gtk_box_append (GTK_BOX (line), self->alarm_icon);
  self->alarm = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->alarm, "body");
  neo_label_set_font_padding (self->alarm, 14);
  gtk_box_append (GTK_BOX (line), self->alarm);
  gtk_box_append (GTK_BOX (self->block), line);
  show_time (self);
  /* the next alarm, whenever it changes */
  g_signal_connect_object (alarms_client_get (), "next-alarm-changed", G_CALLBACK (show_time), self,
                           G_CONNECT_SWAPPED);
}

/* --- the window */

/* UPower's OnBattery; TRUE when that cannot be known, so that the screen is never kept on by mistake */
static gboolean
on_battery (void)
{
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  g_autoptr(GVariant) reply = NULL, value = NULL;

  if (bus)
    reply = g_dbus_connection_call_sync (bus, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                                         "org.freedesktop.DBus.Properties", "Get",
                                         g_variant_new ("(ss)", "org.freedesktop.UPower", "OnBattery"),
                                         G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!reply)
    return TRUE;
  g_variant_get (reply, "(v)", &value);
  return !g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) || g_variant_get_boolean (value);
}

static void
on_destroy (GtkWidget *window, gpointer data)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (window));
  guint cookie = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (window), "inhibit"));

  if (app && cookie)
    gtk_application_uninhibit (app, cookie);
}

/* any touch or key ends it */
static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, GtkWindow *window)
{
  gtk_window_destroy (window);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, GtkWindow *window)
{
  gtk_window_destroy (window);
  return TRUE;
}

void
screensaver_present (GtkApplication *app, GSettings *settings)
{
  SaverView *view = g_object_new (SAVER_TYPE_VIEW, NULL);
  GtkWidget *window = neo_window_new (app);
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  view->settings = g_object_ref (settings);
  build (view);
  gtk_window_set_title (GTK_WINDOW (window), _("Screen saver"));
  gtk_widget_add_css_class (window, "clock-screensaver");
  neo_window_set_content (GTK_WINDOW (window), GTK_WIDGET (view));

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), window);
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), window);
  gtk_widget_add_controller (window, keys);

  /* the screen kept on while on a charger; on battery the screen times out as it always does */
  if (!on_battery ())
    g_object_set_data (G_OBJECT (window), "inhibit",
                       GUINT_TO_POINTER (gtk_application_inhibit (app, GTK_WINDOW (window),
                                                                  GTK_APPLICATION_INHIBIT_IDLE, _("Screen saver"))));
  g_signal_connect (window, "destroy", G_CALLBACK (on_destroy), NULL);
  /* immersive: no status bar, no navigation */
  gtk_window_fullscreen (GTK_WINDOW (window));
  gtk_window_present (GTK_WINDOW (window));
}
