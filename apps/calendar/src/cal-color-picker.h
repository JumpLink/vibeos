/* Etar's colour picker (com.android.calendar.colorpicker: ColorPickerDialog over ColorPickerPalette): a calendar's
 * colour (CalendarColorPickerDialogX, "Calendar Color") and an event's (EventColorPickerDialog, "Event Color", with
 * "Set to default calendar color").
 *
 * The colours go in as they are stored (0xRRGGBB, a calendar's or an event's raw colour). The dialog shows them as
 * Etar does: each as its display colour (Utils.getDisplayColorFromColor, cal_display_color with the "Use real event
 * colors" setting), sorted by HsvColorComparator (hue, then saturation, then value, highest first); the swatch whose
 * display colour is @current's is checked. The colour chosen comes back raw. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @rgb 0xRRGGBB, one of the colours given; not called when the dialog is dismissed */
typedef void (*CalColorChosen) (guint32 rgb, gpointer data);

/* the swatches of @colors under @title, @current checked; a tap on one chooses it and closes */
void cal_color_picker_show       (GtkWidget *parent, const char *title, const guint32 *colors, int n_colors,
                                  guint32 current, CalColorChosen chosen, gpointer data);
/* EventColorPickerDialog: "Event Color", and the neutral "Set to default calendar color", which chooses
 * @calendar_color */
void cal_event_color_picker_show (GtkWidget *parent, const guint32 *colors, int n_colors, guint32 current,
                                  guint32 calendar_color, CalColorChosen chosen, gpointer data);

/* Etar's defaultCalendarColors (V/colors.xml): the 24 colours an offline account gets for its calendars and its
 * events (CalendarDataSource.maybeAddCalendarAndEventColors), and the palette offered for any calendar whose account
 * brings none of its own (CalendarColorPickerDialogX.useDefaultColors) */
const guint32 *cal_color_picker_default_colors (int *n_colors);
/* the colour a new offline calendar gets: key "1" of them (DEFAULT_COLOR_KEY) */
#define CAL_COLOR_PICKER_OFFLINE_DEFAULT 0x11ab64

G_END_DECLS
