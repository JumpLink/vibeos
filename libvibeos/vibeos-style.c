/* vibeos-style: the palette, the stylesheet and the window frame. See vibeos-style.h. */
#include "vibeos-style.h"

#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>

/* The dark palette. The CSS name of each role is its enum name in lower case: VIBEOS_ON_SURFACE is @vibeos_on_surface. */
static const struct {
  const char *css;
  guint32     rgb;
} palette[VIBEOS_N_COLORS] = {
  [VIBEOS_PRIMARY]                   = { "vibeos_primary",                   0x9ccfd7 },
  [VIBEOS_ON_PRIMARY]                = { "vibeos_on_primary",                0x0d474e },
  [VIBEOS_PRIMARY_CONTAINER]         = { "vibeos_primary_container",         0x265a61 },
  [VIBEOS_ON_PRIMARY_CONTAINER]      = { "vibeos_on_primary_container",      0xb9ecf4 },
  [VIBEOS_INVERSE_PRIMARY]           = { "vibeos_inverse_primary",           0x34666d },
  [VIBEOS_SECONDARY]                 = { "vibeos_secondary",                 0xb1cbcf },
  [VIBEOS_ON_SECONDARY]              = { "vibeos_on_secondary",              0x2c4448 },
  [VIBEOS_SECONDARY_CONTAINER]       = { "vibeos_secondary_container",       0x273f43 },
  [VIBEOS_ON_SECONDARY_CONTAINER]    = { "vibeos_on_secondary_container",    0xaac4c8 },
  [VIBEOS_TERTIARY]                  = { "vibeos_tertiary",                  0xc9e2ff },
  [VIBEOS_ON_TERTIARY]               = { "vibeos_on_tertiary",               0x315371 },
  [VIBEOS_TERTIARY_CONTAINER]        = { "vibeos_tertiary_container",        0xb3d5f9 },
  [VIBEOS_ON_TERTIARY_CONTAINER]     = { "vibeos_on_tertiary_container",     0x284a67 },
  [VIBEOS_ERROR]                     = { "vibeos_error",                     0xfa746f },
  [VIBEOS_ERROR_CONTAINER]           = { "vibeos_error_container",           0x871f21 },
  [VIBEOS_SURFACE]                   = { "vibeos_surface",                   0x0a0f10 },
  [VIBEOS_SURFACE_CONTAINER_LOWEST]  = { "vibeos_surface_container_lowest",  0x000000 },
  [VIBEOS_SURFACE_CONTAINER_LOW]     = { "vibeos_surface_container_low",     0x0e1415 },
  [VIBEOS_SURFACE_CONTAINER]         = { "vibeos_surface_container",         0x131b1c },
  [VIBEOS_SURFACE_CONTAINER_HIGH]    = { "vibeos_surface_container_high",    0x192122 },
  [VIBEOS_SURFACE_CONTAINER_HIGHEST] = { "vibeos_surface_container_highest", 0x1e2729 },
  [VIBEOS_SURFACE_BRIGHT]            = { "vibeos_surface_bright",            0x242e2f },
  [VIBEOS_ON_SURFACE]                = { "vibeos_on_surface",                0xdde7e9 },
  [VIBEOS_ON_SURFACE_VARIANT]        = { "vibeos_on_surface_variant",        0xa2adaf },
  [VIBEOS_OUTLINE]                   = { "vibeos_outline",                   0x6d7779 },
  [VIBEOS_OUTLINE_VARIANT]           = { "vibeos_outline_variant",           0x404a4b },
  [VIBEOS_INVERSE_SURFACE]           = { "vibeos_inverse_surface",           0xf6fafb },
  [VIBEOS_WHITE]                     = { "vibeos_white",                     0xffffff },
};

GdkRGBA
vibeos_color (VibeosColor color)
{
  return vibeos_color_alpha (color, 1);
}

GdkRGBA
vibeos_color_alpha (VibeosColor color, float alpha)
{
  guint32 rgb = palette[color].rgb;
  return (GdkRGBA) { ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, alpha };
}

GdkRGBA
vibeos_rgba_mix (const GdkRGBA *a, const GdkRGBA *b, float t)
{
  return (GdkRGBA) {
    a->red + (b->red - a->red) * t,
    a->green + (b->green - a->green) * t,
    a->blue + (b->blue - a->blue) * t,
    a->alpha + (b->alpha - a->alpha) * t,
  };
}

void
vibeos_init (void)
{
  static gboolean done;
  if (done)
    return;
  done = TRUE;

  /* the palette first, as @define-color lines, then the stylesheet that uses it */
  GString *css = g_string_new (NULL);
  for (int i = 0; i < VIBEOS_N_COLORS; i++)
    g_string_append_printf (css, "@define-color %s #%06x;\n", palette[i].css, palette[i].rgb);
  GBytes *sheet = g_resources_lookup_data ("/vibeos/libvibeos/vibeos.css", 0, NULL);
  g_string_append_len (css, g_bytes_get_data (sheet, NULL), g_bytes_get_size (sheet));
  g_bytes_unref (sheet);

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css->str);
  /* above the theme (APPLICATION), so GTK's own look never shows through a libvibeos widget */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
  g_string_free (css, TRUE);

  /* the apps are always dark, whatever the system's preference (Theme.DeskClock's parent is a Dark theme) */
#if GTK_CHECK_VERSION (4, 20, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", TRUE, NULL);
#endif
}

/* ---- the window ------------------------------------------------------------------------------------------------ */

/* ---- VIBEOS_SHOT: a window's picture, for checking the UI ---------------------------------------------------------
 *
 * With VIBEOS_SHOT=/path/shot.png in the environment, a window renders itself to that file, at the screen's scale, a
 * second after it maps, and again on every SIGUSR1 (after the test has changed something). The picture is drawn
 * offscreen from the widgets, so it works where a screenshot cannot: behind the lock screen, on a phone nobody
 * unlocks for the test. (A window hidden there gets no frames, though, so its animations stop where they are: for
 * those, run the app on a display of its own, such as a headless compositor, with VIBEOS_SHOT_SCALE=3.) */

static gboolean
shoot (gpointer data)
{
  GtkWidget *window = data;
  const char *path = g_getenv ("VIBEOS_SHOT");
  int width = gtk_widget_get_width (window), height = gtk_widget_get_height (window);
  /* the screen's scale, or VIBEOS_SHOT_SCALE: a window on a test display at scale 1 still makes a phone-sized picture */
  int scale = vibeos_device_scale (window);
  GdkPaintable *paintable = gtk_widget_paintable_new (window);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GskRenderNode *node;

  gtk_snapshot_scale (snapshot, scale, scale);
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node = gtk_snapshot_free_to_node (snapshot);
  if (!gtk_widget_get_mapped (window) || !node)
    g_message ("VIBEOS_SHOT: nothing to draw (the window is %s)", gtk_widget_get_mapped (window) ? "empty" : "not shown");
  else
    {
      GdkTexture *texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node,
                                                         &GRAPHENE_RECT_INIT (0, 0, width * scale, height * scale));
      if (gdk_texture_save_to_png (texture, path))
        g_message ("VIBEOS_SHOT: %s, %d×%d", path, width * scale, height * scale);
      else
        g_message ("VIBEOS_SHOT: cannot write %s", path);
      g_object_unref (texture);
    }
  g_clear_pointer (&node, gsk_render_node_unref);
  g_object_unref (paintable);
  return G_SOURCE_REMOVE;
}

static void
on_shot_map (GtkWidget *window)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, shoot, g_object_ref (window), g_object_unref);
}

static gboolean
on_shot_signal (gpointer data)
{
  GListModel *windows = gtk_window_get_toplevels ();

  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(GtkWidget) window = g_list_model_get_item (windows, i);

      if (gtk_widget_has_css_class (window, "vibeos"))
        shoot (window);
    }
  return G_SOURCE_CONTINUE;
}

GtkWidget *
vibeos_window_new (GtkApplication *app)
{
  static gboolean shots;
  GtkWidget *window = gtk_application_window_new (app);
  gtk_widget_add_css_class (window, "vibeos");
  /* no title bar: the app draws its own app bar, and on the phone the shell shows every window full screen */
  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 760);
  if (g_getenv ("VIBEOS_SHOT"))
    {
      g_signal_connect (window, "map", G_CALLBACK (on_shot_map), NULL);
      if (!shots)
        g_unix_signal_add (SIGUSR1, on_shot_signal, NULL);
      shots = TRUE;
    }
  return window;
}

void
vibeos_window_set_content (GtkWindow *window, GtkWidget *content)
{
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), content);
  g_object_set_data (G_OBJECT (window), "vibeos-overlay", overlay);
  gtk_window_set_child (window, overlay);
}

GtkOverlay *
vibeos_window_get_overlay (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);
  return root ? g_object_get_data (G_OBJECT (root), "vibeos-overlay") : NULL;
}

int
vibeos_device_scale (GtkWidget *widget)
{
  const char *shot_scale = g_getenv ("VIBEOS_SHOT_SCALE");

  return shot_scale && atoi (shot_scale) > 0 ? atoi (shot_scale) : gtk_widget_get_scale_factor (widget);
}

/* ---- text ---------------------------------------------------------------------------------------------------- */

void
vibeos_label_set_font_padding (GtkWidget *label, double size)
{
  gtk_widget_set_margin_top (label, (int) round (size * (2163 - 1900) / 2048.0));
  gtk_widget_set_margin_bottom (label, (int) round (size * (555 - 500) / 2048.0));
  gtk_label_set_yalign (GTK_LABEL (label), 0);
}

/* ---- icons ----------------------------------------------------------------------------------------------------- */

void
vibeos_snapshot_icon (GtkSnapshot *snapshot, GtkWidget *widget, const char *icon_name, double x, double y, double size,
                   const GdkRGBA *color)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  GtkIconPaintable *icon = gtk_icon_theme_lookup_icon (theme, icon_name, NULL, (int) size,
                                                       gtk_widget_get_scale_factor (widget), GTK_TEXT_DIR_NONE, 0);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  /* the symbolic colours are foreground, error, warning, success: an Android tint is the first of them */
  GdkRGBA colors[4] = { *color, *color, *color, *color };
  gtk_symbolic_paintable_snapshot_symbolic (GTK_SYMBOLIC_PAINTABLE (icon), snapshot, size, size, colors, 4);
  gtk_snapshot_restore (snapshot);
  g_object_unref (icon);
}
