/* The colour picker (cal-color-picker.h).
 *
 * At its small size (a phone): a dialog with the title over the scrolled palette, a table of 4 columns (NUM_COLUMNS)
 * of round swatches. Every second row runs right to left and a short last row is filled with blank cells.
 *
 * A swatch is an oval in its colour; pressed, its value drops to 70 %, without a ripple. The checked one carries a
 * faint black disc with a white check in it. A tap chooses and dismisses. */
#include "cal-color-picker.h"
#include "cal-colors.h"
#include "cal-model.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define COLUMNS 4
#define SWATCH 48.0
#define MARGIN 4.0
#define PRESSED_VALUE 0.70f

static const guint32 default_colors[] = {
  0xd1270c, 0x11ab64, 0x1454b9, 0x4ea719, 0x62b492, 0x6365cc, 0x6e96b9, 0x6eb3b9,
  0x7449b5, 0x8363cc, 0x85b03c, 0x8a4d38, 0x9b9b9b, 0x9e41b8, 0xa29496, 0xa37c82,
  0xa63d36, 0xc55c7e, 0xc61600, 0xc81e02, 0xc89d2d, 0xc9b64c, 0xcc3f00, 0xcc770c,
};

const guint32 *
cal_color_picker_default_colors (int *n_colors)
{
  *n_colors = G_N_ELEMENTS (default_colors);
  return default_colors;
}

/* --- HSV */

static void
to_hsv (guint32 rgb, float *h, float *s, float *v)
{
  float r = ((rgb >> 16) & 0xff) / 255.f, g = ((rgb >> 8) & 0xff) / 255.f, b = (rgb & 0xff) / 255.f;
  float max = MAX (r, MAX (g, b)), min = MIN (r, MIN (g, b)), d = max - min;

  *v = max;
  *s = max > 0 ? d / max : 0;
  if (d == 0)
    *h = 0;
  else if (max == r)
    *h = 60 * fmodf ((g - b) / d + 6, 6);
  else if (max == g)
    *h = 60 * ((b - r) / d + 2);
  else
    *h = 60 * ((r - g) / d + 4);
}

static guint32
from_hsv (float h, float s, float v)
{
  float c = v * s, x = c * (1 - fabsf (fmodf (h / 60, 2) - 1)), m = v - c, r, g, b;
  int sector = (int) (h / 60) % 6;

  switch (sector)
    {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
  return ((guint32) lroundf ((r + m) * 255) << 16) | ((guint32) lroundf ((g + m) * 255) << 8)
         | (guint32) lroundf ((b + m) * 255);
}

typedef struct {
  guint32 raw, display;
} Color;

/* by hue, then saturation, then value, each the higher first */
static int
compare_hsv (gconstpointer a, gconstpointer b)
{
  float h1, s1, v1, h2, s2, v2;

  to_hsv (((const Color *) a)->display, &h1, &s1, &v1);
  to_hsv (((const Color *) b)->display, &h2, &s2, &v2);
  if (h1 != h2)
    return h1 < h2 ? 1 : -1;
  if (s1 != s2)
    return s1 < s2 ? 1 : -1;
  if (v1 != v2)
    return v1 < v2 ? 1 : -1;
  return 0;
}

/* --- a swatch */

#define CAL_TYPE_SWATCH (cal_swatch_get_type ())
G_DECLARE_FINAL_TYPE (CalSwatch, cal_swatch, CAL, SWATCH, GtkWidget)

struct _CalSwatch {
  GtkWidget parent;
  guint32 color;                /* the display colour */
  gboolean checked, pressed;
};

G_DEFINE_FINAL_TYPE (CalSwatch, cal_swatch, GTK_TYPE_WIDGET)

/* the check mark on the chosen swatch, in the coordinates of a SWATCH-wide one */
static GskPath *
check_path (void)
{
  static GskPath *path;

  if (!path)
    path = gsk_path_parse ("M 18.5 23 L 21.5 29.5 L 30.5 18.5");
  return path;
}

static void
cal_swatch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalSwatch *self = CAL_SWATCH (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget), size = MIN (w, h);
  guint32 color = self->color;
  GdkRGBA rgba;
  GskRoundedRect oval;

  if (self->pressed)
    {
      float hue, s, v;

      to_hsv (color, &hue, &s, &v);
      color = from_hsv (hue, s, v * PRESSED_VALUE);
    }
  rgba = cal_argb (0xff000000 | color);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((w - size) / 2, (h - size) / 2));
  gsk_rounded_rect_init_from_rect (&oval, &GRAPHENE_RECT_INIT (0, 0, size, size), size / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &oval);
  gtk_snapshot_append_color (snapshot, &rgba, &oval.bounds);
  gtk_snapshot_pop (snapshot);
  if (self->checked)
    {
      double scale = size / SWATCH, disc = 31 * scale;
      GdkRGBA shade = { 0, 0, 0, 38 / 255.f }, white = { 1, 1, 1, 1 };
      g_autoptr(GskStroke) stroke = gsk_stroke_new (3.5f);

      gsk_rounded_rect_init_from_rect (&oval, &GRAPHENE_RECT_INIT ((size - disc) / 2, (size - disc) / 2, disc, disc),
                                       disc / 2);
      gtk_snapshot_push_rounded_clip (snapshot, &oval);
      gtk_snapshot_append_color (snapshot, &shade, &oval.bounds);
      gtk_snapshot_pop (snapshot);
      gtk_snapshot_scale (snapshot, scale, scale);
      gsk_stroke_set_line_join (stroke, GSK_LINE_JOIN_MITER);
      gtk_snapshot_append_stroke (snapshot, check_path (), stroke, &white);
    }
  gtk_snapshot_restore (snapshot);
}

static void
cal_swatch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                    int *nat_base)
{
  *min = *nat = (int) SWATCH;
}

static void
cal_swatch_class_init (CalSwatchClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = cal_swatch_snapshot;
  widget_class->measure = cal_swatch_measure;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
}

static void
cal_swatch_init (CalSwatch *self)
{
}

static void
set_pressed (CalSwatch *swatch, gboolean pressed)
{
  swatch->pressed = pressed;
  gtk_widget_queue_draw (GTK_WIDGET (swatch));
}

static void
on_swatch_pressed (GtkGestureClick *click, int n, double x, double y, CalSwatch *swatch)
{
  set_pressed (swatch, TRUE);
}

/* GtkGesture::end (and ::cancel): the sequence comes before the data */
static void
on_swatch_released (GtkGesture *gesture, GdkEventSequence *sequence, CalSwatch *swatch)
{
  set_pressed (swatch, FALSE);
}

/* --- the dialog */

typedef struct {
  CalColorChosen chosen;
  gpointer data;
  GArray *colors;               /* Color, as shown */
  guint32 calendar_color;       /* the event dialog's "default": raw */
  NeoDialog *dialog;
} Picker;

static void
picker_free (gpointer data, GClosure *closure)
{
  Picker *picker = data;

  g_array_unref (picker->colors);
  g_free (picker);
}

#define RESPONSE_DEFAULT 2
#define RESPONSE_SWATCH 100

static void
on_response (NeoDialog *dialog, int response, Picker *picker)
{
  if (response == RESPONSE_DEFAULT)
    picker->chosen (picker->calendar_color, picker->data);
  else if (response >= RESPONSE_SWATCH)
    picker->chosen (g_array_index (picker->colors, Color, response - RESPONSE_SWATCH).raw, picker->data);
}

static void
on_swatch (NeoPressable *pressable, Picker *picker)
{
  neo_dialog_respond (picker->dialog, RESPONSE_SWATCH + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (pressable),
                                                                                             "cal-index")));
}

static void
on_default (NeoPressable *button, Picker *picker)
{
  neo_dialog_respond (picker->dialog, RESPONSE_DEFAULT);
}

static gboolean
real_colors (GtkWidget *parent)
{
  GtkRoot *root = gtk_widget_get_root (parent);

  /* outside the app's window (a test) there are no settings: the default, off */
  return root && g_object_get_data (G_OBJECT (root), "cal-shell")
         && g_settings_get_boolean (cal_window_get_settings (parent), "real-event-colors");
}

static GtkWidget *
cell (Picker *picker, int index, guint32 current_display)
{
  GtkWidget *swatch, *pressable;
  GtkGesture *click;
  const Color *color;
  g_autofree char *name = NULL;

  if (index < 0)
    {
      /* a blank cell */
      GtkWidget *blank = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

      gtk_widget_set_size_request (blank, (int) SWATCH, (int) SWATCH);
      gtk_widget_add_css_class (blank, "cal-swatch");
      return blank;
    }
  color = &g_array_index (picker->colors, Color, index);
  swatch = g_object_new (CAL_TYPE_SWATCH, NULL);
  CAL_SWATCH (swatch)->color = color->display;
  CAL_SWATCH (swatch)->checked = color->display == current_display;
  /* the pressed state only: the tap is the pressable's, which a scroll takes away */
  click = gtk_gesture_click_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_swatch_pressed), swatch);
  g_signal_connect (click, "end", G_CALLBACK (on_swatch_released), swatch);
  gtk_widget_add_controller (swatch, GTK_EVENT_CONTROLLER (click));
  pressable = neo_pressable_new (swatch);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_NONE, 0);
  gtk_widget_add_css_class (pressable, "cal-swatch");
  g_object_set_data (G_OBJECT (pressable), "cal-index", GINT_TO_POINTER (index));
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_swatch), picker);
  /* each swatch's accessible name, numbered in reading order */
  name = CAL_SWATCH (swatch)->checked ? g_strdup_printf (_("Color %d selected"), index + 1)
                                      : g_strdup_printf (_("Color %d"), index + 1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return pressable;
}

static void
show (GtkWidget *parent, const char *title, const guint32 *colors, int n_colors, guint32 current, gboolean event,
      guint32 calendar_color, CalColorChosen chosen, gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  gboolean real = real_colors (parent);
  guint32 current_display = cal_display_color (current & 0xffffff, real);
  GtkWidget *grid = gtk_grid_new (), *scroll = spongy_scroll_view_new (), *content;
  int rows;

  picker->chosen = chosen;
  picker->data = data;
  picker->calendar_color = calendar_color;
  picker->colors = g_array_new (FALSE, FALSE, sizeof (Color));
  for (int i = 0; i < n_colors; i++)
    {
      Color color = { colors[i] & 0xffffff, cal_display_color (colors[i] & 0xffffff, real) };
      gboolean twin = FALSE;

      /* one swatch a display colour, as the colour key map holds one */
      for (guint j = 0; j < picker->colors->len; j++)
        twin |= g_array_index (picker->colors, Color, j).display == color.display;
      if (!twin)
        g_array_append_val (picker->colors, color);
    }
  g_array_sort (picker->colors, compare_hsv);

  rows = ((int) picker->colors->len + COLUMNS - 1) / COLUMNS;
  for (int row = 0; row < rows; row++)
    for (int column = 0; column < COLUMNS; column++)
      {
        int index = row * COLUMNS + column;
        /* odd rows right to left */
        int place = row % 2 ? COLUMNS - 1 - column : column;

        gtk_grid_attach (GTK_GRID (grid), cell (picker, index < (int) picker->colors->len ? index : -1,
                                                current_display),
                         place, row, 1, 1);
      }
  gtk_widget_set_halign (grid, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (grid, "cal-palette");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), grid);
  gtk_widget_add_css_class (scroll, "cal-palette-scroll");
  content = scroll;

  picker->dialog = neo_dialog_new (title);
  gtk_widget_add_css_class (GTK_WIDGET (picker->dialog), "cal-color-picker");
  if (event)
    {
      /* the neutral button sits at the start of the button bar, on its own */
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *reset = neo_text_button_new (_("Set to default calendar color"));

      gtk_box_append (GTK_BOX (box), scroll);
      gtk_widget_add_css_class (buttons, "neo-dialog-buttons");
      gtk_widget_add_css_class (buttons, "cal-dialog-neutral");
      gtk_box_append (GTK_BOX (buttons), reset);
      g_signal_connect (reset, "clicked", G_CALLBACK (on_default), picker);
      gtk_box_append (GTK_BOX (box), buttons);
      content = box;
    }
  neo_dialog_set_content (picker->dialog, content);
  g_signal_connect_data (picker->dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  neo_dialog_present (picker->dialog, parent);
}

void
cal_color_picker_show (GtkWidget *parent, const char *title, const guint32 *colors, int n_colors, guint32 current,
                       CalColorChosen chosen, gpointer data)
{
  show (parent, title, colors, n_colors, current, FALSE, 0, chosen, data);
}

void
cal_event_color_picker_show (GtkWidget *parent, const guint32 *colors, int n_colors, guint32 current,
                             guint32 calendar_color, CalColorChosen chosen, gpointer data)
{
  show (parent, _("Event Color"), colors, n_colors, current, TRUE, calendar_color & 0xffffff, chosen, data);
}
