/* The alarms in the Clock's service: what alarm-service.c says at its top. */
#pragma once

#include <gio/gio.h>

typedef struct _AlarmService AlarmService;

/* serves the alarms on @session and rings them; NULL (and @error) if it cannot serve them. @system: for logind and
 * vibeos-wakeup, or NULL */
AlarmService *alarm_service_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings,
                                 const char *store_path, GError **error);
