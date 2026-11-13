/* An outlined text field (cal-outlined-field.h). */
#include "cal-outlined-field.h"

#include <math.h>
#include <neo.h>

#include "cal-colors.h"
#include "cal-text.h"

#define LABEL_SIZE 12.0                 /* the label on the edge */
#define LABEL_SPACING 0.4               /* the label's letter spacing, 0.0333 em */
#define LABEL_CUTOUT 4.0                /* the outline's gap at each side of the label */
#define CORNER 4.0                      /* the box's corner radius */
#define LABEL_MS 167                    /* the label moving */
#define ASCENT (1900 / 2048.0)          /* Roboto's ascent, per px of size */

struct _CalOutlinedField {
  GtkWidget parent;
  GtkWidget *child;
  char *label;
  double text_size, letter_spacing, padding;
  double box_height;            /* as allocated */
  double floated;               /* the label: 0 in the box at the text's size, 1 on the edge, smaller */
  gboolean up;                  /* where it goes: on the edge (the cutout opens at once) */
  gboolean focused;             /* the text has the focus (as the focus controller tells: in its "enter" the
                                 * widget's has-focus is not set yet) */
  NeoAnim *anim;
};

G_DEFINE_FINAL_TYPE (CalOutlinedField, cal_outlined_field, GTK_TYPE_WIDGET)

/* the box's top: half the small label's ascent, in whole device pixels */
static double
label_margin (int scale)
{
  return floor (LABEL_SIZE * scale * ASCENT / 2) / scale;
}

/* the child's own line: its ascent and height as Pango sets it */
static void
child_line (CalOutlinedField *self, double *ascent, double *height)
{
  g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout (self->child, "X");
  PangoRectangle logical;

  pango_layout_get_extents (layout, NULL, &logical);
  *ascent = pango_layout_get_baseline (layout) / (double) PANGO_SCALE;
  *height = logical.height / (double) PANGO_SCALE;
}

/* the box's height for the child's lines: the first and last with font padding, those between at the
 * font's spacing */
static double
box_height (CalOutlinedField *self, int child_height, int scale)
{
  double ascent, line, first, spacing;
  int lines = 1;

  if (GTK_IS_TEXT_VIEW (self->child))
    {
      child_line (self, &ascent, &line);
      lines = MAX (1, (int) round (child_height / line));
    }
  cal_text_line (self->text_size, scale, NULL, &first);
  cal_paint_line (self->text_size, scale, NULL, &spacing);
  return 2 * self->padding + first + (lines - 1) * spacing;
}

static GtkSizeRequestMode
cal_outlined_field_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
cal_outlined_field_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                            int *min_baseline, int *nat_baseline)
{
  CalOutlinedField *self = CAL_OUTLINED_FIELD (widget);
  int inset = (int) ceil (2 * self->padding), child_min, child_nat;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      gtk_widget_measure (self->child, orientation, -1, &child_min, &child_nat, NULL, NULL);
      *min = child_min + inset;
      *nat = child_nat + inset;
    }
  else
    {
      int scale = neo_device_scale (widget);

      gtk_widget_measure (self->child, orientation, for_size < 0 ? -1 : MAX (0, for_size - inset), &child_min,
                          &child_nat, NULL, NULL);
      *min = *nat = (int) ceil (label_margin (scale) + box_height (self, child_nat, scale) - 0.01);
    }
}

static void
cal_outlined_field_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalOutlinedField *self = CAL_OUTLINED_FIELD (widget);
  int scale = neo_device_scale (widget), child_width = MAX (0, width - (int) ceil (2 * self->padding));
  int child_min, child_height;
  double ascent, line, text_baseline, y;

  gtk_widget_measure (self->child, GTK_ORIENTATION_VERTICAL, child_width, &child_min, &child_height, NULL, NULL);
  self->box_height = box_height (self, child_height, scale);
  /* the child's first baseline on the box's, which is the font's top under the padding */
  child_line (self, &ascent, &line);
  cal_text_line (self->text_size, scale, &text_baseline, NULL);
  y = label_margin (scale) + self->padding + text_baseline - ascent;
  if (GTK_IS_TEXT (self->child))
    y -= (child_height - line) / 2;     /* GtkText centres its line in its height */
  gtk_widget_allocate (self->child, child_width, child_height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) self->padding, (float) y)));
}

GskPath *
cal_outline_path (double x, double y, double w, double h, double r, double gap_start, double gap_end)
{
  GskPathBuilder *builder = gsk_path_builder_new ();
  float k = 0.5523f;           /* a quarter circle's cubic */

  gsk_path_builder_move_to (builder, (float) (gap_end > gap_start ? gap_end : x + r), (float) y);
  gsk_path_builder_line_to (builder, (float) (x + w - r), (float) y);
  gsk_path_builder_cubic_to (builder, (float) (x + w - r + r * k), (float) y, (float) (x + w), (float) (y + r - r * k),
                             (float) (x + w), (float) (y + r));
  gsk_path_builder_line_to (builder, (float) (x + w), (float) (y + h - r));
  gsk_path_builder_cubic_to (builder, (float) (x + w), (float) (y + h - r + r * k), (float) (x + w - r + r * k),
                             (float) (y + h), (float) (x + w - r), (float) (y + h));
  gsk_path_builder_line_to (builder, (float) (x + r), (float) (y + h));
  gsk_path_builder_cubic_to (builder, (float) (x + r - r * k), (float) (y + h), (float) x,
                             (float) (y + h - r + r * k), (float) x, (float) (y + h - r));
  gsk_path_builder_line_to (builder, (float) x, (float) (y + r));
  gsk_path_builder_cubic_to (builder, (float) x, (float) (y + r - r * k), (float) (x + r - r * k), (float) y,
                             (float) (x + r), (float) y);
  if (gap_end > gap_start)
    gsk_path_builder_line_to (builder, (float) gap_start, (float) y);
  else
    gsk_path_builder_close (builder);
  return gsk_path_builder_free_to_path (builder);
}

static PangoLayout *
label_layout (CalOutlinedField *self, double size, double spacing)
{
  PangoLayout *layout = cal_text_layout (GTK_WIDGET (self), self->label, size, 700);
  PangoAttrList *attrs = pango_attr_list_new ();

  pango_attr_list_insert (attrs, pango_attr_letter_spacing_new ((int) round (spacing * PANGO_SCALE)));
  pango_layout_set_attributes (layout, attrs);
  pango_attr_list_unref (attrs);
  return layout;
}

static GdkRGBA
mix (GdkRGBA a, GdkRGBA b, double f)
{
  return (GdkRGBA) { a.red + (b.red - a.red) * f, a.green + (b.green - a.green) * f, a.blue + (b.blue - a.blue) * f,
                     a.alpha + (b.alpha - a.alpha) * f };
}

static void
cal_outlined_field_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalOutlinedField *self = CAL_OUTLINED_FIELD (widget);
  int scale = neo_device_scale (widget);
  double width = gtk_widget_get_width (widget), top = label_margin (scale), f = self->floated;
  gboolean focused = self->focused;
  double stroke = focused ? 2 : 1, gap_start = 0, gap_end = 0, expanded_baseline;
  double size = self->text_size + (LABEL_SIZE - self->text_size) * f;
  g_autoptr(PangoLayout) label = label_layout (self, size, self->letter_spacing
                                                           + (LABEL_SPACING - self->letter_spacing) * f);
  g_autoptr(GskPath) path = NULL;
  g_autoptr(GskStroke) pen = gsk_stroke_new ((float) stroke);
  GdkRGBA outline = cal_argb (focused ? CAL_PRIMARY : CAL_OUTLINE);
  GdkRGBA color = mix (cal_argb (CAL_ON_SURFACE_VARIANT), cal_argb (focused ? CAL_PRIMARY : CAL_ON_SURFACE_VARIANT),
                       f);

  if (self->up)
    {
      g_autoptr(PangoLayout) collapsed = label_layout (self, LABEL_SIZE, LABEL_SPACING);
      PangoRectangle logical;

      pango_layout_get_extents (collapsed, NULL, &logical);
      gap_start = self->padding - LABEL_CUTOUT;
      gap_end = self->padding + logical.width / (double) PANGO_SCALE + LABEL_CUTOUT;
    }
  path = cal_outline_path (stroke / 2, top + stroke / 2, width - stroke, self->box_height - stroke, CORNER,
                           gap_start, gap_end);
  gtk_snapshot_append_stroke (snapshot, path, pen, &outline);

  /* from the text's baseline to the edge's label's (its top the widget's, the label margin over the box) */
  cal_text_line (self->text_size, scale, &expanded_baseline, NULL);
  expanded_baseline += top + self->padding;
  cal_text_draw (snapshot, label, self->padding,
                 expanded_baseline + (LABEL_SIZE * ASCENT - expanded_baseline) * f, 0, &color);

  gtk_widget_snapshot_child (widget, self->child, snapshot);
}

static gboolean
is_empty (CalOutlinedField *self)
{
  if (GTK_IS_TEXT_VIEW (self->child))
    return gtk_text_buffer_get_char_count (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->child))) == 0;
  return !*gtk_editable_get_text (GTK_EDITABLE (self->child));
}

static void
on_float (NeoAnim *anim, double value, gpointer data)
{
  CAL_OUTLINED_FIELD (data)->floated = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

/* the label goes up with the focus or a text, down with neither */
static void
restate (CalOutlinedField *self, gboolean animate)
{
  gboolean up = self->focused || !is_empty (self);

  if (up != self->up || !animate)
    {
      self->up = up;
      if (animate)
        neo_anim_start (self->anim, self->floated, up ? 1 : 0, LABEL_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
      else
        {
          neo_anim_stop (self->anim);
          self->floated = up ? 1 : 0;
        }
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_focus (GtkEventControllerFocus *focus, CalOutlinedField *self)
{
  self->focused = gtk_event_controller_focus_contains_focus (focus);
  restate (self, TRUE);
}

static void
on_changed (CalOutlinedField *self)
{
  restate (self, TRUE);
}

/* a tap on the box's padding focuses the text */
static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalOutlinedField *self)
{
  if (!gtk_widget_has_focus (self->child))
    gtk_widget_grab_focus (self->child);
}

static void
cal_outlined_field_dispose (GObject *object)
{
  CalOutlinedField *self = CAL_OUTLINED_FIELD (object);

  g_clear_pointer (&self->child, gtk_widget_unparent);
  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->label, g_free);
  G_OBJECT_CLASS (cal_outlined_field_parent_class)->dispose (object);
}

static void
cal_outlined_field_class_init (CalOutlinedFieldClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_outlined_field_dispose;
  widget_class->get_request_mode = cal_outlined_field_get_request_mode;
  widget_class->measure = cal_outlined_field_measure;
  widget_class->size_allocate = cal_outlined_field_allocate;
  widget_class->snapshot = cal_outlined_field_snapshot;
  gtk_widget_class_set_css_name (widget_class, "cal-outlined-field");
}

static void
cal_outlined_field_init (CalOutlinedField *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->anim = neo_anim_new (GTK_WIDGET (self), on_float, self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
cal_outlined_field_new (GtkWidget *child, const char *label, double text_size, double letter_spacing, double padding)
{
  CalOutlinedField *self = g_object_new (CAL_TYPE_OUTLINED_FIELD, NULL);
  GtkEventController *focus = gtk_event_controller_focus_new ();

  g_return_val_if_fail (GTK_IS_TEXT (child) || GTK_IS_TEXT_VIEW (child), GTK_WIDGET (self));
  self->child = child;
  self->label = g_strdup (label);
  self->text_size = text_size;
  self->letter_spacing = letter_spacing;
  self->padding = padding;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  gtk_accessible_update_property (GTK_ACCESSIBLE (child), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (focus, "enter", G_CALLBACK (on_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_focus), self);
  gtk_widget_add_controller (child, focus);
  if (GTK_IS_TEXT_VIEW (child))
    g_signal_connect_object (gtk_text_view_get_buffer (GTK_TEXT_VIEW (child)), "changed", G_CALLBACK (on_changed),
                             self, G_CONNECT_SWAPPED);
  else
    g_signal_connect_object (child, "changed", G_CALLBACK (on_changed), self, G_CONNECT_SWAPPED);
  restate (self, FALSE);
  return GTK_WIDGET (self);
}
