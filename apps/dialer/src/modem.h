/* The phone's modem, as ModemManager shows it on the system bus: the first modem that can make voice calls
 * (org.freedesktop.ModemManager1.Modem.Voice), its calls coming and going, and the country of the network it is on.
 *
 * Plain GDBus rather than libmm-glib: the service needs a handful of calls and properties, and talking D-Bus
 * directly keeps it working with any ModemManager build the phone carries. */
#pragma once

#include <gio/gio.h>

typedef struct _Modem Modem;

typedef struct {
  /* a voice modem came (it is enabled and has its Voice interface) or went (unplugged, ModemManager restarted) */
  void (*available_changed) (gboolean available, gpointer data);
  /* ModemManager made a call object: an incoming call, or one dialled (by us or by anyone else) */
  void (*call_added) (const char *path, gpointer data);
  void (*call_deleted) (const char *path, gpointer data);
} ModemCallbacks;

Modem      *modem_new (GDBusConnection *system, const ModemCallbacks *callbacks, gpointer data);
void        modem_free (Modem *self);

gboolean    modem_is_available (Modem *self);
/* the network's country as ISO 3166 ("DE"), from its MCC; NULL while not registered */
const char *modem_get_country (Modem *self);
/* only emergency calls can be made (no SIM, or not registered) */
gboolean    modem_get_emergency_only (Modem *self);
/* the modem's own words for itself ("Samsung SM-G970F"), for what it learned about the modem to be kept by */
char       *modem_get_model (Modem *self);

/* Voice.CreateCall: a new outgoing call object for @number, not yet dialling (the caller Starts it) */
void        modem_create_call (Modem *self, const char *number, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer data);
char       *modem_create_call_finish (Modem *self, GAsyncResult *result, GError **error);
/* one of Voice's methods that take nothing: HoldAndAccept (GSM's CHLD=2), HangupAndAccept (CHLD=1), HangupAll */
void        modem_voice_method (Modem *self, const char *method, GAsyncReadyCallback callback, gpointer data);
gboolean    modem_voice_method_finish (Modem *self, GAsyncResult *result, GError **error);
/* Voice.DeleteCall: ModemManager keeps a call that ended until someone deletes it */
void        modem_delete_call (Modem *self, const char *path);

/* @error said the modem does not do it at all (+CME ERROR: 4), not that it failed this time */
gboolean    modem_error_is_unsupported (const GError *error);
