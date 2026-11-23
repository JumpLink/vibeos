/* dictate-calls: phone calls as Agent sees them, through the Phone's call service (vibeos.Dialer.Calls, which
 * vibeos-dialer-calls keeps on the session bus): the calls read, and a call that starts or stops ringing watched.
 *
 * The in-call tools (dictate-tools-phone.c) tell the service what to do; the app (dictate-main.c) watches it, and
 * Agent says who is calling when the user wants that (dictate_calls_may_announce ()). A caller is said by name, or
 * as an unknown or a private number: never its digits.
 *
 * For the tests the service is on a bus of its own, DICTATE_DIALER_BUS (its address), with a mock modem behind it
 * (apps/dialer/tests/mock-phone.py): no real call. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* as the Dialer's call-types.h has them */
typedef enum {
  DICTATE_CALL_CONNECTING = 0,
  DICTATE_CALL_DIALING,
  DICTATE_CALL_INCOMING,
  DICTATE_CALL_WAITING,         /* ringing while another call is up */
  DICTATE_CALL_ACTIVE,
  DICTATE_CALL_ONHOLD,
  DICTATE_CALL_DISCONNECTING,
  DICTATE_CALL_DISCONNECTED,
} DictateCallState;

typedef struct {
  guint id;
  char *number;                 /* "" when it came without one */
  guint presentation;           /* 1 the number came, 2 withheld, 3 not sent by the network, 4 a payphone */
  guint direction;              /* 1 incoming, 2 outgoing */
  DictateCallState state;
  gint64 connected;             /* when it was answered, Unix ms; 0 not yet */
} DictateCall;

#define DICTATE_CALLS_NAME      "vibeos.Dialer.Calls"
#define DICTATE_CALLS_PATH      "/vibeos/Dialer/Calls"
#define DICTATE_CALLS_INTERFACE "vibeos.Dialer.Calls"

/* the bus the call service is on */
GDBusConnection *dictate_calls_bus      (GCancellable *cancellable, GError **error);
/* the calls in the service's Calls property (aa{sv}), not yet over, oldest first: DictateCall */
GPtrArray       *dictate_calls_parse    (GVariant *calls);
void             dictate_call_free      (DictateCall *call);
gboolean         dictate_call_is_ringing (const DictateCall *call);
/* what goes on, for the model ("a call is ringing"), or NULL when no call is */
const char      *dictate_calls_context  (GPtrArray *calls);

/* "Call from %s. Answer or decline?" in `language`, `who` the contact's name, or NULL for a number not in the
 * contacts (`withheld`: a private number) */
char            *dictate_calls_announcement (const char *language, const char *who, gboolean withheld);
/* the same for the call ringing in `current`, get_current_call's result (dictate-tools-phone.c); NULL for none. A
 * result whose names were withheld (private mode on the speaker) says "one of your contacts" */
char            *dictate_calls_announce_text (const char *language, const char *current);

/* the quiet hours (quiet-hours, "HH:MM-HH:MM") are on now */
gboolean         dictate_calls_quiet_now (GSettings *settings);
/* whether Agent says a ringing call aloud: its window is open (`open`) or Announce calls is on; Agent may read
 * calls; and it is not quiet hours, unless calls are let through them */
gboolean         dictate_calls_may_announce (GSettings *settings, gboolean open);

/* --- watching: "ringing" (guint id) when a call starts ringing, "stopped" (guint id) when it stops (answered,
 * declined, missed), "changed" () when anything about the calls changed */
#define DICTATE_TYPE_CALL_WATCH (dictate_call_watch_get_type ())
G_DECLARE_FINAL_TYPE (DictateCallWatch, dictate_call_watch, DICTATE, CALL_WATCH, GObject)

DictateCallWatch *dictate_call_watch_new   (void);
/* the calls as last seen: DictateCall, owned by the watch */
GPtrArray        *dictate_call_watch_calls (DictateCallWatch *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DictateCall, dictate_call_free)

G_END_DECLS
