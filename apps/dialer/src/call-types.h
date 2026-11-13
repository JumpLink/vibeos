/* What a call is to the Dialer, shared by its service (neo-dialer-calls) and its app (neo-dialer).
 *
 * The service watches ModemManager's calls and turns them into the call states and end causes below, so that the app,
 * the notifications and the call log all read the same thing. They travel over D-Bus as plain numbers: these enums are
 * the one place their meanings are written down. */
#pragma once

/* the service: its name on the session bus, and its one object */
#define CALLS_BUS_NAME "neo.Dialer.Calls"
#define CALLS_PATH "/neo/Dialer/Calls"
#define CALLS_INTERFACE "neo.Dialer.Calls"

/* the apps: the Phone app with its tabs, and the in-call screen, which may show over the lock screen and so is an
 * app of its own */
#define DIALER_APP_ID "neo.Dialer"
#define INCALL_APP_ID "neo.Dialer.InCall"

/* a call's state, in the order the in-call screens care about */
typedef enum {
  CALL_STATE_CONNECTING = 0,    /* asked for, the modem not dialling yet: "Calling…" */
  CALL_STATE_DIALING,           /* ModemManager's DIALING and RINGING_OUT: still "Calling…" (no "alerting" state) */
  CALL_STATE_INCOMING,          /* RINGING_IN: the answer screen */
  CALL_STATE_CALL_WAITING,      /* WAITING: ringing while another call is up */
  CALL_STATE_ACTIVE,
  CALL_STATE_ONHOLD,
  CALL_STATE_DISCONNECTING,     /* hung up by us, not over yet: "Hanging up" */
  CALL_STATE_DISCONNECTED,      /* over; the service keeps it a moment so that "Call ended" can show */
} CallState;

/* why a call ended, which also says how long an ended call stays */
typedef enum {
  CALL_END_NONE = 0,
  CALL_END_LOCAL,               /* we hung up */
  CALL_END_REMOTE,              /* they did */
  CALL_END_MISSED,              /* an incoming call that stopped ringing unanswered */
  CALL_END_REJECTED,            /* an incoming call we declined */
  CALL_END_CANCELED,            /* an outgoing call we hung up before it was answered */
  CALL_END_BUSY,                /* refused or busy */
  CALL_END_ERROR,               /* the modem could not make it */
} CallEnd;

typedef enum {
  CALL_DIRECTION_UNKNOWN = 0,   /* ModemManager's MMCallDirection */
  CALL_DIRECTION_INCOMING = 1,
  CALL_DIRECTION_OUTGOING = 2,
} CallDirection;

/* CallLog.Calls.TYPE: the kind of a call in the call log */
typedef enum {
  CALL_TYPE_INCOMING = 1,
  CALL_TYPE_OUTGOING = 2,
  CALL_TYPE_MISSED = 3,
  CALL_TYPE_VOICEMAIL = 4,
  CALL_TYPE_REJECTED = 5,
  CALL_TYPE_BLOCKED = 6,
  CALL_TYPE_ANSWERED_EXTERNALLY = 7,
} CallType;

/* CallLog.Calls.PRESENTATION: whether the caller's number came with the call */
typedef enum {
  PRESENTATION_ALLOWED = 1,
  PRESENTATION_RESTRICTED = 2,  /* withheld: "Private number" */
  PRESENTATION_UNKNOWN = 3,     /* not sent by the network: "Unknown" */
  PRESENTATION_PAYPHONE = 4,
} Presentation;
