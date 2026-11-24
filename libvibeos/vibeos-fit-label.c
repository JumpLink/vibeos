/* VibeosFitLabel: see vibeos-fit-label.h. */
#include "vibeos-fit-label.h"

#include <math.h>
#include <string.h>

/* the scale goes down in steps this big from the one at which every word fits, as autoSize goes down in steps of its
 * granularity */
#define STEP 0.02

#define SOFT_HYPHEN "\302\255"     /* U+00AD */

struct _VibeosFitLabel {
  GtkWidget  parent;
  GtkWidget *label;
  char      *text;            /* the label's text as it was given, soft hyphens and all */
  gboolean   hyphens_shown;   /* the label shows them now (at the smallest scale) */
  char      *shown;           /* the text set here for the label to show, with or without them */
  guint      swap;            /* an idle that puts them in or takes them out */
};

G_DEFINE_FINAL_TYPE (VibeosFitLabel, vibeos_fit_label, GTK_TYPE_WIDGET)

/* ---- the scale ------------------------------------------------------------------------------------------------- */

/* `text` without its soft hyphens, and `attrs` (may be NULL) moved with the letters: a copy of each */
static char *
without_hyphens (const char *text, PangoAttrList *attrs, PangoAttrList **attrs_out)
{
  GString *whole = g_string_new (text);
  const char *at;

  if (attrs_out)
    *attrs_out = attrs ? pango_attr_list_copy (attrs) : NULL;
  /* from the end, so the positions before each one stay as they are */
  for (gssize end = whole->len; (at = g_strrstr_len (whole->str, end, SOFT_HYPHEN)); )
    {
      end = at - whole->str;
      g_string_erase (whole, end, strlen (SOFT_HYPHEN));
      if (attrs_out && *attrs_out)
        pango_attr_list_update (*attrs_out, end, strlen (SOFT_HYPHEN), 0);
    }
  return g_string_free (whole, FALSE);
}

/* the scale, from 1 down to `minimum`, at which `layout`'s text fits `width` px: no word wider than the width, and
 * at most `lines` lines (0 or less: any number); `minimum` when not even that is enough. Soft hyphens play no part:
 * a translation puts them where a word may break once it is drawn as small as it can be, not before (VibeosFitLabel
 * shows them only then). `layout` is not changed. Apart from any widget, so the tests measure it without a display
 * (test-fit.c) */
double _vibeos_fit_label_scale (PangoLayout *layout, int width, int lines, double minimum);

double
_vibeos_fit_label_scale (PangoLayout *layout, int width, int lines, double minimum)
{
  g_autoptr (PangoLayout) copy = NULL;
  PangoRectangle logical;
  double scale, words;

  if (width <= 0)
    return 1;
  copy = pango_layout_copy (layout);
  pango_layout_set_ellipsize (copy, PANGO_ELLIPSIZE_NONE);
  pango_layout_set_height (copy, -1);
  pango_layout_set_wrap (copy, PANGO_WRAP_WORD);
  if (strstr (pango_layout_get_text (layout), SOFT_HYPHEN))
    {
      PangoAttrList *attrs;
      g_autofree char *text = without_hyphens (pango_layout_get_text (layout), pango_layout_get_attributes (layout),
                                               &attrs);

      pango_layout_set_text (copy, text, -1);
      pango_layout_set_attributes (copy, attrs);
      g_clear_pointer (&attrs, pango_attr_list_unref);
    }
  /* wrapped at every chance, the widest line is the widest word (GtkLabel's minimum width, the same way) */
  pango_layout_set_width (copy, 0);
  pango_layout_get_pixel_extents (copy, NULL, &logical);
  words = logical.width > width ? (double) width / logical.width : 1;
  if (words < minimum)
    return minimum;
  if (lines <= 0)
    return words;
  /* the largest scale, from there, at which the text takes few enough lines */
  for (scale = words; scale >= minimum; scale -= STEP)
    {
      pango_layout_set_width (copy, (int) floor (width / scale) * PANGO_SCALE);
      if (pango_layout_get_line_count (copy) <= lines)
        return scale;
    }
  return minimum;
}

static double
scale_at (VibeosFitLabel *self, int width)
{
  GtkLabel *label = GTK_LABEL (self->label);

  return _vibeos_fit_label_scale (gtk_label_get_layout (label), width, gtk_label_get_lines (label),
                                  VIBEOS_FIT_LABEL_MIN_SCALE);
}

/* ---- soft hyphens ---------------------------------------------------------------------------------------------- */

/* a plain label shows its soft hyphens only at the smallest scale, the one place a word may break at them; with
 * markup or attributes of the app's its text is not touched, and they stay */
static gboolean
can_swap (VibeosFitLabel *self)
{
  GtkLabel *label = GTK_LABEL (self->label);

  return self->text && strstr (self->text, SOFT_HYPHEN) && !gtk_label_get_use_markup (label) &&
         !gtk_label_get_attributes (label);
}

static void
show_hyphens (VibeosFitLabel *self, gboolean shown)
{
  g_free (self->shown);
  self->shown = shown ? g_strdup (self->text) : without_hyphens (self->text, NULL, NULL);
  self->hyphens_shown = shown;
  gtk_label_set_label (GTK_LABEL (self->label), self->shown);
}

static gboolean
swap_hyphens (gpointer data)
{
  VibeosFitLabel *self = data;

  self->swap = 0;
  show_hyphens (self, !self->hyphens_shown);
  return G_SOURCE_REMOVE;
}

/* the app set the label's text: kept as it is, and shown without soft hyphens until the scale is at its smallest. The
 * text set here comes back too (the label's notification can come once the setting is over), and is let be */
static void
on_label_changed (GtkLabel *label, GParamSpec *pspec, VibeosFitLabel *self)
{
  if (self->shown && !g_strcmp0 (gtk_label_get_label (label), self->shown))
    return;
  g_clear_pointer (&self->shown, g_free);
  g_clear_handle_id (&self->swap, g_source_remove);
  g_free (self->text);
  self->text = g_strdup (gtk_label_get_label (label));
  self->hyphens_shown = TRUE;
  if (can_swap (self))
    show_hyphens (self, FALSE);
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

static void
vibeos_fit_label_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                          int *minimum_baseline, int *natural_baseline)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);
  double scale = 1;
  int width = -1;

  *minimum = *natural = 0;
  if (!gtk_widget_should_layout (self->label))
    return;
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      /* the minimum is the label's at its smallest, where it breaks words */
      gtk_widget_measure (self->label, orientation, -1, minimum, natural, NULL, NULL);
      *minimum = (int) ceil (*minimum * VIBEOS_FIT_LABEL_MIN_SCALE);
      return;
    }
  /* the height, for a width, of the label drawn at the scale it takes there */
  if (for_size >= 0)
    {
      scale = scale_at (self, for_size);
      width = (int) ceil (for_size / scale);
    }
  gtk_widget_measure (self->label, orientation, width, minimum, natural, NULL, NULL);
  *minimum = (int) ceil (*minimum * scale);
  *natural = (int) ceil (*natural * scale);
}

static void
vibeos_fit_label_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);
  gboolean shown;
  double scale;

  if (!gtk_widget_should_layout (self->label))
    return;
  /* the label laid out in as much more room as it is drawn smaller */
  scale = scale_at (self, width);
  gtk_widget_allocate (self->label, (int) ceil (width / scale), (int) ceil (height / scale), -1,
                       scale < 1 ? gsk_transform_scale (NULL, scale, scale) : NULL);
  /* its soft hyphens in at the smallest scale and out above it, once this layout is done */
  shown = scale <= VIBEOS_FIT_LABEL_MIN_SCALE;
  if (!can_swap (self) || shown == self->hyphens_shown)
    g_clear_handle_id (&self->swap, g_source_remove);
  else if (!self->swap)
    self->swap = g_idle_add (swap_hyphens, self);
}

static GtkSizeRequestMode
vibeos_fit_label_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
vibeos_fit_label_compute_expand (GtkWidget *widget, gboolean *hexpand, gboolean *vexpand)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);

  *hexpand = gtk_widget_compute_expand (self->label, GTK_ORIENTATION_HORIZONTAL);
  *vexpand = gtk_widget_compute_expand (self->label, GTK_ORIENTATION_VERTICAL);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_fit_label_dispose (GObject *object)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (object);

  g_clear_handle_id (&self->swap, g_source_remove);
  if (self->label)
    g_signal_handlers_disconnect_by_data (self->label, self);
  g_clear_pointer (&self->label, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_fit_label_parent_class)->dispose (object);
}

static void
vibeos_fit_label_finalize (GObject *object)
{
  g_free (VIBEOS_FIT_LABEL (object)->text);
  g_free (VIBEOS_FIT_LABEL (object)->shown);
  G_OBJECT_CLASS (vibeos_fit_label_parent_class)->finalize (object);
}

static void
vibeos_fit_label_class_init (VibeosFitLabelClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_fit_label_dispose;
  G_OBJECT_CLASS (klass)->finalize = vibeos_fit_label_finalize;
  widget_class->measure = vibeos_fit_label_measure;
  widget_class->size_allocate = vibeos_fit_label_size_allocate;
  widget_class->get_request_mode = vibeos_fit_label_get_request_mode;
  widget_class->compute_expand = vibeos_fit_label_compute_expand;
  gtk_widget_class_set_css_name (widget_class, "vibeos-fit-label");
}

static void
vibeos_fit_label_init (VibeosFitLabel *self)
{
}

GtkWidget *
vibeos_fit_label_new (GtkWidget *label)
{
  VibeosFitLabel *self;

  g_return_val_if_fail (GTK_IS_LABEL (label), NULL);
  self = g_object_new (VIBEOS_TYPE_FIT_LABEL, NULL);
  self->label = label;
  /* at words; a word breaks only where the scale is at its smallest, the only place one is still too wide */
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_parent (label, GTK_WIDGET (self));
  g_signal_connect (label, "notify::label", G_CALLBACK (on_label_changed), self);
  on_label_changed (GTK_LABEL (label), NULL, self);
  return GTK_WIDGET (self);
}

GtkWidget *
vibeos_fit_label_get_label (VibeosFitLabel *self)
{
  g_return_val_if_fail (VIBEOS_IS_FIT_LABEL (self), NULL);
  return self->label;
}
