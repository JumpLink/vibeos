/* Time zones: Etar's TimeZonePickerDialog (com.android.timezonepicker) and the names it shows, from ICU.
 * Owned by the time-zone work; the editor's time zone row and "Home time zone" use it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the zone chosen, an IANA id ("Europe/Berlin"); not called when the picker is dismissed */
typedef void (*CalTzChosen) (const char *tzid, gpointer data);

/* the text Etar shows for @tzid at the time @at (Unix seconds; 0 now): TimeZonePickerUtils.getGmtDisplayName, the
 * zone's name, two spaces, its offset ("GMT+1", "GMT+5:30") and " ☀" for a zone with daylight saving time:
 * "Central European Standard Time  GMT+1 ☀". An id ICU does not know comes back as it is. */
char *cal_tz_label        (const char *tzid, gint64 at);
/* the same as Pango markup, with the spans Etar colours: the sun #BFBFBF always, the offset #888888 with @gray_gmt.
 * The editor's time zone button is getGmtDisplayName(…, grayGmt true), Home time zone's summary grayGmt false. */
char *cal_tz_label_markup (const char *tzid, gint64 at, gboolean gray_gmt);
/* the picker over @parent's window: a search field and the zones; @current first in the list, then the zones chosen
 * last. @at is the time the names are for (the event's start; 0 now). */
void  cal_tz_picker_show  (GtkWidget *parent, const char *current, gint64 at, CalTzChosen chosen, gpointer data);

G_END_DECLS
