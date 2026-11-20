/* The Calendar's three looks (cal-colors.h). */
#include "cal-colors.h"

#include <gtk/gtk.h>
#include <math.h>
#include <vibeos.h>

/* a role at an alpha, as 0xAARRGGBB */
static guint32
argb (VibeosColor color, double alpha)
{
  GdkRGBA rgba = vibeos_color (color);

  return ((guint32) lround (alpha * 255) << 24) | ((guint32) lround (rgba.red * 255) << 16)
         | ((guint32) lround (rgba.green * 255) << 8) | (guint32) lround (rgba.blue * 255);
}

/* the palette from the theme as it is now: every colour a role of it; the black look puts the window's surfaces on
 * black */
static void
compute (CalPalette *p, CalLook look)
{
  gboolean black = look == CAL_LOOK_BLACK;

  *p = (CalPalette) {
    .surface = black ? 0xff000000 : argb (VIBEOS_SURFACE, 1),
    .surface_container_low = argb (VIBEOS_SURFACE_CONTAINER_LOW, 1),
    .surface_container = argb (VIBEOS_SURFACE_CONTAINER, 1),
    .surface_container_high = argb (VIBEOS_SURFACE_CONTAINER_HIGH, 1),
    .surface_container_highest = argb (VIBEOS_SURFACE_CONTAINER_HIGHEST, 1),
    .on_surface = argb (VIBEOS_ON_SURFACE, 1), .on_surface_variant = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .outline = argb (VIBEOS_OUTLINE, 1),
    .primary = argb (VIBEOS_PRIMARY, 1), .on_primary = argb (VIBEOS_ON_PRIMARY, 1),
    .primary_container = argb (VIBEOS_PRIMARY_CONTAINER, 1),
    .on_primary_container = argb (VIBEOS_ON_PRIMARY_CONTAINER, 1),
    .secondary_container = argb (VIBEOS_SECONDARY_CONTAINER, 1),
    .on_secondary_container = argb (VIBEOS_ON_SECONDARY_CONTAINER, 1),
    /* the Day and Week views: the past a faint wash over the window */
    .hour_bg = black ? 0xff000000 : argb (VIBEOS_SURFACE, 1),
    .future_bg = black ? 0xff000000 : argb (VIBEOS_SURFACE, 1),
    .past_bg = argb (black ? VIBEOS_SURFACE_CONTAINER_LOW : VIBEOS_SURFACE_CONTAINER, 1),
    .grid_line = argb (VIBEOS_OUTLINE_VARIANT, 1),
    .hour_label = argb (VIBEOS_ON_SURFACE_VARIANT, 1), .day_label = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .weekend_label = argb (VIBEOS_ON_SURFACE, 1),
    .week_today = argb (VIBEOS_PRIMARY, 1), .now_line = argb (VIBEOS_ERROR, 1),
    .grid_selected = argb (VIBEOS_PRIMARY, 0.9), .clicked = argb (VIBEOS_PRIMARY, 0.5),
    .new_event_hint = argb (VIBEOS_ON_PRIMARY, 1),
    .more_events = argb (VIBEOS_ON_SURFACE_VARIANT, 1), .expand_icon = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .staggered_shadow = black ? 0xf0000000 : argb (VIBEOS_SURFACE, 0.94),
    /* Month: the month in focus on the window, every second other month on a container */
    .month_focus_bg = black ? argb (VIBEOS_SURFACE_CONTAINER_LOW, 1) : argb (VIBEOS_SURFACE, 1),
    .month_other_bg = black ? 0xff000000 : argb (VIBEOS_SURFACE_CONTAINER, 1),
    .month_today_bg = argb (VIBEOS_PRIMARY, 0.28),
    .month_number = argb (VIBEOS_ON_SURFACE, 1), .month_other_number = argb (VIBEOS_OUTLINE, 1),
    .month_day_names = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .month_weekend_names = argb (VIBEOS_ON_SURFACE_VARIANT, 1), .month_week_number = argb (VIBEOS_PRIMARY, 1),
    .month_event_text = argb (VIBEOS_ON_SURFACE, 1), .month_event_extra = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .month_grid_line = argb (VIBEOS_OUTLINE_VARIANT, 1),
    /* Agenda */
    .agenda_separator = argb (VIBEOS_OUTLINE_VARIANT, 1), .agenda_past_present = argb (VIBEOS_ERROR, 0.7),
    .agenda_day_text = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .agenda_title = argb (VIBEOS_ON_SURFACE, 1), .agenda_when = argb (VIBEOS_ON_SURFACE_VARIANT, 1),
    .agenda_declined_title = argb (VIBEOS_OUTLINE, 1), .agenda_declined_when = argb (VIBEOS_ON_SURFACE_VARIANT, 0.5),
    .selected_number = argb (VIBEOS_ON_PRIMARY, 1),
    .switch_thumb_off = argb (VIBEOS_OUTLINE, 1), .switch_track_off = argb (VIBEOS_SURFACE_CONTAINER_HIGHEST, 1),
    .disabled = argb (VIBEOS_ON_SURFACE, 0.38),
    .dark = vibeos_theme_get_dark (vibeos_theme_get_default ()),
  };
}

static CalPalette palette;
const CalPalette *cal_palette = &palette;
gboolean cal_theme_light;
static CalLook current = CAL_LOOK_DARK;
static GtkCssProvider *overlay;

GdkRGBA
cal_ripple_color (void)
{
  return vibeos_ripple_default_color ();
}

/* a VibeosPressable as it maps (a page or dialog built after the look changed) takes the look's ripple */
static gboolean
on_map (GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer data)
{
  GObject *object = g_value_get_object (&values[0]);

  if (VIBEOS_IS_PRESSABLE (object))
    {
      GdkRGBA ripple = cal_ripple_color ();

      vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (object), &ripple);
    }
  return TRUE;
}

void
cal_palette_restyle (GtkWidget *root)
{
  if (VIBEOS_IS_PRESSABLE (root))
    {
      GdkRGBA ripple = cal_ripple_color ();

      vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (root), &ripple);
    }
  gtk_widget_queue_draw (root);
  for (GtkWidget *child = gtk_widget_get_first_child (root); child; child = gtk_widget_get_next_sibling (child))
    cal_palette_restyle (child);
}

void
cal_palette_set (CalLook look)
{
  static const char *sheets[] = {
    [CAL_LOOK_DARK] = NULL,
    [CAL_LOOK_BLACK] = "/vibeos/Calendar/calendar-black.css",
    [CAL_LOOK_LIGHT] = NULL,
  };
  GdkDisplay *display = gdk_display_get_default ();

  static gboolean hooked;

  if (!hooked)
    {
      /* the class made, or "map" is not there yet to hook (at startup no widget exists) */
      g_type_class_ref (GTK_TYPE_WIDGET);
      g_type_ensure (VIBEOS_TYPE_PRESSABLE);
      g_signal_add_emission_hook (g_signal_lookup ("map", GTK_TYPE_WIDGET), 0, on_map, NULL, NULL);
      hooked = TRUE;
    }
  current = look;
  compute (&palette, look);
  if (!display)
    return;
  if (overlay)
    {
      gtk_style_context_remove_provider_for_display (display, GTK_STYLE_PROVIDER (overlay));
      g_clear_object (&overlay);
    }
  if (sheets[look])
    {
      overlay = gtk_css_provider_new ();
      gtk_css_provider_load_from_resource (overlay, sheets[look]);
      /* over calendar.css (APPLICATION + 1): its named colours, redefined, win */
      gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (overlay),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    }
}

CalLook
cal_palette_look (void)
{
  return current;
}
