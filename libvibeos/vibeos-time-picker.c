/* VibeosTimePicker: see vibeos-time-picker.h. One widget draws the header and the dial; the dial cross-fades between hours
 * and minutes. */
#include "vibeos-time-picker.h"
#include "vibeos-anim.h"
#include "vibeos-dialog.h"
#include "vibeos-layout.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"
#include "vibeos-text.h"

#include <math.h>

#define HEADER_H        100
#define TIME_SIZE       60      /* the header's hours and minutes, in sp */
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
#define TEXT_OUTER      16      /* the outer ring's numbers, in sp */
#define TEXT_INNER      12      /* the inner ring's */
#define FADE_MS         500     /* the hours <-> minutes cross-fade */
/* sideways (a window wider than tall), as Android's landscape picker: the time in a column at the start, the picker's
 * full height, and a smaller dial beside it */
#define HEADER_W        168
#define DIAL_SIDEWAYS   224
#define DIAL_GAP        24      /* either side of the sideways dial */
#define AMPM_GAP        8       /* sideways: AM and PM under the time, and between them */
/* the dialog's Cancel and OK, in the picker's corner under the dial */
#define BUTTONS_GAP     8
#define BUTTONS_TOP     4
#define BUTTONS_END     12
#define BUTTONS_BOTTOM  8

enum { HOURS, MINUTES };

struct _VibeosTimePicker {
  GtkWidget    parent;
  int          hour, minute;           /* 0..23, 0..59 */
  gboolean     use_24h;
  int          showing;                /* HOURS or MINUTES */
  double       to_minutes;             /* the cross-fade: 0 hours .. 1 minutes */
  VibeosAnim     *fade;
  gboolean     dragging;               /* a touch that began on the dial */
  gboolean     sideways;               /* the time in a column at the start, the dial beside it */
  int          dial;                   /* the dial's diameter: DIAL, or DIAL_SIDEWAYS */
  GtkWidget   *buttons;                /* the dialog's Cancel and OK (the dialog's picker only) */

  /* the ring numbers: [0] 12 1 2 .. 11, [1] 00 13 .. 23 (24-hour), [2] 00 05 .. 55 */
  PangoLayout *rings[3][12];
  PangoLayout *time_hours, *time_minutes, *separator, *am, *pm;
  graphene_rect_t hours_box, minutes_box, am_box, pm_box;
};

G_DEFINE_FINAL_TYPE (VibeosTimePicker, vibeos_time_picker, GTK_TYPE_WIDGET)

gboolean
vibeos_clock_uses_24h (void)
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

/* the font of a layout whose text is `sp`, at the font size and weight in effect */
static void
set_font (PangoLayout *layout, int sp)
{
  PangoFontDescription *font = pango_font_description_from_string ("VibeOS Roboto");
  pango_font_description_set_weight (font, vibeos_font_weight (400));
  pango_font_description_set_absolute_size (font, vibeos_sp (sp) * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  g_object_set_data (G_OBJECT (layout), "vibeos-sp", GINT_TO_POINTER (sp));
}

static PangoLayout *
make_layout (VibeosTimePicker *self, const char *text, int sp)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), text);
  set_font (layout, sp);
  return layout;
}

static void
refont (PangoLayout *layout)
{
  set_font (layout, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (layout), "vibeos-sp")));
}

/* the font settings changed: every layout's font again */
static void
on_text_changed (VibeosTimePicker *self)
{
  PangoLayout *header[] = { self->time_hours, self->time_minutes, self->separator, self->am, self->pm };
  for (int ring = 0; ring < 3; ring++)
    for (int i = 0; i < 12; i++)
      refont (self->rings[ring][i]);
  for (guint i = 0; i < G_N_ELEMENTS (header); i++)
    refont (header[i]);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
update_header (VibeosTimePicker *self)
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
dial_centre (VibeosTimePicker *self, double *cx, double *cy)
{
  if (self->sideways)
    {
      *cx = HEADER_W + DIAL_GAP + self->dial / 2.0;
      *cy = DIAL_TOP + self->dial / 2.0;
      return;
    }
  *cx = gtk_widget_get_width (GTK_WIDGET (self)) / 2.0;
  *cy = HEADER_H + DIAL_TOP + self->dial / 2.0;
}

/* the selector of `which`: its angle (degrees clockwise from 12) and its distance from the centre */
static void
selector_of (VibeosTimePicker *self, int which, double *degrees, double *distance)
{
  if (which == HOURS) {
    *degrees = (self->hour % 12) * 30;
    gboolean inner = self->use_24h && (self->hour == 0 || self->hour > 12);
    *distance = self->dial / 2.0 - (inner ? INSET_INNER : INSET_OUTER);
  } else {
    *degrees = self->minute * 6;
    *distance = self->dial / 2.0 - INSET_OUTER;
  }
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

static void
draw_ring (VibeosTimePicker *self, GtkSnapshot *snapshot, int ring, double radius, const GdkRGBA *color)
{
  double cx, cy;
  dial_centre (self, &cx, &cy);
  for (int i = 0; i < 12; i++) {
    double a = i * G_PI / 6;
    draw_centred (snapshot, self->rings[ring][i], cx + radius * sin (a), cy - radius * cos (a), color);
  }
}

static void
draw_numbers (VibeosTimePicker *self, GtkSnapshot *snapshot, int which, gboolean activated, float alpha)
{
  GdkRGBA outer = activated ? vibeos_color_alpha (VIBEOS_SURFACE, alpha) : vibeos_color_alpha (VIBEOS_ON_SURFACE, alpha);
  GdkRGBA inner = activated ? vibeos_color_alpha (VIBEOS_SURFACE, alpha) : vibeos_color_alpha (VIBEOS_ON_SURFACE_VARIANT, alpha);
  if (which == HOURS) {
    draw_ring (self, snapshot, 0, self->dial / 2.0 - INSET_OUTER, &outer);
    if (self->use_24h)
      draw_ring (self, snapshot, 1, self->dial / 2.0 - INSET_INNER, &inner);
  } else {
    draw_ring (self, snapshot, 2, self->dial / 2.0 - INSET_OUTER, &outer);
  }
}

static void
draw_dial (VibeosTimePicker *self, GtkSnapshot *snapshot, int which, float alpha)
{
  if (alpha <= 0)
    return;
  double cx, cy, degrees, distance;
  dial_centre (self, &cx, &cy);
  selector_of (self, which, &degrees, &distance);
  double a = degrees * G_PI / 180;
  double sx = cx + distance * sin (a), sy = cy - distance * cos (a);
  GdkRGBA selector = vibeos_color_alpha (VIBEOS_SECONDARY, alpha);

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
    GdkRGBA dot = vibeos_color_alpha (VIBEOS_SURFACE, alpha);
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

/* sideways: the column at the start, the time in its middle with AM over PM under it in 12-hour mode */
static void
draw_header_sideways (VibeosTimePicker *self, GtkSnapshot *snapshot, double height)
{
  GdkRGBA header = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);
  GdkRGBA on = vibeos_color (VIBEOS_ON_SURFACE), off = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
  PangoRectangle h, m, s, a, p;
  gboolean pm = self->hour >= 12;

  gtk_snapshot_append_color (snapshot, &header, &GRAPHENE_RECT_INIT (0, 0, HEADER_W, height));
  pango_layout_get_pixel_extents (self->time_hours, NULL, &h);
  pango_layout_get_pixel_extents (self->time_minutes, NULL, &m);
  pango_layout_get_pixel_extents (self->separator, NULL, &s);
  pango_layout_get_pixel_extents (self->am, NULL, &a);
  pango_layout_get_pixel_extents (self->pm, NULL, &p);
  double group = h.height + (self->use_24h ? 0 : 2 * AMPM_GAP + a.height + p.height);
  double y = round ((height - group) / 2), sx = round ((HEADER_W - s.width) / 2.0);
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

  double w = MAX (a.width, p.width), ax = round ((HEADER_W - w) / 2.0);
  self->am_box = GRAPHENE_RECT_INIT (ax, y + h.height + AMPM_GAP, w, a.height);
  self->pm_box = GRAPHENE_RECT_INIT (ax, y + h.height + 2 * AMPM_GAP + a.height, w, p.height);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (round ((HEADER_W - a.width) / 2.0), self->am_box.origin.y));
  gtk_snapshot_append_layout (snapshot, self->am, pm ? &off : &on);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (round ((HEADER_W - p.width) / 2.0), self->pm_box.origin.y));
  gtk_snapshot_append_layout (snapshot, self->pm, pm ? &on : &off);
  gtk_snapshot_restore (snapshot);
}

static void
draw_header (VibeosTimePicker *self, GtkSnapshot *snapshot, double width)
{
  GdkRGBA header = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_append_color (snapshot, &header, &GRAPHENE_RECT_INIT (0, 0, width, HEADER_H));

  /* the header's text colour is "primary when activated, secondary otherwise" */
  GdkRGBA on = vibeos_color (VIBEOS_ON_SURFACE), off = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
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
vibeos_time_picker_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosTimePicker *self = VIBEOS_TIME_PICKER (widget);
  double width = gtk_widget_get_width (widget);
  GdkRGBA surface = vibeos_color (VIBEOS_SURFACE);
  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, width, gtk_widget_get_height (widget)));
  if (self->sideways)
    draw_header_sideways (self, snapshot, gtk_widget_get_height (widget));
  else
    draw_header (self, snapshot, width);

  double cx, cy;
  dial_centre (self, &cx, &cy);
  GskRoundedRect disc;
  gsk_rounded_rect_init_from_rect (&disc, &GRAPHENE_RECT_INIT (cx - self->dial / 2.0, cy - self->dial / 2.0, self->dial,
                                                               self->dial), self->dial / 2.0);
  GdkRGBA background = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_push_rounded_clip (snapshot, &disc);
  gtk_snapshot_append_color (snapshot, &background, &disc.bounds);
  gtk_snapshot_pop (snapshot);

  draw_dial (self, snapshot, HOURS, 1 - self->to_minutes);
  draw_dial (self, snapshot, MINUTES, self->to_minutes);

  GskRoundedRect dot;
  GdkRGBA selector = vibeos_color (VIBEOS_SECONDARY);
  gsk_rounded_rect_init_from_rect (&dot, &GRAPHENE_RECT_INIT (cx - CENTER_DOT_R, cy - CENTER_DOT_R,
                                                              2 * CENTER_DOT_R, 2 * CENTER_DOT_R), CENTER_DOT_R);
  gtk_snapshot_push_rounded_clip (snapshot, &dot);
  gtk_snapshot_append_color (snapshot, &selector, &dot.bounds);
  gtk_snapshot_pop (snapshot);
  if (self->buttons)
    gtk_widget_snapshot_child (widget, self->buttons, snapshot);
}

/* the band under the dial that the buttons take, with their margins (none without them) */
static int
buttons_band (VibeosTimePicker *self)
{
  int height = 0;

  if (!self->buttons)
    return 0;
  gtk_widget_measure (self->buttons, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
  return BUTTONS_TOP + height + BUTTONS_BOTTOM;
}

static void
vibeos_time_picker_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                         int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  VibeosTimePicker *self = VIBEOS_TIME_PICKER (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *minimum = *natural = self->sideways ? HEADER_W + 2 * DIAL_GAP + self->dial : self->dial + 2 * DIAL_SIDE;
  else
    *minimum = *natural = (self->sideways ? 0 : HEADER_H) + DIAL_TOP + self->dial + buttons_band (self);
}

/* the buttons in the corner under the dial, at the end */
static void
vibeos_time_picker_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosTimePicker *self = VIBEOS_TIME_PICKER (widget);
  int w, h;

  if (!self->buttons)
    return;
  gtk_widget_measure (self->buttons, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  gtk_widget_measure (self->buttons, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
  gtk_widget_allocate (self->buttons, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (width - BUTTONS_END - w,
                                                                            height - BUTTONS_BOTTOM - h)));
}

static void
set_sideways (VibeosTimePicker *self, gboolean sideways)
{
  if (sideways == self->sideways)
    return;
  self->sideways = sideways;
  self->dial = sideways ? DIAL_SIDEWAYS : DIAL;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

/* the window turned: sideways when it is wider than tall */
static void
on_layout_changed (VibeosLayout *layout, VibeosTimePicker *self)
{
  set_sideways (self, vibeos_layout_is_landscape (layout));
}

/* ---- input ----------------------------------------------------------------------------------------------------- */

static void
on_fade (VibeosAnim *anim, double value, gpointer data)
{
  VibeosTimePicker *self = data;
  self->to_minutes = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
show_dial (VibeosTimePicker *self, int which)
{
  if (self->showing == which)
    return;
  self->showing = which;
  /* accelerate-decelerate */
  vibeos_anim_start (self->fade, self->to_minutes, which == MINUTES, FADE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

/* The value under (x, y), snapped to the nearest hour or minute, or FALSE when a
 * touch-down lands off the numbers (force: a drag already under way takes any point) */
static gboolean
pick (VibeosTimePicker *self, double x, double y, gboolean force)
{
  double cx, cy;
  dial_centre (self, &cx, &cy);
  double dx = x - cx, dy = y - cy, distance = hypot (dx, dy);
  double outer = self->dial / 2.0 - INSET_OUTER, inner = self->dial / 2.0 - INSET_INNER;
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
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosTimePicker *self)
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
  } else if ((self->sideways ? x > HEADER_W : y > HEADER_H) && pick (self, x, y, FALSE)) {
    /* the dial has the touch: the dialog's scrim and anything else keep out of it */
    self->dragging = TRUE;
    vibeos_anim_finish (self->fade);
  }
}

static void
on_drag (GtkGestureDrag *drag, double dx, double dy, VibeosTimePicker *self)
{
  double x, y;
  if (!self->dragging || !gtk_gesture_drag_get_start_point (drag, &x, &y))
    return;
  pick (self, x + dx, y + dy, TRUE);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, VibeosTimePicker *self)
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
vibeos_time_picker_dispose (GObject *object)
{
  VibeosTimePicker *self = VIBEOS_TIME_PICKER (object);
  g_clear_pointer (&self->fade, vibeos_anim_free);
  g_clear_pointer (&self->buttons, gtk_widget_unparent);
  for (int r = 0; r < 3; r++)
    for (int i = 0; i < 12; i++)
      g_clear_object (&self->rings[r][i]);
  g_clear_object (&self->time_hours);
  g_clear_object (&self->time_minutes);
  g_clear_object (&self->separator);
  g_clear_object (&self->am);
  g_clear_object (&self->pm);
  G_OBJECT_CLASS (vibeos_time_picker_parent_class)->dispose (object);
}

static void
vibeos_time_picker_class_init (VibeosTimePickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = vibeos_time_picker_dispose;
  widget_class->snapshot = vibeos_time_picker_snapshot;
  widget_class->measure = vibeos_time_picker_measure;
  widget_class->size_allocate = vibeos_time_picker_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "vibeos-time-picker");
}

static void
vibeos_time_picker_init (VibeosTimePicker *self)
{
  self->fade = vibeos_anim_new (GTK_WIDGET (self), on_fade, self);
  self->dial = DIAL;

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
vibeos_time_picker_new (int hour, int minute, gboolean use_24h)
{
  VibeosTimePicker *self = g_object_new (VIBEOS_TYPE_TIME_PICKER, NULL);
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
  g_signal_connect_object (vibeos_text_get_default (), "changed", G_CALLBACK (on_text_changed), self,
                           G_CONNECT_SWAPPED);
  update_header (self);
  return GTK_WIDGET (self);
}

void
vibeos_time_picker_get_time (VibeosTimePicker *self, int *hour, int *minute)
{
  *hour = self->hour;
  *minute = self->minute;
}

/* ---- the dialog ------------------------------------------------------------------------------------------------ */

typedef struct {
  VibeosTimeChosen  chosen;
  gpointer       data;
  GDestroyNotify destroy;
  VibeosTimePicker *picker;
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
on_response (VibeosDialog *dialog, int response, Pending *pending)
{
  if (response == VIBEOS_RESPONSE_OK) {
    int hour, minute;
    vibeos_time_picker_get_time (pending->picker, &hour, &minute);
    pending->chosen (hour, minute, pending->data);
  }
}

static void
on_button (GtkWidget *button, VibeosDialog *dialog)
{
  vibeos_dialog_respond (dialog, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "vibeos-response")));
}

static void
add_button (GtkWidget *row, VibeosDialog *dialog, const char *label, int response)
{
  GtkWidget *button = vibeos_text_button_new (label);

  g_object_set_data (G_OBJECT (button), "vibeos-response", GINT_TO_POINTER (response));
  g_signal_connect (button, "clicked", G_CALLBACK (on_button), dialog);
  gtk_box_append (GTK_BOX (row), button);
}

void
vibeos_time_picker_dialog (GtkWidget *parent, int hour, int minute, VibeosTimeChosen chosen, gpointer data,
                        GDestroyNotify destroy)
{
  VibeosDialog *dialog = vibeos_dialog_new (NULL);
  GtkRoot *root = gtk_widget_get_root (parent);
  /* the picker reaches the card's edges, on the plain surface colour */
  vibeos_dialog_set_flush (dialog, TRUE);
  GtkWidget *picker = vibeos_time_picker_new (hour, minute, vibeos_clock_uses_24h ());
  VibeosTimePicker *self = VIBEOS_TIME_PICKER (picker);
  /* Cancel and OK in the picker's corner rather than in a row of the dialog's under it, so that sideways the time's
   * column reaches the card's bottom beside them */
  self->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BUTTONS_GAP);
  add_button (self->buttons, dialog, "Cancel", VIBEOS_RESPONSE_CANCEL);
  add_button (self->buttons, dialog, "OK", VIBEOS_RESPONSE_OK);
  gtk_widget_set_parent (self->buttons, picker);
  /* sideways in a window wider than tall (a tablet or a phone held sideways), and turned with the window */
  if (GTK_IS_WINDOW (root))
    {
      VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (root));

      set_sideways (self, vibeos_layout_is_landscape (layout));
      g_signal_connect_object (layout, "changed", G_CALLBACK (on_layout_changed), picker, 0);
    }
  vibeos_dialog_set_content (dialog, picker);
  Pending *pending = g_new (Pending, 1);
  *pending = (Pending) { chosen, data, destroy, VIBEOS_TIME_PICKER (picker) };
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), pending, pending_free, 0);
  vibeos_dialog_present (dialog, parent);
}
