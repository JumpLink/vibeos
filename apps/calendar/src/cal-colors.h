/* Etar's colours on this phone (spec/calendar.md §1), as 0xAARRGGBB.
 *
 * Not libneo's palette: Etar's Material 3 theme is the library's baseline (purple-grey surfaces) with only
 * colorPrimary/colorAccent taken from the wallpaper (system_accent1_500, measured #4D7F86), and its views draw with
 * colours of their own (res/values/colors.xml, the _dark set). The CSS names the same values (calendar.css). */
#pragma once

#include <gdk/gdk.h>

/* Material 3 roles as Etar's theme resolves them (measured) */
#define CAL_SURFACE                     0xff141218
#define CAL_SURFACE_CONTAINER_LOW       0xff1d1b20
#define CAL_SURFACE_CONTAINER           0xff211f26
#define CAL_SURFACE_CONTAINER_HIGH      0xff2b2930
#define CAL_SURFACE_CONTAINER_HIGHEST   0xff36343b
#define CAL_ON_SURFACE                  0xffe6e0e9
#define CAL_ON_SURFACE_VARIANT          0xffcac4d0
#define CAL_OUTLINE                     0xff938f99
#define CAL_PRIMARY                     0xff4d7f86      /* colorPrimary = colorAccent = system_accent1_500 */
#define CAL_ON_PRIMARY                  0xff381e72
#define CAL_PRIMARY_CONTAINER           0xff4f378b
#define CAL_ON_PRIMARY_CONTAINER        0xffeaddff
#define CAL_SECONDARY_CONTAINER         0xff00363c      /* night-v31: system_accent1_800 */
#define CAL_ON_SECONDARY_CONTAINER      0xffe8def8

/* DayView (V/colors.xml) */
#define CAL_HOUR_BG             0xff141414      /* calendar_hour_background_dark: hours column, week header */
#define CAL_FUTURE_BG           0xff141414      /* bg_dark: the grid from now on */
#define CAL_PAST_BG             0xff2e2d32      /* day_past_background_color #22DDDDDD over the surface */
#define CAL_GRID_LINE           0xff6a6a6a      /* grid_line_dark */
#define CAL_HOUR_LABEL          0xb2ffffff      /* calendar_hour_label_dark */
#define CAL_DAY_LABEL           0xb2ffffff      /* calendar_date_banner_text_color_dark */
#define CAL_WEEKEND_LABEL       0xfffafafa      /* week_saturday_dark, week_sunday_dark (measured) */
#define CAL_WEEK_TODAY          0xff90caf9      /* today_highlight_color_dark */
#define CAL_NOW_LINE            0xffff0000
#define CAL_GRID_SELECTED       0xe633b5e5      /* calendar_grid_area_selected_dark */
#define CAL_CLICKED             0x8033b5e5      /* calendar_view_clicked_dark */
#define CAL_NEW_EVENT_HINT      0xffffffff
#define CAL_MORE_EVENTS         0xff666666      /* month_more_events / more_events text */
#define CAL_EXPAND_ICON         0xff797979      /* ic_expand_small as tinted (measured) */

/* Month (V/colors.xml) */
#define CAL_MONTH_FOCUS_BG      0xff212121      /* month_bgcolor_dark */
#define CAL_MONTH_OTHER_BG      0xff000000      /* month_other_bgcolor_dark */
#define CAL_MONTH_TODAY_BG      0x6efffffa      /* month_today_bgcolor_dark */
#define CAL_MONTH_NUMBER        0xffffffff
#define CAL_MONTH_OTHER_NUMBER  0xff616161
#define CAL_MONTH_DAY_NAMES     0xffffffff

/* Agenda */
#define CAL_AGENDA_SEPARATOR    0xffbbbbbb
#define CAL_AGENDA_PAST_PRESENT 0xb2ff0000

static inline GdkRGBA
cal_argb (guint32 argb)
{
  return (GdkRGBA) { ((argb >> 16) & 0xff) / 255.f, ((argb >> 8) & 0xff) / 255.f, (argb & 0xff) / 255.f,
                     ((argb >> 24) & 0xff) / 255.f };
}
