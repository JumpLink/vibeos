/* dictate-device: who this phone is to the Agent's gateway (the store's docs/api.md, section 17).
 *
 * The device id is the store's: the random UUID Software keeps in vibeos-software/device-id under the user's data
 * directory, made here the same way when Software has not run yet, so the phone is one device to the store and to
 * the gateway. The secret is the Agent's own: 32 random bytes as 64 hex digits in vibeos-dictate/agent-secret
 * (0600), made the first time. Neither is ever logged. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

const char *dictate_device_id      (void);
const char *dictate_device_secret  (void);
/* the hardware's name (the device tree's model, else DMI's), or NULL */
const char *dictate_device_model   (void);
/* VibeOS's release, as the app knows it (libvibeos): set once at start, NULL when nobody did */
void        dictate_device_set_release (const char *release);
const char *dictate_device_release (void);

G_END_DECLS
