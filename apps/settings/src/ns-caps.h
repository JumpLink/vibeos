/* What the device has, for the rows that only make sense with it: a modem (SIMs, the ring volume, the ringtone, the
 * dial pad tones), a battery (Battery), and whether it is a phone or a tablet (About phone, About tablet). Nothing
 * here asks for a device by name: a modem is one ModemManager has, a battery one UPower reports, a tablet a screen
 * whose smallest side is 600 dp or more.
 *
 * For screenshots on a machine without them: NS_CAPABILITIES=modem,battery (or "none") says what the device has and
 * asks no service. */
#pragma once

#include <gtk/gtk.h>

typedef enum {
  NS_CAP_MODEM   = 1 << 0,
  NS_CAP_BATTERY = 1 << 1,
} NsCap;

/* starts following ModemManager and UPower; once, at startup */
void        ns_caps_init         (void);
gboolean    ns_caps_has          (NsCap cap);
/* `widget` shown only while the device has `cap` (a modem can come and go) */
void        ns_caps_bind_visible (GtkWidget *widget, NsCap cap);
/* `cb` runs whenever what the device has changes, as long as `object` lives */
void        ns_caps_on_changed   (GCallback cb, gpointer object);

/* "phone" or "tablet": the device class of the app's window (a phone before there is one) */
const char *ns_device_noun       (void);
/* "About phone" or "About tablet" */
const char *ns_about_title       (void);
