/* sw-device: this device as the store knows it (the API's device object).
 *
 *   id               a random UUID made once and kept in Software's data directory
 *                    (~/.local/share/vibeos-software/device-id): the same for every account signed in here
 *   model            hostnamed's HardwareModel, else the device tree's model, else "Linux device"
 *   vibeos_version   libvibeos's vibeos_release ()
 *   arch             dpkg --print-architecture, else the architecture Software was built for
 *
 * The model and the architecture are asked for once, at start (sw_device_load); until they are in, the fallbacks
 * stand. */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* starts reading the model and the architecture; `callback` runs once both are known */
void        sw_device_load        (GCallback callback, gpointer data);
gboolean    sw_device_is_loaded   (void);

const char *sw_device_id          (void);
const char *sw_device_model       (void);
const char *sw_device_version     (void);
const char *sw_device_arch        (void);

/* { "id", "model", "vibeos_version", "arch" }; without the id for PUT /me/device */
JsonNode   *sw_device_to_json     (gboolean with_id);

/* For the tests: the id kept in `dir` (made there on first use), a device-tree model's text cleaned up (trailing NULs
 * and spaces gone), and Debian's name for the architecture this was built for */
char       *sw_device_id_in       (const char *dir);
char       *sw_device_clean_model (const char *raw, gsize length);
const char *sw_device_built_arch  (void);

G_END_DECLS
