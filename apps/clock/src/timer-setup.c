/* TimerSetup: see timer-setup.h. The time over the keypad, in a box as wide as the pad; its digits shrink to fit
 * the box. */
#include "timer-setup.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <string.h>

#define FAB_ROW         120     /* the view's bottom padding, for the buttons' row */
#define TIME_FRACTION   0.25    /* the time between guidelines at the top and a quarter of the height */
#define TIME_SIZE       72      /* at most: it shrinks to fit */
#define SUFFIX_SCALE    0.5f    /* h, m and s at half the size */
#define PAD_MAX_WIDTH   315     /* the pad: 3:4, at most this wide and PAD_MAX_HEIGHT tall */
#define PAD_MAX_HEIGHT  420
#define PAD_MARGIN      16      /* ... with this much under it */
#define KEY_MARGIN      2       /* a key, in from its cell's edges */
#define DIGIT_SIZE      36      /* ... its text */
#define DELETE_ICON     40      /* the delete key's icon */
#define DIGITS          6

/* the keys in the pad's order: 1 to 9, then 00, 0 and delete */
enum { KEY_00 = 9, KEY_0 = 10, KEY_DELETE = 11, N_KEYS = 12 };

struct _TimerSetup
{
  GtkWidget parent;
  int input[DIGITS];            /* the digits typed, the last one first */
  int pointer;                  /* the index of the first one typed; -1 for none */
  GtkWidget *keys[N_KEYS];
  PangoLayout *time;            /* "00h 00m 00s", drawn by the view itself as its size changes with the width */
  graphene_rect_t time_box;
  double time_size;
};

G_DEFINE_FINAL_TYPE (TimerSetup, timer_setup, GTK_TYPE_WIDGET)

enum { INPUT_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
values (TimerSetup *self, int *hours, int *minutes, int *seconds)
{
  *seconds = self->input[1] * 10 + self->input[0];
  *minutes = self->input[3] * 10 + self->input[2];
  *hours = self->input[5] * 10 + self->input[4];
}

/* --- the time */

static void
append_size (PangoAttrList *attributes, double size, guint start, guint end)
{
  PangoAttribute *attribute = pango_attr_size_new_absolute (size * PANGO_SCALE);

  attribute->start_index = start;
  attribute->end_index = end;
  pango_attr_list_insert (attributes, attribute);
}

/* the template "^1^4 ^2^5 ^3^6" at `size`: each group of two digits, its letter at half the size; from the first
 * group that is not zero to the end in the accent colour. The groups start at characters 0, 4 and 8. */
static void
set_time (TimerSetup *self, double size)
{
  const char *letters[3] = { _("h"), _("m"), _("s") };
  int numbers[3];
  g_autoptr(GString) text = g_string_new (NULL);
  g_autoptr(PangoAttrList) attributes = pango_attr_list_new ();
  int highlight;

  values (self, &numbers[0], &numbers[1], &numbers[2]);
  append_size (attributes, size, 0, G_MAXUINT);
  for (int i = 0; i < 3; i++)
    {
      guint start;

      if (i)
        g_string_append_c (text, ' ');
      g_string_append_printf (text, "%02d", numbers[i]);
      start = text->len;
      g_string_append (text, letters[i]);
      append_size (attributes, size * SUFFIX_SCALE, start, text->len);
    }
  highlight = numbers[0] ? 0 : numbers[1] ? 4 : numbers[2] ? 8 : -1;
  if (highlight >= 0 && highlight < (int) g_utf8_strlen (text->str, -1))
    {
      GdkRGBA primary = vibeos_color (VIBEOS_PRIMARY);
      PangoAttribute *color = pango_attr_foreground_new (primary.red * 65535, primary.green * 65535,
                                                         primary.blue * 65535);

      color->start_index = g_utf8_offset_to_pointer (text->str, highlight) - text->str;
      color->end_index = G_MAXUINT;
      pango_attr_list_insert (attributes, color);
    }
  pango_layout_set_text (self->time, text->str, -1);
  pango_layout_set_attributes (self->time, attributes);
}

/* the largest size in whole device pixels, up to TIME_SIZE, at which the time fits the box's width, found by
 * halving. The box is a whole number of device pixels (735.75 at 3:4 becomes 736). At 150 device pixels the Roboto
 * we ship lays this line out 736.41 wide, a fraction over, so half a pixel over still counts as fitting: else the
 * time comes out a size smaller than it should. */
#define FIT_SLACK 0.5

static void
fit_time (TimerSetup *self)
{
  int scale = vibeos_device_scale (GTK_WIDGET (self));
  int low = 1, high = TIME_SIZE * scale, size = high;
  double room = round (self->time_box.size.width * scale) + FIT_SLACK;

  while (self->time_box.size.width > 0 && high >= low)
    {
      int middle = (low + high + 1) / 2;
      PangoRectangle extents;

      set_time (self, (double) middle / scale);
      pango_layout_get_extents (self->time, NULL, &extents);
      if ((double) extents.width / PANGO_SCALE * scale > room)
        high = middle - 1;
      else
        {
          size = middle;
          low = middle + 1;
        }
    }
  self->time_size = (double) size / scale;
  set_time (self, self->time_size);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* what a screen reader says: "0 hours, 1 minute, 30 seconds" (timer_setup_description) */
static void
describe_time (TimerSetup *self)
{
  int hours, minutes, seconds;
  g_autofree char *h = NULL, *m = NULL, *s = NULL, *text = NULL;

  values (self, &hours, &minutes, &seconds);
  h = g_strdup_printf (ngettext ("%d hour", "%d hours", hours), hours);
  m = g_strdup_printf (ngettext ("%d minute", "%d minutes", minutes), minutes);
  s = g_strdup_printf (ngettext ("%d second", "%d seconds", seconds), seconds);
  text = g_strdup_printf ("%s, %s, %s", h, m, s);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

static void
update_time (TimerSetup *self)
{
  fit_time (self);
  describe_time (self);
}

/* --- typing */

static void
describe_delete (TimerSetup *self)
{
  g_autofree char *text = NULL;

  /* "Delete 3" for the digit it would take back */
  if (self->pointer >= 0)
    text = g_strdup_printf (_("Delete %d"), self->input[0]);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->keys[KEY_DELETE]), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  text ? text : _("Delete"), -1);
}

static void
append (TimerSetup *self, int digit)
{
  /* a 0 first does nothing; a seventh digit is not taken */
  if ((self->pointer == -1 && digit == 0) || self->pointer == DIGITS - 1)
    return;
  memmove (&self->input[1], &self->input[0], (self->pointer + 1) * sizeof *self->input);
  self->input[0] = digit;
  self->pointer++;
  update_time (self);
  describe_delete (self);
  if (self->pointer == 0)
    g_signal_emit (self, signals[INPUT_CHANGED], 0);
}

static void
delete (TimerSetup *self)
{
  if (self->pointer < 0)
    return;
  memmove (&self->input[0], &self->input[1], self->pointer * sizeof *self->input);
  self->input[self->pointer] = 0;
  self->pointer--;
  update_time (self);
  describe_delete (self);
  if (self->pointer == -1)
    g_signal_emit (self, signals[INPUT_CHANGED], 0);
}

void
timer_setup_reset (TimerSetup *self)
{
  if (self->pointer == -1)
    return;
  memset (self->input, 0, sizeof self->input);
  self->pointer = -1;
  update_time (self);
  describe_delete (self);
}

static void
on_key (VibeosPressable *key, TimerSetup *self)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (key), "key"));

  if (index == KEY_DELETE)
    delete (self);
  else if (index == KEY_00)
    {
      append (self, 0);
      append (self, 0);
    }
  else
    append (self, index == KEY_0 ? 0 : index + 1);
}

/* a long press on delete clears it all, and the FAB shrinks and grows back even when there was nothing */
static void
on_delete_held (VibeosPressable *key, TimerSetup *self)
{
  timer_setup_reset (self);
  g_signal_emit (self, signals[INPUT_CHANGED], 0);
}

gboolean
timer_setup_key_pressed (TimerSetup *self, guint keyval)
{
  if (keyval == GDK_KEY_BackSpace)
    on_key (VIBEOS_PRESSABLE (self->keys[KEY_DELETE]), self);
  else if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
    append (self, keyval - GDK_KEY_0);
  else if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9)
    append (self, keyval - GDK_KEY_KP_0);
  else
    return FALSE;
  return TRUE;
}

gboolean
timer_setup_has_input (TimerSetup *self)
{
  return self->pointer != -1;
}

gint64
timer_setup_get_length (TimerSetup *self)
{
  int hours, minutes, seconds;

  values (self, &hours, &minutes, &seconds);
  return ((gint64) hours * 3600 + minutes * 60 + seconds) * 1000;
}

/* --- GtkWidget */

/* a guideline at `fraction` of `size`, on a whole device pixel */
static double
guideline (double size, double fraction, int scale)
{
  return floor (0.5 + fraction * size * scale) / scale;
}

static void
timer_setup_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  TimerSetup *self = TIMER_SETUP (widget);
  int scale = vibeos_device_scale (widget);
  double inner = height - FAB_ROW, time_bottom = guideline (inner, TIME_FRACTION, scale);
  double room = inner - PAD_MARGIN - time_bottom;
  double pad_height = MIN (room, PAD_MAX_HEIGHT), pad_width = pad_height * 3 / 4;
  double pad_x, pad_y, columns[4], rows[5];
  double old_width = self->time_box.size.width;

  /* the pad as large as fits under the time at 3:4, centred; the time above it, as wide */
  if (pad_width > MIN (width, PAD_MAX_WIDTH))
    {
      pad_width = MIN (width, PAD_MAX_WIDTH);
      pad_height = pad_width * 4 / 3;
    }
  pad_x = (width - pad_width) / 2;
  pad_y = time_bottom + (room - pad_height) / 2;
  self->time_box = GRAPHENE_RECT_INIT (pad_x, 0, pad_width, time_bottom);
  if (pad_width != old_width)
    fit_time (self);

  /* the keys in cells between the pad's guidelines, square, KEY_MARGIN in from the cell's edges */
  columns[0] = rows[0] = 0;
  columns[1] = guideline (pad_width, 0.3333, scale);
  columns[2] = guideline (pad_width, 0.6666, scale);
  columns[3] = pad_width;
  for (int i = 1; i < 4; i++)
    rows[i] = guideline (pad_height, 0.25 * i, scale);
  rows[4] = pad_height;
  for (int i = 0; i < N_KEYS; i++)
    {
      int row = i / 3, column = i % 3;
      double cell_width = columns[column + 1] - columns[column] - 2 * KEY_MARGIN;
      double cell_height = rows[row + 1] - rows[row] - 2 * KEY_MARGIN;
      double size = round (MIN (cell_width, cell_height));
      graphene_point_t at = GRAPHENE_POINT_INIT (pad_x + columns[column] + KEY_MARGIN + (cell_width - size) / 2,
                                                 pad_y + rows[row] + KEY_MARGIN + (cell_height - size) / 2);

      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->keys[i]), VIBEOS_RIPPLE_CIRCLE, size / 2);
      gtk_widget_allocate (self->keys[i], size, size, -1, gsk_transform_translate (NULL, &at));
    }
}

static void
timer_setup_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* the time centred in its box with its font padding: the baseline 0.39 of the size
 * under the middle (the padding is Roboto's yMax 2163 over yMin -555 of 2048 units) */
static void
timer_setup_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  TimerSetup *self = TIMER_SETUP (widget);
  GdkRGBA color = vibeos_color (VIBEOS_ON_SURFACE);
  PangoRectangle extents;
  double baseline = self->time_box.origin.y + self->time_box.size.height / 2
                    + self->time_size * (2163 - 555) / 2.0 / 2048;

  pango_layout_get_extents (self->time, NULL, &extents);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (
    self->time_box.origin.x + (self->time_box.size.width - (double) extents.width / PANGO_SCALE) / 2,
    baseline - (double) pango_layout_get_baseline (self->time) / PANGO_SCALE));
  gtk_snapshot_append_layout (snapshot, self->time, &color);
  gtk_snapshot_restore (snapshot);
  GTK_WIDGET_CLASS (timer_setup_parent_class)->snapshot (widget, snapshot);
}

/* the layout takes the widget's font when it is made: again when the style changes */
static void
timer_setup_css_changed (GtkWidget *widget, GtkCssStyleChange *change)
{
  TimerSetup *self = TIMER_SETUP (widget);

  GTK_WIDGET_CLASS (timer_setup_parent_class)->css_changed (widget, change);
  g_set_object (&self->time, NULL);
  self->time = gtk_widget_create_pango_layout (widget, NULL);
  fit_time (self);
}

static void
timer_setup_dispose (GObject *object)
{
  TimerSetup *self = TIMER_SETUP (object);

  for (int i = 0; i < N_KEYS; i++)
    g_clear_pointer (&self->keys[i], gtk_widget_unparent);
  g_clear_object (&self->time);
  G_OBJECT_CLASS (timer_setup_parent_class)->dispose (object);
}

static void
timer_setup_class_init (TimerSetupClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = timer_setup_dispose;
  widget_class->measure = timer_setup_measure;
  widget_class->size_allocate = timer_setup_size_allocate;
  widget_class->snapshot = timer_setup_snapshot;
  widget_class->css_changed = timer_setup_css_changed;
  gtk_widget_class_set_css_name (widget_class, "timer-setup");
  signals[INPUT_CHANGED] = g_signal_new ("input-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                         NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
timer_setup_init (TimerSetup *self)
{
  GdkRGBA key_fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), delete_fill = vibeos_color (VIBEOS_TERTIARY_CONTAINER);

  self->pointer = -1;
  self->time = gtk_widget_create_pango_layout (GTK_WIDGET (self), NULL);
  self->time_size = TIME_SIZE;
  set_time (self, TIME_SIZE);

  /* round keys, delete in a colour of its own. The pad is left to right in every language. */
  gtk_widget_set_direction (GTK_WIDGET (self), GTK_TEXT_DIR_LTR);
  for (int i = 0; i < N_KEYS; i++)
    {
      GtkWidget *child;

      if (i == KEY_DELETE)
        {
          child = gtk_image_new_from_icon_name ("clock-backspace-symbolic");
          gtk_image_set_pixel_size (GTK_IMAGE (child), DELETE_ICON);
          gtk_widget_add_css_class (child, "timer-delete");
        }
      else
        {
          g_autofree char *digit = i == KEY_00 ? g_strdup ("00") : g_strdup_printf ("%d", i == KEY_0 ? 0 : i + 1);

          child = gtk_label_new (digit);
          gtk_widget_add_css_class (child, "timer-digit");
          vibeos_label_set_font_padding_sp (child, DIGIT_SIZE);
          gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
        }
      self->keys[i] = vibeos_pressable_new (child);
      vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->keys[i]), i == KEY_DELETE ? &delete_fill : &key_fill, FALSE);
      g_object_set_data (G_OBJECT (self->keys[i]), "key", GINT_TO_POINTER (i));
      g_signal_connect (self->keys[i], "clicked", G_CALLBACK (on_key), self);
      gtk_widget_set_parent (self->keys[i], GTK_WIDGET (self));
    }
  g_signal_connect (self->keys[KEY_DELETE], "long-pressed", G_CALLBACK (on_delete_held), self);
  describe_delete (self);
  describe_time (self);
}

GtkWidget *
timer_setup_new (void)
{
  return g_object_new (TIMER_TYPE_SETUP, NULL);
}
