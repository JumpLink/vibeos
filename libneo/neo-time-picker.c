/* NeoTimePicker: see neo-time-picker.h. One widget draws the header and the dial; the dial cross-fades between hours
 * and minutes. */
#include "neo-time-picker.h"
#include "neo-anim.h"
#include "neo-dialog.h"
#include "neo-style.h"

#include <math.h>

#define HEADER_H        100
#define TIME_SIZE       60      /* the header's hours and minutes */
#define AMPM_SIZE       16
#define AMPM_PADDING    16
#define SEPARATOR_PAD   2       /* either side of the ':' */
#define DIAL            296
#define DIAL_TOP        16
#define DIAL_SIDE       16
#define SELECTOR_R      20
#define SELECTOR_STROKE 2       /* the line from the centre to the selector */
#define CENTER_DOT_R    3
#define SELECTOR_DOT_R  2       /* in the selector on a minute between the numbers */
#define INSET_OUTER     22      /* the outer ring of numbers, in from the dial's edge */
#define INSET_INNER     58      /* the inner ring (24-hour mode) */
#define TEXT_OUTER      16      /* the outer ring's numbers */
#define TEXT_INNER      12      /* the inner ring's */
#define FADE_MS         500     /* the hours <-> minutes cross-fade */

#define WIDTH  (DIAL + 2 * DIAL_SIDE)
#define HEIGHT (HEADER_H + DIAL_TOP + DIAL)

enum { HOURS, MINUTES };

struct _NeoTimePicker {
  GtkWidget    parent;
  int          hour, minute;           /* 0..23, 0..59 */
  gboolean     use_24h;
  int          showing;                /* HOURS or MINUTES */
  double       to_minutes;             /* the cross-fade: 0 hours .. 1 minutes */
  NeoAnim     *fade;
  gboolean     dragging;               /* a touch that began on the dial */

  /* the ring numbers: [0] 12 1 2 .. 11, [1] 00 13 .. 23 (24-hour), [2] 00 05 .. 55 */
  PangoLayout *rings[3][12];
  PangoLayout *time_hours, *time_minutes, *separator, *am, *pm;
  graphene_rect_t hours_box, minutes_box, am_box, pm_box;
};

G_DEFINE_FINAL_TYPE (NeoTimePicker, neo_time_picker, GTK_TYPE_WIDGET)

gboolean
neo_clock_uses_24h (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                              "org.gnome.desktop.interface", TRUE);
  if (!schema)
    return TRUE;
  g_settings_schema_unref (schema);
  GSettings *settings = g_settings_new ("org.gnome.desktop.interface");
  char *format = g_settings_get_string (settings, "clock-format");
  gboolean is_24h = g_strcmp0 (format, "12h") != 0;
  g_free (format);
  g_object_unref (settings);
  return is_24h;
}

/* ---- text ------------------------------------------------------------------------------------------------------ */

static PangoLayout *
make_layout (NeoTimePicker *self, const char *text, int size)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), text);
  PangoFontDescription *font = pango_font_description_from_string ("Neo Roboto");
  pango_font_description_set_weight (font, 700);
  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  return layout;
}

static void
update_header (NeoTimePicker *self)
{
  int shown_hour = self->use_24h ? self->hour : (self->hour % 12 ? self->hour % 12 : 12);
  char text[8];
  /* the locale's pattern decides the leading zero: "HH:mm" has it, "h:mm a" does not */
  g_snprintf (text, sizeof text, self->use_24h ? "%02d" : "%d", shown_hour);
  pango_layout_set_text (self->time_hours, text, -1);
  g_snprintf (text, sizeof text, "%02d", self->minute);
  pango_layout_set_text (self->time_minutes, text, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* draws `layout` with its logical box centred on (x, y) */
static void
draw_centred (GtkSnapshot *snapshot, PangoLayout *layout, double x, double y, const GdkRGBA *color)
{
  PangoRectangle logical;
  pango_layout_get_pixel_extents (layout, NULL, &logical);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (round (x - logical.width / 2.0), round (y - logical.height / 2.0)));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}

/* ---- geometry -------------------------------------------------------------------------------------------------- */

static void
dial_centre (NeoTimePicker *self, double *cx, double *cy)
{
  *cx = gtk_widget_get_width (GTK_WIDGET (self)) / 2.0;
  *cy = HEADER_H + DIAL_TOP + DIAL / 2.0;
}

/* the selector of `which`: its angle (degrees clockwise from 12) and its distance from the centre */
static void
selector_of (NeoTimePicker *self, int which, double *degrees, double *distance)
{
  if (which == HOURS) {
    *degrees = (self->hour % 12) * 30;
    gboolean inner = self->use_24h && (self->hour == 0 || self->hour > 12);
    *distance = DIAL / 2.0 - (inner ? INSET_INNER : INSET_OUTER);
  } else {
    *degrees = self->minute * 6;
    *distance = DIAL / 2.0 - INSET_OUTER;
  }
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

static void
draw_ring (NeoTimePicker *self, GtkSnapshot *snapshot, int ring, double radius, const GdkRGBA *color)
{
  double cx, cy;
  dial_centre (self, &cx, &cy);
  for (int i = 0; i < 12; i++) {
    double a = i * G_PI / 6;
    draw_centred (snapshot, self->rings[ring][i], cx + radius * sin (a), cy - radius * cos (a), color);
  }
}

static void
draw_numbers (NeoTimePicker *self, GtkSnapshot *snapshot, int which, gboolean activated, float alpha)
{
  GdkRGBA outer = activated ? neo_color_alpha (NEO_SURFACE, alpha) : neo_color_alpha (NEO_ON_SURFACE, alpha);
  GdkRGBA inner = activated ? neo_color_alpha (NEO_SURFACE, alpha) : neo_color_alpha (NEO_ON_SURFACE_VARIANT, alpha);
  if (which == HOURS) {
    draw_ring (self, snapshot, 0, DIAL / 2.0 - INSET_OUTER, &outer);
    if (self->use_24h)
      draw_ring (self, snapshot, 1, DIAL / 2.0 - INSET_INNER, &inner);
  } else {
    draw_ring (self, snapshot, 2, DIAL / 2.0 - INSET_OUTER, &outer);
  }
}

static void
draw_dial (NeoTimePicker *self, GtkSnapshot *snapshot, int which, float alpha)
{
  if (alpha <= 0)
    return;
  double cx, cy, degrees, distance;
  dial_centre (self, &cx, &cy);
  selector_of (self, which, &degrees, &distance);
  double a = degrees * G_PI / 180;
  double sx = cx + distance * sin (a), sy = cy - distance * cos (a);
  GdkRGBA selector = neo_color_alpha (NEO_SECONDARY, alpha);

  /* the numbers, then the selector over them, then the numbers again inside the selector in the dark colour
   * (clipped to the selector), so a number half under it is drawn half in each colour */
  draw_numbers (self, snapshot, which, FALSE, alpha);

  GskRoundedRect circle;
  gsk_rounded_rect_init_from_rect (&circle, &GRAPHENE_RECT_INIT (sx - SELECTOR_R, sy - SELECTOR_R,
                                                                 2 * SELECTOR_R, 2 * SELECTOR_R), SELECTOR_R);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
  gtk_snapshot_append_color (snapshot, &selector, &circle.bounds);
  draw_numbers (self, snapshot, which, TRUE, alpha);
  /* between two numbers (a minute that is not a multiple of five) a dot marks the selector's middle */
  if (which == MINUTES && self->minute % 5) {
    GdkRGBA dot = neo_color_alpha (NEO_SURFACE, alpha);
    GskRoundedRect d;
    gsk_rounded_rect_init_from_rect (&d, &GRAPHENE_RECT_INIT (sx - SELECTOR_DOT_R, sy - SELECTOR_DOT_R,
                                                              2 * SELECTOR_DOT_R, 2 * SELECTOR_DOT_R), SELECTOR_DOT_R);
    gtk_snapshot_push_rounded_clip (snapshot, &d);
    gtk_snapshot_append_color (snapshot, &dot, &d.bounds);
    gtk_snapshot_pop (snapshot);
  }
  gtk_snapshot_pop (snapshot);

  /* the hand: from the centre to the selector's near edge */
  GskPathBuilder *builder = gsk_path_builder_new ();
  gsk_path_builder_move_to (builder, cx, cy);
  gsk_path_builder_line_to (builder, cx + (distance - SELECTOR_R) * sin (a), cy - (distance - SELECTOR_R) * cos (a));
  GskPath *line = gsk_path_builder_free_to_path (builder);
  GskStroke *stroke = gsk_stroke_new (SELECTOR_STROKE);
  gtk_snapshot_append_stroke (snapshot, line, stroke, &selector);
  gsk_stroke_free (stroke);
  gsk_path_unref (line);
}

static void
draw_header (NeoTimePicker *self, GtkSnapshot *snapshot, double width)
{
  GdkRGBA header = neo_color (NEO_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_append_color (snapshot, &header, &GRAPHENE_RECT_INIT (0, 0, width, HEADER_H));

  /* the header's text colour is "primary when activated, secondary otherwise" */
  GdkRGBA on = neo_color (NEO_ON_SURFACE), off = neo_color (NEO_ON_SURFACE_VARIANT);
  PangoRectangle h, m, s;
  pango_layout_get_pixel_extents (self->time_hours, NULL, &h);
  pango_layout_get_pixel_extents (self->time_minutes, NULL, &m);
  pango_layout_get_pixel_extents (self->separator, NULL, &s);
  double sx = (width - s.width) / 2.0, y = (HEADER_H - h.height) / 2.0;
  self->hours_box = GRAPHENE_RECT_INIT (sx - SEPARATOR_PAD - h.width, y, h.width, h.height);
  self->minutes_box = GRAPHENE_RECT_INIT (sx + s.width + SEPARATOR_PAD, y, m.width, m.height);

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &self->hours_box.origin);
  gtk_snapshot_append_layout (snapshot, self->time_hours, self->showing == HOURS ? &on : &off);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (sx, y));
  gtk_snapshot_append_layout (snapshot, self->separator, &off);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &self->minutes_box.origin);
  gtk_snapshot_append_layout (snapshot, self->time_minutes, self->showing == MINUTES ? &on : &off);
  gtk_snapshot_restore (snapshot);

  if (self->use_24h)
    return;
  /* AM over PM, after the minutes */
  PangoRectangle a, p;
  pango_layout_get_pixel_extents (self->am, NULL, &a);
  pango_layout_get_pixel_extents (self->pm, NULL, &p);
  double ax = graphene_rect_get_x (&self->minutes_box) + m.width + AMPM_PADDING;
  double top = HEADER_H / 2.0 - a.height;
  self->am_box = GRAPHENE_RECT_INIT (ax, top, MAX (a.width, p.width), a.height);
  self->pm_box = GRAPHENE_RECT_INIT (ax, top + a.height, MAX (a.width, p.width), p.height);
  gboolean pm = self->hour >= 12;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &self->am_box.origin);
  gtk_snapshot_append_layout (snapshot, self->am, pm ? &off : &on);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &self->pm_box.origin);
  gtk_snapshot_append_layout (snapshot, self->pm, pm ? &on : &off);
  gtk_snapshot_restore (snapshot);
}

static void
neo_time_picker_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NeoTimePicker *self = NEO_TIME_PICKER (widget);
  double width = gtk_widget_get_width (widget);
  GdkRGBA surface = neo_color (NEO_SURFACE);
  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, width, gtk_widget_get_height (widget)));
  draw_header (self, snapshot, width);

  double cx, cy;
  dial_centre (self, &cx, &cy);
  GskRoundedRect disc;
  gsk_rounded_rect_init_from_rect (&disc, &GRAPHENE_RECT_INIT (cx - DIAL / 2.0, cy - DIAL / 2.0, DIAL, DIAL), DIAL / 2.0);
  GdkRGBA background = neo_color (NEO_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_push_rounded_clip (snapshot, &disc);
  gtk_snapshot_append_color (snapshot, &background, &disc.bounds);
  gtk_snapshot_pop (snapshot);

  draw_dial (self, snapshot, HOURS, 1 - self->to_minutes);
  draw_dial (self, snapshot, MINUTES, self->to_minutes);

  GskRoundedRect dot;
  GdkRGBA selector = neo_color (NEO_SECONDARY);
  gsk_rounded_rect_init_from_rect (&dot, &GRAPHENE_RECT_INIT (cx - CENTER_DOT_R, cy - CENTER_DOT_R,
                                                              2 * CENTER_DOT_R, 2 * CENTER_DOT_R), CENTER_DOT_R);
  gtk_snapshot_push_rounded_clip (snapshot, &dot);
  gtk_snapshot_append_color (snapshot, &selector, &dot.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
neo_time_picker_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                         int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? WIDTH : HEIGHT;
}

/* ---- input ----------------------------------------------------------------------------------------------------- */

static void
on_fade (NeoAnim *anim, double value, gpointer data)
{
  NeoTimePicker *self = data;
  self->to_minutes = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
show_dial (NeoTimePicker *self, int which)
{
  if (self->showing == which)
    return;
  self->showing = which;
  /* accelerate-decelerate */
  neo_anim_start (self->fade, self->to_minutes, which == MINUTES, FADE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

/* The value under (x, y), snapped to the nearest hour or minute, or FALSE when a
 * touch-down lands off the numbers (force: a drag already under way takes any point) */
static gboolean
pick (NeoTimePicker *self, double x, double y, gboolean force)
{
  double cx, cy;
  dial_centre (self, &cx, &cy);
  double dx = x - cx, dy = y - cy, distance = hypot (dx, dy);
  double outer = DIAL / 2.0 - INSET_OUTER, inner = DIAL / 2.0 - INSET_INNER;
  gboolean on_inner = FALSE;
  if (self->showing == HOURS && self->use_24h) {
    if (!force && (distance < inner - SELECTOR_R || distance > outer + SELECTOR_R))
      return FALSE;
    on_inner = distance <= (inner + outer) / 2;
  } else if (!force && (distance < outer - SELECTOR_R || distance > outer + SELECTOR_R)) {
    return FALSE;
  }

  double degrees = atan2 (dx, -dy) * 180 / G_PI;
  if (degrees < 0)
    degrees += 360;
  if (self->showing == HOURS) {
    int step = ((int) round (degrees / 30)) % 12;
    int hour;
    if (self->use_24h)
      hour = on_inner ? (step ? step + 12 : 0) : (step ? step : 12);
    else
      hour = (step ? step : 12) % 12 + (self->hour >= 12 ? 12 : 0);
    self->hour = hour;
  } else {
    self->minute = ((int) round (degrees / 6)) % 60;
  }
  update_header (self);
  return TRUE;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, NeoTimePicker *self)
{
  graphene_point_t p = GRAPHENE_POINT_INIT (x, y);
  self->dragging = FALSE;
  if (graphene_rect_contains_point (&self->hours_box, &p)) {
    show_dial (self, HOURS);
  } else if (graphene_rect_contains_point (&self->minutes_box, &p)) {
    show_dial (self, MINUTES);
  } else if (!self->use_24h && graphene_rect_contains_point (&self->am_box, &p)) {
    if (self->hour >= 12)
      self->hour -= 12;
    update_header (self);
  } else if (!self->use_24h && graphene_rect_contains_point (&self->pm_box, &p)) {
    if (self->hour < 12)
      self->hour += 12;
    update_header (self);
  } else if (y > HEADER_H && pick (self, x, y, FALSE)) {
    /* the dial has the touch: the dialog's scrim and anything else keep out of it */
    self->dragging = TRUE;
    neo_anim_finish (self->fade);
  }
}

static void
on_drag (GtkGestureDrag *drag, double dx, double dy, NeoTimePicker *self)
{
  double x, y;
  if (!self->dragging || !gtk_gesture_drag_get_start_point (drag, &x, &y))
    return;
  pick (self, x + dx, y + dy, TRUE);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, NeoTimePicker *self)
{
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  /* an hour picked by touch moves on to the minutes */
  if (self->showing == HOURS)
    show_dial (self, MINUTES);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
neo_time_picker_dispose (GObject *object)
{
  NeoTimePicker *self = NEO_TIME_PICKER (object);
  g_clear_pointer (&self->fade, neo_anim_free);
  for (int r = 0; r < 3; r++)
    for (int i = 0; i < 12; i++)
      g_clear_object (&self->rings[r][i]);
  g_clear_object (&self->time_hours);
  g_clear_object (&self->time_minutes);
  g_clear_object (&self->separator);
  g_clear_object (&self->am);
  g_clear_object (&self->pm);
  G_OBJECT_CLASS (neo_time_picker_parent_class)->dispose (object);
}

static void
neo_time_picker_class_init (NeoTimePickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = neo_time_picker_dispose;
  widget_class->snapshot = neo_time_picker_snapshot;
  widget_class->measure = neo_time_picker_measure;
  gtk_widget_class_set_css_name (widget_class, "neo-time-picker");
}

static void
neo_time_picker_init (NeoTimePicker *self)
{
  self->fade = neo_anim_new (GTK_WIDGET (self), on_fade, self);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
neo_time_picker_new (int hour, int minute, gboolean use_24h)
{
  NeoTimePicker *self = g_object_new (NEO_TYPE_TIME_PICKER, NULL);
  self->hour = CLAMP (hour, 0, 23);
  self->minute = CLAMP (minute, 0, 59);
  self->use_24h = use_24h;

  /* the numbers on the dial: hours 12, 1-11; the 24-hour inner ring 00, 13-23; minutes 00, 05, ... 55 */
  for (int i = 0; i < 12; i++) {
    char text[4];
    g_snprintf (text, sizeof text, "%d", i ? i : 12);
    self->rings[0][i] = make_layout (self, text, TEXT_OUTER);
    g_snprintf (text, sizeof text, "%02d", i ? i + 12 : 0);
    self->rings[1][i] = make_layout (self, text, TEXT_INNER);
    g_snprintf (text, sizeof text, "%02d", i * 5);
    self->rings[2][i] = make_layout (self, text, TEXT_OUTER);
  }
  self->time_hours = make_layout (self, "", TIME_SIZE);
  self->time_minutes = make_layout (self, "", TIME_SIZE);
  self->separator = make_layout (self, ":", TIME_SIZE);
  self->am = make_layout (self, "AM", AMPM_SIZE);
  self->pm = make_layout (self, "PM", AMPM_SIZE);
  update_header (self);
  return GTK_WIDGET (self);
}

void
neo_time_picker_get_time (NeoTimePicker *self, int *hour, int *minute)
{
  *hour = self->hour;
  *minute = self->minute;
}

/* ---- the dialog ------------------------------------------------------------------------------------------------ */

typedef struct {
  NeoTimeChosen  chosen;
  gpointer       data;
  GDestroyNotify destroy;
  NeoTimePicker *picker;
} Pending;

static void
pending_free (gpointer data, GClosure *closure)
{
  Pending *pending = data;
  if (pending->destroy)
    pending->destroy (pending->data);
  g_free (pending);
}

static void
on_response (NeoDialog *dialog, int response, Pending *pending)
{
  if (response == NEO_RESPONSE_OK) {
    int hour, minute;
    neo_time_picker_get_time (pending->picker, &hour, &minute);
    pending->chosen (hour, minute, pending->data);
  }
}

void
neo_time_picker_dialog (GtkWidget *parent, int hour, int minute, NeoTimeChosen chosen, gpointer data,
                        GDestroyNotify destroy)
{
  NeoDialog *dialog = neo_dialog_new (NULL);
  /* the picker reaches the card's edges, on the plain surface colour */
  neo_dialog_set_flush (dialog, TRUE);
  GtkWidget *picker = neo_time_picker_new (hour, minute, neo_clock_uses_24h ());
  neo_dialog_set_content (dialog, picker);
  neo_dialog_add_button (dialog, "Cancel", NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, "OK", NEO_RESPONSE_OK);
  Pending *pending = g_new (Pending, 1);
  *pending = (Pending) { chosen, data, destroy, NEO_TIME_PICKER (picker) };
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), pending, pending_free, 0);
  neo_dialog_present (dialog, parent);
}
