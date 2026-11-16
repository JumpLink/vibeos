/* VIBEOS_DUMP: a window's widget tree with every widget's place, for measuring the UI.
 *
 * With VIBEOS_DUMP=/path/dump.xml in the environment, a window writes its tree to that file a second after it maps,
 * and again on every SIGUSR1 (with VIBEOS_SHOT set too, the picture and the tree come from the same moment). The file
 * is in the format the reference phone's view dumps use, so that a tool can hold one against the other:
 *
 *   <hierarchy rotation="0" density="3">
 *     <node index="0" class="GtkLabel" text="Network" resource-id="" content-desc="" css="vibeos-list-row-title"
 *           bounds="[252,398][663,466]" dp="84,94,137,22.667" />
 *
 * bounds are in screen pixels: GTK px times the screen's scale, or VIBEOS_SHOT_SCALE on a test display at scale 1.
 * dp is the same box in GTK px, unrounded (x, y, width, height). The class is the widget's type, resource-id its
 * name (gtk_widget_set_name) when it has one, text a label's or an entry's text and content-desc an image's icon.
 * Only widgets on screen are written: those mapped, and inside the window. */
#include "vibeos-style.h"

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <string.h>

/* a number with no more digits than it needs: 94, 22.667 */
static void
append_number (GString *out, double value)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];
  double rounded = round (value * 1000) / 1000;

  if (rounded == floor (rounded))
    g_string_append_printf (out, "%d", (int) rounded);
  else
    {
      char *end;

      g_ascii_formatd (buffer, sizeof buffer, "%.3f", rounded);
      end = buffer + strlen (buffer) - 1;
      while (*end == '0')
        *end-- = '\0';
      g_string_append (out, buffer);
    }
}

static void
append_attribute (GString *out, const char *name, const char *value)
{
  g_autofree char *escaped = g_markup_escape_text (value ? value : "", -1);

  g_string_append_printf (out, " %s=\"%s\"", name, escaped);
}

static char *
css_classes (GtkWidget *widget)
{
  g_auto(GStrv) classes = gtk_widget_get_css_classes (widget);
  const char *name = gtk_widget_get_css_name (widget);
  g_autofree char *joined = g_strjoinv (" ", classes);

  /* the CSS name first: vibeos-list-row, label */
  return *joined ? g_strconcat (name, " ", joined, NULL) : g_strdup (name);
}

static const char *
widget_text (GtkWidget *widget)
{
  if (GTK_IS_LABEL (widget))
    return gtk_label_get_text (GTK_LABEL (widget));
  if (GTK_IS_EDITABLE (widget) && !GTK_IS_TEXT (gtk_widget_get_parent (widget)))
    return gtk_editable_get_text (GTK_EDITABLE (widget));
  return "";
}

static const char *
widget_description (GtkWidget *widget)
{
  if (GTK_IS_IMAGE (widget) && gtk_image_get_storage_type (GTK_IMAGE (widget)) == GTK_IMAGE_ICON_NAME)
    return gtk_image_get_icon_name (GTK_IMAGE (widget));
  return "";
}

static void
dump_widget (GString *out, GtkWidget *window, GtkWidget *widget, int index, int depth, double scale)
{
  graphene_rect_t box;
  int x0, y0, x1, y1, i = 0;
  /* in float, as the box is, so that an edge on a half pixel rounds as it always has */
  float s = (float) scale;
  g_autofree char *css = NULL;

  if (!gtk_widget_get_mapped (widget) || !gtk_widget_compute_bounds (widget, window, &box))
    return;
  x0 = (int) round (box.origin.x * s);
  y0 = (int) round (box.origin.y * s);
  x1 = (int) round ((box.origin.x + box.size.width) * s);
  y1 = (int) round ((box.origin.y + box.size.height) * s);
  css = css_classes (widget);

  g_string_append_printf (out, "%*s<node index=\"%d\"", 2 * depth, "", index);
  append_attribute (out, "class", G_OBJECT_TYPE_NAME (widget));
  append_attribute (out, "text", widget_text (widget));
  append_attribute (out, "resource-id", strcmp (gtk_widget_get_name (widget), G_OBJECT_TYPE_NAME (widget))
                                        ? gtk_widget_get_name (widget) : "");
  append_attribute (out, "content-desc", widget_description (widget));
  append_attribute (out, "css", css);
  g_string_append_printf (out, " bounds=\"[%d,%d][%d,%d]\" dp=\"", x0, y0, x1, y1);
  append_number (out, box.origin.x);
  g_string_append_c (out, ',');
  append_number (out, box.origin.y);
  g_string_append_c (out, ',');
  append_number (out, box.size.width);
  g_string_append_c (out, ',');
  append_number (out, box.size.height);
  g_string_append_c (out, '"');

  if (!gtk_widget_get_first_child (widget))
    {
      g_string_append (out, " />\n");
      return;
    }
  g_string_append (out, ">\n");
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    dump_widget (out, window, child, i++, depth + 1, scale);
  g_string_append_printf (out, "%*s</node>\n", 2 * depth, "");
}

static gboolean
dump (gpointer data)
{
  GtkWidget *window = data;
  const char *path = g_getenv ("VIBEOS_DUMP");
  double scale = vibeos_device_scale (window);
  g_autoptr(GString) out = g_string_new ("<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n");
  g_autoptr(GError) error = NULL;

  g_string_append (out, "<hierarchy rotation=\"0\" density=\"");
  append_number (out, scale);
  g_string_append (out, "\">\n");
  dump_widget (out, window, window, 0, 1, scale);
  g_string_append (out, "</hierarchy>\n");
  if (g_file_set_contents (path, out->str, out->len, &error))
    g_message ("VIBEOS_DUMP: %s, %d×%d", path, (int) round (gtk_widget_get_width (window) * scale),
               (int) round (gtk_widget_get_height (window) * scale));
  else
    g_message ("VIBEOS_DUMP: %s", error->message);
  return G_SOURCE_REMOVE;
}

static void
on_dump_map (GtkWidget *window)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, dump, g_object_ref (window), g_object_unref);
}

static gboolean
on_dump_signal (gpointer data)
{
  GListModel *windows = gtk_window_get_toplevels ();

  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(GtkWidget) window = g_list_model_get_item (windows, i);

      if (gtk_widget_has_css_class (window, "vibeos"))
        dump (window);
    }
  return G_SOURCE_CONTINUE;
}

/* called by vibeos_window_new for every window */
G_GNUC_INTERNAL void
vibeos_dump_attach (GtkWidget *window)
{
  static gboolean signal_added;

  if (!g_getenv ("VIBEOS_DUMP"))
    return;
  g_signal_connect (window, "map", G_CALLBACK (on_dump_map), NULL);
  if (!signal_added)
    g_unix_signal_add (SIGUSR1, on_dump_signal, NULL);
  signal_added = TRUE;
}
