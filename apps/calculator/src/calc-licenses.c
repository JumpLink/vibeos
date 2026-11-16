/* CalcLicenses: see calc-licenses.h. The toolbar is the history's (calc-history.c). */
#include "calc-licenses.h"
#include "calc-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define TOOLBAR        64
#define TITLE_SIZE     22
#define TITLE_X        60
#define ARROW_X        83.5   /* device px: the arrow's centre from the start */
#define ICON_Y         95.5   /* device px: the arrow's centre from the top */
#define BUTTON         48
#define PAGE           "/vibeos/Calculator/licenses.html"

struct _CalcLicenses {
  GtkWidget    parent;
  GtkWidget   *back;
  GtkWidget   *scroll;
  PangoLayout *title;
};

G_DEFINE_FINAL_TYPE (CalcLicenses, calc_licenses, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
calc_licenses_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_licenses_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcLicenses *self = CALC_LICENSES (widget);
  int scale = vibeos_device_scale (widget);

  gtk_widget_allocate (self->back, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (ARROW_X / scale - BUTTON / 2.0,
                                                                            ICON_Y / scale - BUTTON / 2.0)));
  gtk_widget_allocate (self->scroll, width, MAX (0, height - TOOLBAR), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, TOOLBAR)));
}

static void
calc_licenses_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcLicenses *self = CALC_LICENSES (widget);
  int scale = vibeos_device_scale (widget);
  double w = gtk_widget_get_width (widget);
  GdkRGBA surface = vibeos_color (VIBEOS_SURFACE), on_surface = vibeos_color (VIBEOS_ON_SURFACE);
  double px = TITLE_SIZE * scale;
  double top = floor (-CALC_TOP * px), bottom = ceil (CALC_BOTTOM * px);
  double box_top = floor ((TOOLBAR * scale - (bottom - top)) / 2);

  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, w, TOOLBAR));
  if (!self->title)
    self->title = calc_text_layout (widget, _("Open source licenses"), TITLE_SIZE, CALC_WEIGHT_REGULAR);
  calc_text_draw (snapshot, self->title, TITLE_X, (box_top - top) / scale, &on_surface);
  gtk_widget_snapshot_child (widget, self->back, snapshot);
  gtk_widget_snapshot_child (widget, self->scroll, snapshot);
}

static void
on_back (GtkWidget *button, CalcLicenses *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CalcLicenses *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  g_signal_emit (self, signals[CLOSED], 0);
  return TRUE;
}

static void
calc_licenses_dispose (GObject *object)
{
  CalcLicenses *self = CALC_LICENSES (object);

  g_clear_pointer (&self->back, gtk_widget_unparent);
  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  g_clear_object (&self->title);
  G_OBJECT_CLASS (calc_licenses_parent_class)->dispose (object);
}

static void
calc_licenses_class_init (CalcLicensesClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_licenses_dispose;
  widget_class->measure = calc_licenses_measure;
  widget_class->size_allocate = calc_licenses_size_allocate;
  widget_class->snapshot = calc_licenses_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-licenses");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
calc_licenses_init (CalcLicenses *self)
{
}

/* the text between `open` and the next `close` from *pos on, or NULL; moves *pos past it */
static char *
element_text (const char *html, const char **pos, const char *open, const char *close)
{
  const char *start = strstr (*pos, open), *end;

  if (!start)
    return NULL;
  start += strlen (open);
  end = strstr (start, close);
  if (!end)
    return NULL;
  *pos = end + strlen (close);
  return g_strndup (start, end - start);
}

static GtkWidget *
page_label (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

/* the page: licenses.html has one <h3>, one <ul> of <li>s and one <pre>, plain text in each (no entities) */
static GtkWidget *
build_page (void)
{
  g_autoptr (GBytes) bytes = g_resources_lookup_data (PAGE, 0, NULL);
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autofree char *html = NULL;
  const char *pos;
  char *text;

  gtk_widget_add_css_class (page, "calc-licenses-page");
  if (!bytes)
    return page;
  html = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
  pos = html;
  if ((text = element_text (html, &pos, "<h3>", "</h3>"))) {
    gtk_box_append (GTK_BOX (page), page_label (g_strstrip (text), "calc-licenses-h3"));
    g_free (text);
  }
  while ((text = element_text (html, &pos, "<li>", "</li>"))) {
    GtkWidget *item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bullet = gtk_label_new ("•");

    gtk_widget_add_css_class (item, "calc-licenses-li");
    gtk_widget_add_css_class (bullet, "calc-licenses-bullet");
    gtk_label_set_xalign (GTK_LABEL (bullet), 0);
    gtk_box_append (GTK_BOX (item), bullet);
    gtk_box_append (GTK_BOX (item), page_label (g_strstrip (text), "calc-licenses-li-text"));
    gtk_box_append (GTK_BOX (page), item);
    g_free (text);
    if (!strstr (pos, "<li>") || (strstr (pos, "</ul>") && strstr (pos, "</ul>") < strstr (pos, "<li>")))
      break;
  }
  if ((text = element_text (html, &pos, "<pre>", "</pre>"))) {
    /* HTML drops the newline right after <pre>; the one before </pre> ends the last line */
    char *body = text[0] == '\n' ? text + 1 : text;
    size_t len = strlen (body);

    if (len && body[len - 1] == '\n')
      body[len - 1] = '\0';
    gtk_box_append (GTK_BOX (page), page_label (body, "calc-licenses-pre"));
    g_free (text);
  }
  return page;
}

GtkWidget *
calc_licenses_new (void)
{
  CalcLicenses *self = g_object_new (CALC_TYPE_LICENSES, NULL);
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->back = vibeos_icon_button_new ("calc-back-symbolic");
  gtk_widget_add_css_class (self->back, "calc-toolbar-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  /* the shell's back gesture clicks a button named Back */
                                  _("Back"), -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back), self);
  gtk_widget_set_parent (self->back, GTK_WIDGET (self));
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), build_page ());
  gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  return GTK_WIDGET (self);
}
