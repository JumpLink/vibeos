/* Etar's colours on this phone (spec/calendar.md §1), as 0xAARRGGBB, in the three looks its Theme setting gives:
 * dark (the `_dark` colours over Material 3's baseline dark roles), pure black (the `_black` colours and a black
 * window, Pure black night mode) and light (the plain colours over the baseline light roles). DynamicTheme.getColor
 * picks a colour by appending `_dark` or `_black` to its name; cal_palette_set does the same once for the app.
 *
 * Not libneo's palette: Etar's Material 3 theme is the library's baseline (purple-grey surfaces) with only
 * colorPrimary/colorAccent taken from the wallpaper (system_accent1_500, measured #4D7F86), and its views draw with
 * colours of their own (res/values/colors.xml). The stylesheets name the same values (calendar.css for dark,
 * calendar-black.css and calendar-light.css over it). The names below read the palette in use. */
#pragma once

#include <gtk/gtk.h>

typedef enum {
  CAL_LOOK_DARK,
  CAL_LOOK_BLACK,               /* "Pure black night mode" */
  CAL_LOOK_LIGHT,
} CalLook;

typedef struct {
  /* Material 3 roles as Etar's theme resolves them */
  guint32 surface, surface_container_low, surface_container, surface_container_high, surface_container_highest;
  guint32 on_surface, on_surface_variant, outline, primary, on_primary, primary_container, on_primary_container;
  guint32 secondary_container, on_secondary_container;
  /* DayView */
  guint32 hour_bg, future_bg, past_bg, grid_line, hour_label, day_label, weekend_label, week_today, now_line;
  guint32 grid_selected, clicked, new_event_hint, more_events, expand_icon, staggered_shadow;
  /* Month */
  guint32 month_focus_bg, month_other_bg, month_today_bg, month_number, month_other_number, month_day_names;
  guint32 month_weekend_names, month_week_number, month_event_text, month_event_extra, month_grid_line;
  /* Agenda */
  guint32 agenda_separator, agenda_past_present, agenda_day_text, agenda_title, agenda_when;
  guint32 agenda_declined_title, agenda_declined_when;
  /* the time picker's number under the selector, the compat switch's off thumb and track */
  guint32 selected_number, switch_thumb_off, switch_track_off;
  gboolean dark;
} CalPalette;

extern const CalPalette *cal_palette;

/* the look in use: the palette above, and the stylesheet over calendar.css (none for dark) */
void        cal_palette_set   (CalLook look);
CalLook     cal_palette_look  (void);
/* the Theme setting is exactly "light" (not "system" in a light look): MonthWeekEventsView.drawBackground then
 * paints today in colorPrimary at alpha 72 instead of month_today_bgcolor */
extern gboolean cal_theme_light;
/* colorControlHighlight halved as RippleDrawable halves it: white 20 % dark (ripple_material_dark, libneo's),
 * black 12 % light (ripple_material_light). Every NeoPressable takes it as it maps. */
GdkRGBA     cal_ripple_color  (void);
/* after cal_palette_set: every widget under @root redrawn in the new colours, its ripples recoloured */
void        cal_palette_restyle (GtkWidget *root);

/* Material 3 roles */
#define CAL_SURFACE                     (cal_palette->surface)
#define CAL_SURFACE_CONTAINER_LOW       (cal_palette->surface_container_low)
#define CAL_SURFACE_CONTAINER           (cal_palette->surface_container)
#define CAL_SURFACE_CONTAINER_HIGH      (cal_palette->surface_container_high)
#define CAL_SURFACE_CONTAINER_HIGHEST   (cal_palette->surface_container_highest)
#define CAL_ON_SURFACE                  (cal_palette->on_surface)
#define CAL_ON_SURFACE_VARIANT          (cal_palette->on_surface_variant)
#define CAL_OUTLINE                     (cal_palette->outline)
#define CAL_PRIMARY                     (cal_palette->primary)      /* colorPrimary = colorAccent */
#define CAL_ON_PRIMARY                  (cal_palette->on_primary)
#define CAL_PRIMARY_CONTAINER           (cal_palette->primary_container)
#define CAL_ON_PRIMARY_CONTAINER        (cal_palette->on_primary_container)
#define CAL_SECONDARY_CONTAINER         (cal_palette->secondary_container)
#define CAL_ON_SECONDARY_CONTAINER      (cal_palette->on_secondary_container)

/* DayView (V/colors.xml) */
#define CAL_HOUR_BG             (cal_palette->hour_bg)          /* calendar_hour_background: hours column, header */
#define CAL_FUTURE_BG           (cal_palette->future_bg)        /* calendar_future_bg_color: the grid from now on */
#define CAL_PAST_BG             (cal_palette->past_bg)          /* day_past_background_color over the window */
#define CAL_GRID_LINE           (cal_palette->grid_line)        /* calendar_grid_line_inner_horizontal_color */
#define CAL_HOUR_LABEL          (cal_palette->hour_label)       /* calendar_hour_label */
#define CAL_DAY_LABEL           (cal_palette->day_label)        /* calendar_date_banner_text_color */
#define CAL_WEEKEND_LABEL       (cal_palette->weekend_label)    /* week_saturday, week_sunday */
#define CAL_WEEK_TODAY          (cal_palette->week_today)       /* week_today */
#define CAL_NOW_LINE            (cal_palette->now_line)
#define CAL_GRID_SELECTED       (cal_palette->grid_selected)    /* calendar_grid_area_selected */
#define CAL_CLICKED             (cal_palette->clicked)          /* day_event_clicked_background_color */
#define CAL_NEW_EVENT_HINT      (cal_palette->new_event_hint)
#define CAL_MORE_EVENTS         (cal_palette->more_events)      /* month_event_other_color, "+N" */
#define CAL_EXPAND_ICON         (cal_palette->expand_icon)      /* ic_expand_small as tinted (measured) */
#define CAL_STAGGERED_SHADOW    (cal_palette->staggered_shadow) /* staggered_event_shadow_color */

/* Month */
#define CAL_MONTH_FOCUS_BG      (cal_palette->month_focus_bg)   /* month_focus_month_bgcolor */
#define CAL_MONTH_OTHER_BG      (cal_palette->month_other_bg)   /* month_other_bgcolor */
#define CAL_MONTH_TODAY_BG      (cal_palette->month_today_bg)   /* month_today_bgcolor */
#define CAL_MONTH_NUMBER        (cal_palette->month_number)     /* month_day_number */
#define CAL_MONTH_OTHER_NUMBER  (cal_palette->month_other_number)
#define CAL_MONTH_DAY_NAMES     (cal_palette->month_day_names)
#define CAL_MONTH_WEEKEND_NAMES (cal_palette->month_weekend_names)  /* month_saturday, month_sunday */
#define CAL_MONTH_WEEK_NUMBER   (cal_palette->month_week_number)    /* month_week_num_color */
#define CAL_MONTH_EVENT_TEXT    (cal_palette->month_event_text)     /* month_event_color */
#define CAL_MONTH_EVENT_EXTRA   (cal_palette->month_event_extra)    /* month_event_extra_color: times, "+N" */
#define CAL_MONTH_GRID_LINE     (cal_palette->month_grid_line)      /* month_grid_lines */

/* Agenda */
#define CAL_AGENDA_SEPARATOR    (cal_palette->agenda_separator)
#define CAL_AGENDA_PAST_PRESENT (cal_palette->agenda_past_present)
#define CAL_AGENDA_DAY_TEXT     (cal_palette->agenda_day_text)
#define CAL_AGENDA_TITLE        (cal_palette->agenda_title)     /* agenda_item_standard_color */
#define CAL_AGENDA_WHEN         (cal_palette->agenda_when)      /* agenda_item_where_text_color */
#define CAL_AGENDA_DECLINED_TITLE (cal_palette->agenda_declined_title)
#define CAL_AGENDA_DECLINED_WHEN  (cal_palette->agenda_declined_when)

static inline GdkRGBA
cal_argb (guint32 argb)
{
  return (GdkRGBA) { ((argb >> 16) & 0xff) / 255.f, ((argb >> 8) & 0xff) / 255.f, (argb & 0xff) / 255.f,
                     ((argb >> 24) & 0xff) / 255.f };
}
