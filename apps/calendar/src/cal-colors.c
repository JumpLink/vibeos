/* Etar's three looks (cal-colors.h). */
#include "cal-colors.h"

#include <gtk/gtk.h>
#include <neo.h>

/* the `_dark` colours over the baseline dark roles (measured on the phone, spec/calendar.md §1) */
static const CalPalette dark = {
  .surface = 0xff141218, .surface_container_low = 0xff1d1b20, .surface_container = 0xff211f26,
  .surface_container_high = 0xff2b2930, .surface_container_highest = 0xff36343b,
  .on_surface = 0xffe6e0e9, .on_surface_variant = 0xffcac4d0, .outline = 0xff938f99,
  .primary = 0xff4d7f86, .on_primary = 0xff381e72, .primary_container = 0xff4f378b,
  .on_primary_container = 0xffeaddff,
  .secondary_container = 0xff00363c,           /* night-v31: system_accent1_800 */
  .on_secondary_container = 0xffe8def8,
  .hour_bg = 0xff141414, .future_bg = 0xff141414,
  .past_bg = 0xff2e2d32,                        /* #22DDDDDD over the window's #141218 */
  .grid_line = 0xff6a6a6a, .hour_label = 0xb2ffffff, .day_label = 0xb2ffffff, .weekend_label = 0xfffafafa,
  .week_today = 0xff90caf9, .now_line = 0xffff0000, .grid_selected = 0xe633b5e5, .clicked = 0x8033b5e5,
  .new_event_hint = 0xffffffff, .more_events = 0xff666666, .expand_icon = 0xff797979,
  .staggered_shadow = 0xf0141414,
  .month_focus_bg = 0xff212121, .month_other_bg = 0xff000000, .month_today_bg = 0x6efffffa,
  .month_number = 0xffffffff, .month_other_number = 0xff616161, .month_day_names = 0xffffffff,
  .month_weekend_names = 0xfff5f5f5, .month_week_number = 0xff4d7f86, .month_event_text = 0xffffffff,
  .month_event_extra = 0xffffffff, .month_grid_line = 0xff6a6a6a,
  .agenda_separator = 0xffbbbbbb, .agenda_past_present = 0xb2ff0000,
  .agenda_day_text = 0xffcac4d0,                /* measured (agenda_day_item_text_color_dark is #E0E0E0) */
  .agenda_title = 0xffffffff, .agenda_when = 0xfff5f5f5,
  .agenda_declined_title = 0xff616161, .agenda_declined_when = 0x7fffffff,
  .selected_number = 0xff141218, .switch_thumb_off = 0xffb9b9b9, .switch_track_off = 0xff5b5a5e,
  .dark = TRUE,
};

/* Pure black night mode: colorBackground #000000 (styles-color.xml colorBackgroundBlack) and the `_black` colours;
 * the dialogs and menus keep their surface containers */
static const CalPalette black = {
  .surface = 0xff000000, .surface_container_low = 0xff1d1b20, .surface_container = 0xff211f26,
  .surface_container_high = 0xff2b2930, .surface_container_highest = 0xff36343b,
  .on_surface = 0xffe6e0e9, .on_surface_variant = 0xffcac4d0, .outline = 0xff938f99,
  .primary = 0xff4d7f86, .on_primary = 0xff381e72, .primary_container = 0xff4f378b,
  .on_primary_container = 0xffeaddff, .secondary_container = 0xff00363c, .on_secondary_container = 0xffe8def8,
  .hour_bg = 0xff000000, .future_bg = 0xff000000,
  .past_bg = 0xff1d1d1d,                        /* #22DDDDDD over black */
  .grid_line = 0xff303030, .hour_label = 0xffffffff, .day_label = 0xffffffff, .weekend_label = 0xfffafafa,
  .week_today = 0xff90caf9, .now_line = 0xffff0000, .grid_selected = 0xe633b5e5, .clicked = 0x8033b5e5,
  .new_event_hint = 0xffffffff, .more_events = 0xff666666, .expand_icon = 0xff797979,
  .staggered_shadow = 0xf0000000,
  .month_focus_bg = 0xff212121, .month_other_bg = 0xff111111, .month_today_bg = 0x6efffffa,
  .month_number = 0xffffffff, .month_other_number = 0xff616161, .month_day_names = 0xffffffff,
  .month_weekend_names = 0xfff5f5f5, .month_week_number = 0xff4d7f86, .month_event_text = 0xffffffff,
  .month_event_extra = 0xffffffff, .month_grid_line = 0xff303030,
  .agenda_separator = 0xffbbbbbb, .agenda_past_present = 0xb2ff0000, .agenda_day_text = 0xffcac4d0,
  .agenda_title = 0xffffffff, .agenda_when = 0xfff5f5f5,
  .agenda_declined_title = 0xff616161, .agenda_declined_when = 0xff424242,
  .selected_number = 0xff141218, .switch_thumb_off = 0xffb9b9b9, .switch_track_off = 0xff5b5a5e,
  .dark = TRUE,
};

/* the plain colours over Material 3's baseline light roles; colorPrimary is system_accent1_500 in both modes, the
 * secondary container system_accent1_200 (values-v31) */
static const CalPalette light = {
  .surface = 0xfffef7ff, .surface_container_low = 0xfff7f2fa, .surface_container = 0xfff3edf7,
  .surface_container_high = 0xffece6f0, .surface_container_highest = 0xffe6e0e9,
  .on_surface = 0xff1d1b20, .on_surface_variant = 0xff49454f, .outline = 0xff79747e,
  .primary = 0xff4d7f86, .on_primary = 0xffffffff, .primary_container = 0xffeaddff,
  .on_primary_container = 0xff21005d,
  .secondary_container = 0xffa0cfd6, .on_secondary_container = 0xff1d192b,
  .hour_bg = 0xffeeeeee, .future_bg = 0xffffffff,
  .past_bg = 0xfffaf3fa,                        /* #22DDDDDD over the window's #FEF7FF */
  .grid_line = 0xffcccccc, .hour_label = 0xff666666, .day_label = 0xff666666, .weekend_label = 0x80333333,
  .week_today = 0x802979ff, .now_line = 0xffff0000, .grid_selected = 0xe633b5e5, .clicked = 0x8033b5e5,
  .new_event_hint = 0xffffffff, .more_events = 0xff666666, .expand_icon = 0xff666666,
  .staggered_shadow = 0xf0141414,
  .month_focus_bg = 0xffffffff, .month_other_bg = 0xffeeeeee, .month_today_bg = 0x25a2ff00,
  .month_number = 0xff555555, .month_other_number = 0xff999999, .month_day_names = 0xff757575,
  .month_weekend_names = 0xff757575, .month_week_number = 0xff41c3b1, .month_event_text = 0xff333333,
  .month_event_extra = 0xff666666, .month_grid_line = 0xffdedede,
  .agenda_separator = 0xffbbbbbb, .agenda_past_present = 0xb2ff0000, .agenda_day_text = 0xff49454f,
  .agenda_title = 0xff333333, .agenda_when = 0xff666666,
  .agenda_declined_title = 0x66333333, .agenda_declined_when = 0x66666666,
  .selected_number = 0xffffffff, .switch_thumb_off = 0xfff1f1f1, .switch_track_off = 0x61000000,
  .dark = FALSE,
};

const CalPalette *cal_palette = &dark;
gboolean cal_theme_light;
static CalLook current = CAL_LOOK_DARK;
static GtkCssProvider *overlay;

GdkRGBA
cal_ripple_color (void)
{
  return cal_palette->dark ? (GdkRGBA) { 1, 1, 1, 0.2f / 2 } : (GdkRGBA) { 0, 0, 0, 0.12f / 2 };
}

/* a NeoPressable as it maps (a page or dialog built after the look changed) takes the look's ripple */
static gboolean
on_map (GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer data)
{
  GObject *object = g_value_get_object (&values[0]);

  if (NEO_IS_PRESSABLE (object))
    {
      GdkRGBA ripple = cal_ripple_color ();

      neo_pressable_set_ripple_color (NEO_PRESSABLE (object), &ripple);
    }
  return TRUE;
}

void
cal_palette_restyle (GtkWidget *root)
{
  if (NEO_IS_PRESSABLE (root))
    {
      GdkRGBA ripple = cal_ripple_color ();

      neo_pressable_set_ripple_color (NEO_PRESSABLE (root), &ripple);
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
    [CAL_LOOK_BLACK] = "/neo/Calendar/calendar-black.css",
    [CAL_LOOK_LIGHT] = "/neo/Calendar/calendar-light.css",
  };
  GdkDisplay *display = gdk_display_get_default ();

  static gboolean hooked;

  if (!hooked)
    {
      /* the class made, or "map" is not there yet to hook (at startup no widget exists) */
      g_type_class_ref (GTK_TYPE_WIDGET);
      g_type_ensure (NEO_TYPE_PRESSABLE);
      g_signal_add_emission_hook (g_signal_lookup ("map", GTK_TYPE_WIDGET), 0, on_map, NULL, NULL);
      hooked = TRUE;
    }
  current = look;
  cal_palette = look == CAL_LOOK_LIGHT ? &light : look == CAL_LOOK_BLACK ? &black : &dark;
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
