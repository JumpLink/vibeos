/* The phone's text messages as ModemManager has them on the system bus: the first modem with a Messaging interface
 * (org.freedesktop.ModemManager1.Modem.Messaging), each message one org.freedesktop.ModemManager1.Sms object.
 *
 * A message that comes is handed over once it is all there (ModemManager puts the parts of a long one together:
 * RECEIVING until the last part, then RECEIVED); messages that came while nobody listened are handed over when the
 * modem comes. The service keeps them and then deletes them off the modem, as Android's SmsDeliverReceiver takes
 * them off the SIM. Sending is Messaging.Create then Sms.Send.
 *
 * Plain GDBus rather than libmm-glib, as the Phone's modem.c: a handful of calls and properties. */
#pragma once

#include <gio/gio.h>

typedef struct _SmsModem SmsModem;

typedef struct {
  /* a messaging modem came (enabled, with its Messaging interface) or went */
  void (*available_changed) (gboolean available, gpointer data);
  /* a message came whole: from @number (as the network sent it), sent by them at @timestamp (Unix ms, 0 when the
   * network gave none). @path is its Sms object, for sms_modem_delete once it is kept */
  void (*received) (const char *path, const char *number, const char *text, gint64 timestamp, gpointer data);
} SmsModemCallbacks;

SmsModem   *sms_modem_new (GDBusConnection *system, const SmsModemCallbacks *callbacks, gpointer data);
void        sms_modem_free (SmsModem *self);

gboolean    sms_modem_is_available (SmsModem *self);
/* the network's country as ISO 3166 ("DE"), from its MCC; NULL while not registered */
const char *sms_modem_get_country (SmsModem *self);

/* sends @text to @number: Messaging.Create, then Sms.Send, which ModemManager answers once the network took it (or
 * refused it). The Sms object is deleted after either way: it is kept in the store, not on the modem. No delivery
 * report is asked for (Messaging's "SMS delivery reports" is off unless set, and is not offered here yet) */
void        sms_modem_send (SmsModem *self, const char *number, const char *text, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer data);
/* the error as ModemManager sent it (g_dbus_error_get_remote_error names it); G_IO_ERROR_NOT_CONNECTED without a
 * modem, G_IO_ERROR_CANCELLED when the modem was freed meanwhile */
gboolean    sms_modem_send_finish (SmsModem *self, GAsyncResult *result, GError **error);
/* Messaging.Delete: a received message the store has now */
void        sms_modem_delete (SmsModem *self, const char *path);
