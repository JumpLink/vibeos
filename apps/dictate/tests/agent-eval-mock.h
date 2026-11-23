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

G_END_DECLS
