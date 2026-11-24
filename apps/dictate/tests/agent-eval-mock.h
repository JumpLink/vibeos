/* agent-eval-mock: the private phone agent-eval runs Agent's tools against (see agent-eval-mock.c) */
#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* the fixtures' "phone": the stores written (calls.db, bugle_db, the EDS address book and calendar) */
void  mock_fixtures_write  (JsonObject *phone);
/* the services stood in for on the private bus, as the fixtures say (after mock_fixtures_write ()) */
void  mock_services_start  (void);
/* back to the fixtures' Wi-Fi and Bluetooth, between cases */
void  mock_services_reset  (void);
/* apps the shell was asked to start */
guint mock_launched        (void);

/* the Phone's call service (SERVICE, vibeos-dialer-calls) on the Dialer's mock phone (MOCK_PHONE, mock-phone.py),
 * for Agent's tools: FALSE when either is missing, and the call cases are skipped */
gboolean mock_calls_start  (const char *mock_phone, const char *service);
/* a case's call ({"from": NUMBER, "state": "ringing" | "active" | "held"}), the last one's ended first; NULL for
 * none. FALSE when it could not be set up */
gboolean mock_calls_set    (JsonObject *call);
/* the calls now: "ringing", "active", "held", "calling" or "none"; the microphone muted, the speaker on */
char    *mock_calls_state  (gboolean *muted, gboolean *speaker);

G_END_DECLS
