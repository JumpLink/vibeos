/* VibeosListRow: see vibeos-list-row.h.
 *
 * The row lays out its parts by hand. Its height is exact (lines of text have heights that are not whole px): it
 * draws its card that tall, places its parts by it, and asks for it rounded up. Its children, bottom first: the
 * tap target with the ripple (a VibeosPressable with nothing in it), then the parts. Only the switch takes taps of
 * its own; a tap anywhere else falls through to the target. */
#include "vibeos-list-row.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"
#include "vibeos-switch.h"
#include "vibeos-text.h"

#include <math.h>

#define PAD               16    /* over the first line of text and under the last */
#define ONE_LINE_H        72    /* the least height: a row of one line */
#define TITLE_LINE        (68 / 3.0)
#define TITLE_MORE        (61 / 3.0)   /* each further line of a wrapped title */
#define SUMMARY_LINE      20
#define SUMMARY_MORE      (53 / 3.0)   /* each further line of a wrapped summary */
#define TITLE_SIZE        16    /* the title's text and the summary's, in sp (vibeos.css): the lines above are for */
#define SUMMARY_SIZE      14    /* these at a font size of 1, and grow with them */
#define TEXT_PLAIN        16    /* the text's inset with nothing at the start */
#define TEXT_AFTER_START  68    /* and with something there */
#define ICON              24
#define ICON_X            20
#define CIRCLE            40
#define CIRCLE_X          16
#define END_PAD           16    /* the end's inset from the card's edge */
#define TEXT_GAP          16    /* between the text and what is at the end */
#define DIVIDER_H         32
#define DIVIDER_GAP       12    /* between the divider and the switch */
#define OUTER_RADIUS      20
#define SELECTED_RADIUS   28
#define HIGHLIGHT_RADIUS  20

typedef enum { START_NONE, START_ICON, START_CIRCLE, START_AVATAR } Start;
typedef enum { END_NONE, END_SWITCH, END_SPLIT, END_ICON, END_VALUE } End;

struct _VibeosListRow {
  GtkWidget  parent;
  GtkWidget *target;            /* the tap target, with the ripple */
  GtkWidget *start;             /* the icon or the picture, or NULL */
  GtkWidget *initials;          /* an avatar's initials, or NULL */
  GtkWidget *title, *summary;
  GtkWidget *chevron;           /* a split row's, before the divider */
  GtkWidget *end;               /* the switch, the chevron (or another icon) or the value, or NULL */
  Start      start_kind;
  End        end_kind;
  GdkRGBA    start_fill;
  GdkRGBA    glyph;             /* an icon circle's icon, drawn in this */
  char      *glyph_name;
  int        text_inset;        /* -1: from the start */
  double     top_radius, bottom_radius;
  double     exact_h;           /* at the width allocated last */
  gboolean   selected;
};

G_DEFINE_FINAL_TYPE (VibeosListRow, vibeos_list_row, GTK_TYPE_WIDGET)

enum { CLICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- measuring ------------------------------------------------------------------------------------------------- */

static int
natural_width (GtkWidget *widget)
{
  int min, nat;

  if (!widget || !gtk_widget_get_visible (widget))
    return 0;
  gtk_widget_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  return nat;
}

static int
natural_height (GtkWidget *widget, int width)
{
  int min, nat;

  gtk_widget_measure (widget, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  return nat;
}

/* how many lines a label's text takes at `width` */
static int
line_count (GtkWidget *label, int width)
{
  const char *text = gtk_label_get_text (GTK_LABEL (label));
  PangoLayout *layout;
  int lines;

  if (!gtk_widget_get_visible (label) || !text || !*text)
    return 0;
  layout = gtk_widget_create_pango_layout (label, text);
  pango_layout_set_width (layout, MAX (width, 1) * PANGO_SCALE);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  lines = pango_layout_get_line_count (layout);
  g_object_unref (layout);
  return lines;
}

static double
text_x (VibeosListRow *self)
{
  if (self->text_inset >= 0)
    return self->text_inset;
  return self->start_kind == START_NONE ? TEXT_PLAIN : TEXT_AFTER_START;
}

/* the switch's left edge, and the divider's on a split row */
static double
switch_x (VibeosListRow *self, int width)
{
  return width - END_PAD - natural_width (self->end);
}

static double
divider_x (VibeosListRow *self, int width)
{
  return switch_x (self, width) - DIVIDER_GAP - 1;
}

/* where the text has to stop */
static double
text_end (VibeosListRow *self, int width)
{
  switch (self->end_kind)
    {
    case END_SWITCH:
      return switch_x (self, width) - TEXT_GAP;
    case END_SPLIT:
      return divider_x (self, width) - ICON - TEXT_GAP;
    case END_ICON:
      return width - END_PAD - ICON - TEXT_GAP;
    case END_VALUE:
      return width - END_PAD - natural_width (self->end) - TEXT_GAP;
    case END_NONE:
    default:
      return width - END_PAD;
    }
}

static int
text_width (VibeosListRow *self, int width)
{
  return MAX (1, (int) floor (text_end (self, width) - text_x (self)));
}

/* the height the title's lines and the summary's take */
static void
text_block (VibeosListRow *self, int width, double *title_h, double *summary_h)
{
  int w = text_width (self, width);
  int title = MAX (1, line_count (self->title, w)), summary = line_count (self->summary, w);

  *title_h = (TITLE_LINE + (title - 1) * TITLE_MORE) * (vibeos_sp (TITLE_SIZE) / TITLE_SIZE);
  *summary_h = summary ? (SUMMARY_LINE + (summary - 1) * SUMMARY_MORE) * (vibeos_sp (SUMMARY_SIZE) / SUMMARY_SIZE) : 0;
}

double
vibeos_list_row_get_exact_height (VibeosListRow *self, int width)
{
  double title_h, summary_h;

  text_block (self, width, &title_h, &summary_h);
  return MAX (ONE_LINE_H, PAD + title_h + summary_h + PAD);
}

/* the height depends on the width (its lines wrap): without this GTK measures it for no width, at its natural one */
static GtkSizeRequestMode
vibeos_list_row_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
vibeos_list_row_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                         int *min_baseline, int *nat_baseline)
{
  VibeosListRow *self = VIBEOS_LIST_ROW (widget);
  int text = MAX (natural_width (self->title), natural_width (self->summary));
  int end = self->end_kind == END_NONE ? 0 : TEXT_GAP + natural_width (self->end)
            + (self->end_kind == END_SPLIT ? ICON + DIVIDER_GAP + 1 : 0);
  int natural = (int) text_x (self) + text + end + END_PAD;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *min = (int) text_x (self) + END_PAD;
      *nat = MAX (*min, natural);
      return;
    }
  *min = *nat = (int) ceil (vibeos_list_row_get_exact_height (self, for_size > 0 ? for_size : natural));
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

static void
put (GtkWidget *child, double x, double y, int width, int height)
{
  gtk_widget_allocate (child, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
vibeos_list_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosListRow *self = VIBEOS_LIST_ROW (widget);
  double h, title_h, summary_h, x = text_x (self), y;
  int w = text_width (self, width);

  text_block (self, width, &title_h, &summary_h);
  h = MAX (ONE_LINE_H, PAD + title_h + summary_h + PAD);
  /* the height asked for is the exact one rounded up: that is drawn exact; a parent that gives more gets it filled */
  h = self->exact_h = height - h < 1 ? h : height;

  put (self->target, 0, 0, self->end_kind == END_SPLIT ? (int) divider_x (self, width) : width, (int) ceil (h));

  if (self->start_kind == START_ICON)
    put (self->start, ICON_X, (h - ICON) / 2, ICON, ICON);
  else if (self->start)
    put (self->start, CIRCLE_X, (h - CIRCLE) / 2, CIRCLE, CIRCLE);
  if (self->initials)
    put (self->initials, CIRCLE_X, (h - CIRCLE) / 2, CIRCLE, CIRCLE);

  /* the lines centred in the row (on one of two lines and more, that is under the padding) */
  y = (h - title_h - summary_h) / 2;
  put (self->title, x, y, w, MAX ((int) ceil (title_h), natural_height (self->title, w)));
  if (gtk_widget_get_visible (self->summary))
    put (self->summary, x, y + title_h, w, MAX ((int) ceil (summary_h), natural_height (self->summary, w)));

  switch (self->end_kind)
    {
    case END_SPLIT:
      put (self->chevron, divider_x (self, width) - ICON, (h - ICON) / 2, ICON, ICON);
      G_GNUC_FALLTHROUGH;
    case END_SWITCH:
    case END_VALUE:
      {
        int end_w = natural_width (self->end), end_h = natural_height (self->end, end_w);

        put (self->end, width - END_PAD - end_w, (h - end_h) / 2, end_w, end_h);
        break;
      }
    case END_ICON:
      put (self->end, width - END_PAD - ICON, (h - ICON) / 2, ICON, ICON);
      break;
    case END_NONE:
    default:
      break;
    }
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

static GskRoundedRect
card_shape (VibeosListRow *self, double width, double height)
{
  GskRoundedRect shape;
  graphene_size_t top = GRAPHENE_SIZE_INIT (self->top_radius, self->top_radius);
  graphene_size_t bottom = GRAPHENE_SIZE_INIT (self->bottom_radius, self->bottom_radius);

  gsk_rounded_rect_init (&shape, &GRAPHENE_RECT_INIT (0, 0, width, height), &top, &top, &bottom, &bottom);
  return shape;
}

static void
fill_rounded (GtkSnapshot *snapshot, const graphene_rect_t *rect, double radius, const GdkRGBA *color)
{
  GskRoundedRect rounded;

  gsk_rounded_rect_init_from_rect (&rounded, rect, MIN (radius, MIN (rect->size.width, rect->size.height) / 2));
  gtk_snapshot_push_rounded_clip (snapshot, &rounded);
  gtk_snapshot_append_color (snapshot, color, rect);
  gtk_snapshot_pop (snapshot);
}

static void
vibeos_list_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosListRow *self = VIBEOS_LIST_ROW (widget);
  double w = gtk_widget_get_width (widget);
  double h = self->exact_h > 0 ? self->exact_h : gtk_widget_get_height (widget);
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, w, h);
  GskRoundedRect card = card_shape (self, w, h);
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_BRIGHT);

  /* selected, the row is a pill in the colour of the pane beside it instead of its card */
  if (self->selected)
    {
      gsk_rounded_rect_init_from_rect (&card, &box, MIN (SELECTED_RADIUS, h / 2));
      fill = vibeos_color (VIBEOS_SURFACE_CONTAINER);
    }
  gtk_snapshot_push_rounded_clip (snapshot, &card);
  gtk_snapshot_append_color (snapshot, &fill, &box);
  /* the press highlight, kept on while the page it opens comes */
  if (gtk_widget_has_css_class (widget, "vibeos-highlighted"))
    {
      GdkRGBA tint = vibeos_color_alpha (VIBEOS_ON_SURFACE, 0.1f);
      fill_rounded (snapshot, &box, HIGHLIGHT_RADIUS, &tint);
    }
  gtk_widget_snapshot_child (widget, self->target, snapshot);
  gtk_snapshot_pop (snapshot);

  if (self->start_kind == START_CIRCLE || self->start_kind == START_AVATAR)
    {
      GtkWidget *holder = self->start ? self->start : self->initials;
      graphene_rect_t circle;
      GskRoundedRect round;

      if (!holder || !gtk_widget_compute_bounds (holder, widget, &circle))
        circle = GRAPHENE_RECT_INIT (CIRCLE_X, (h - CIRCLE) / 2, CIRCLE, CIRCLE);
      gsk_rounded_rect_init_from_rect (&round, &circle, CIRCLE / 2.0);
      gtk_snapshot_push_rounded_clip (snapshot, &round);
      gtk_snapshot_append_color (snapshot, &self->start_fill, &circle);
      if (self->start_kind == START_CIRCLE)
        /* the icon in the circle's own colour, which no stylesheet knows */
        vibeos_snapshot_icon (snapshot, widget, self->glyph_name, circle.origin.x + (CIRCLE - ICON) / 2.0,
                              circle.origin.y + (CIRCLE - ICON) / 2.0, ICON, &self->glyph);
      else if (holder)
        gtk_widget_snapshot_child (widget, holder, snapshot);
      gtk_snapshot_pop (snapshot);
    }
  else if (self->start)
    gtk_widget_snapshot_child (widget, self->start, snapshot);

  gtk_widget_snapshot_child (widget, self->title, snapshot);
  gtk_widget_snapshot_child (widget, self->summary, snapshot);
  if (self->end_kind == END_SPLIT)
    {
      GdkRGBA line = vibeos_color (VIBEOS_OUTLINE_VARIANT);

      gtk_widget_snapshot_child (widget, self->chevron, snapshot);
      gtk_snapshot_append_color (snapshot, &line,
                                 &GRAPHENE_RECT_INIT (divider_x (self, (int) w), (h - DIVIDER_H) / 2, 1, DIVIDER_H));
    }
  if (self->end)
    gtk_widget_snapshot_child (widget, self->end, snapshot);
}

/* ---- the parts ------------------------------------------------------------------------------------------------- */

static void
on_target_clicked (VibeosPressable *target, VibeosListRow *self)
{
  if (self->end_kind == END_SWITCH)
    vibeos_switch_set_active (VIBEOS_SWITCH (self->end), !vibeos_switch_get_active (VIBEOS_SWITCH (self->end)));
  g_signal_emit (self, signals[CLICKED], 0);
}

/* a part that only shows: a tap on it is the row's */
static GtkWidget *
part (VibeosListRow *self, GtkWidget *widget)
{
  gtk_widget_set_can_target (widget, FALSE);
  gtk_widget_set_parent (widget, GTK_WIDGET (self));
  return widget;
}

void
vibeos_list_row_clear_start (VibeosListRow *self)
{
  g_clear_pointer (&self->start, gtk_widget_unparent);
  g_clear_pointer (&self->initials, gtk_widget_unparent);
  self->start_kind = START_NONE;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_list_row_set_icon (VibeosListRow *self, const char *icon_name)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  vibeos_list_row_clear_start (self);
  gtk_image_set_pixel_size (GTK_IMAGE (image), ICON);
  gtk_widget_add_css_class (image, "vibeos-list-row-icon");
  self->start = part (self, image);
  self->start_kind = START_ICON;
}

void
vibeos_list_row_set_icon_circle (VibeosListRow *self, const char *icon_name, const GdkRGBA *fill,
                                 const GdkRGBA *glyph)
{
  /* the image only holds the circle's place; the row draws the circle and the icon itself */
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  vibeos_list_row_clear_start (self);
  gtk_image_set_pixel_size (GTK_IMAGE (image), ICON);
  gtk_widget_add_css_class (image, "vibeos-list-row-icon-circle");
  self->start = part (self, image);
  self->start_kind = START_CIRCLE;
  self->start_fill = fill ? *fill : vibeos_color (VIBEOS_PRIMARY_CONTAINER);
  self->glyph = glyph ? *glyph : vibeos_color (VIBEOS_ON_PRIMARY_CONTAINER);
  g_free (self->glyph_name);
  self->glyph_name = g_strdup (icon_name);
}

void
vibeos_list_row_set_avatar (VibeosListRow *self, GdkPaintable *picture, const char *initials, const GdkRGBA *fill)
{
  vibeos_list_row_clear_start (self);
  self->start_kind = START_AVATAR;
  self->start_fill = fill ? *fill : vibeos_color (VIBEOS_PRIMARY_CONTAINER);
  if (picture)
    {
      GtkWidget *image = gtk_image_new_from_paintable (picture);

      gtk_image_set_pixel_size (GTK_IMAGE (image), CIRCLE);
      self->start = part (self, image);
      return;
    }
  self->initials = part (self, gtk_label_new (initials));
  gtk_widget_add_css_class (self->initials, "vibeos-list-row-initials");
}

void
vibeos_list_row_set_text_inset (VibeosListRow *self, int inset)
{
  self->text_inset = inset;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_list_row_clear_end (VibeosListRow *self)
{
  g_clear_pointer (&self->end, gtk_widget_unparent);
  g_clear_pointer (&self->chevron, gtk_widget_unparent);
  self->end_kind = END_NONE;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

GtkWidget *
vibeos_list_row_set_switch (VibeosListRow *self, gboolean split)
{
  GtkWidget *toggle = vibeos_switch_new ();

  vibeos_list_row_clear_end (self);
  if (split)
    {
      self->chevron = part (self, gtk_image_new_from_icon_name ("go-next-symbolic"));
      gtk_image_set_pixel_size (GTK_IMAGE (self->chevron), ICON);
      gtk_widget_add_css_class (self->chevron, "vibeos-list-row-icon");
    }
  self->end = toggle;
  gtk_widget_set_parent (toggle, GTK_WIDGET (self));
  self->end_kind = split ? END_SPLIT : END_SWITCH;
  return toggle;
}

void
vibeos_list_row_set_end_icon (VibeosListRow *self, const char *icon_name)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  vibeos_list_row_clear_end (self);
  gtk_image_set_pixel_size (GTK_IMAGE (image), ICON);
  gtk_widget_add_css_class (image, "vibeos-list-row-icon");
  self->end = part (self, image);
  self->end_kind = END_ICON;
}

void
vibeos_list_row_set_chevron (VibeosListRow *self)
{
  vibeos_list_row_set_end_icon (self, "go-next-symbolic");
}

void
vibeos_list_row_set_value (VibeosListRow *self, const char *value)
{
  GtkWidget *label = gtk_label_new (value);

  vibeos_list_row_clear_end (self);
  gtk_widget_add_css_class (label, "vibeos-list-row-value");
  self->end = part (self, label);
  self->end_kind = END_VALUE;
}

void
vibeos_list_row_set_title (VibeosListRow *self, const char *title)
{
  gtk_label_set_text (GTK_LABEL (self->title), title ? title : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->target), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_list_row_set_summary (VibeosListRow *self, const char *summary)
{
  gtk_label_set_text (GTK_LABEL (self->summary), summary ? summary : "");
  gtk_widget_set_visible (self->summary, summary && *summary);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_list_row_set_selected (VibeosListRow *self, gboolean selected)
{
  self->selected = !!selected;
  if (selected)
    gtk_widget_add_css_class (self->title, "selected");
  else
    gtk_widget_remove_css_class (self->title, "selected");
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
vibeos_list_row_get_selected (VibeosListRow *self)
{
  return self->selected;
}

void
vibeos_list_row_set_corners (VibeosListRow *self, double top, double bottom)
{
  self->top_radius = top;
  self->bottom_radius = bottom;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_list_row_dispose (GObject *object)
{
  VibeosListRow *self = VIBEOS_LIST_ROW (object);

  vibeos_list_row_clear_start (self);
  vibeos_list_row_clear_end (self);
  g_clear_pointer (&self->title, gtk_widget_unparent);
  g_clear_pointer (&self->summary, gtk_widget_unparent);
  g_clear_pointer (&self->target, gtk_widget_unparent);
  g_clear_pointer (&self->glyph_name, g_free);
  G_OBJECT_CLASS (vibeos_list_row_parent_class)->dispose (object);
}

static void
vibeos_list_row_class_init (VibeosListRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_list_row_dispose;
  widget_class->measure = vibeos_list_row_measure;
  widget_class->get_request_mode = vibeos_list_row_get_request_mode;
  widget_class->size_allocate = vibeos_list_row_size_allocate;
  widget_class->snapshot = vibeos_list_row_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-list-row");

  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static GtkWidget *
text_label (const char *css_class)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_yalign (GTK_LABEL (label), 0.5);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

static void
vibeos_list_row_init (VibeosListRow *self)
{
  self->text_inset = -1;
  self->top_radius = self->bottom_radius = OUTER_RADIUS;

  self->target = vibeos_pressable_new (NULL);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->target), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_set_parent (self->target, GTK_WIDGET (self));
  g_signal_connect (self->target, "clicked", G_CALLBACK (on_target_clicked), self);

  self->title = part (self, text_label ("vibeos-list-row-title"));
  self->summary = part (self, text_label ("vibeos-list-row-summary"));
  gtk_widget_set_visible (self->summary, FALSE);
}

GtkWidget *
vibeos_list_row_new (const char *title, const char *summary)
{
  VibeosListRow *self = g_object_new (VIBEOS_TYPE_LIST_ROW, NULL);

  vibeos_list_row_set_title (self, title);
  vibeos_list_row_set_summary (self, summary);
  return GTK_WIDGET (self);
}
